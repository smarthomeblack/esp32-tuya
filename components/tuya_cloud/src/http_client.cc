#include "tuya_cloud/http_client.h"

#include <esp_crt_bundle.h>
#include <esp_http_client.h>
#include <esp_log.h>
#include <esp_timer.h>

#include <strings.h>

#include <atomic>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "tuya_cloud/constants.h"

static const char* TAG = "HttpClient";

namespace tuya_cloud {

namespace {

/// Hard cap on the buffered body (fail fast on bogus Content-Length).
constexpr size_t kMaxResponseBytes = 512 * 1024;

// Persistent per-host handles: each esp_http_client_init+connect pays a
// 50-150ms TCP+TLS handshake; pooling lets a request burst reuse one session.
constexpr size_t kMaxPooledHosts = 4;

// Idle gap after which the session is dropped proactively: a server-side
// keep-alive close must not surface as a failed next request.
constexpr int64_t kIdleReconnectMs = 5000;

esp_http_client_method_t ToEspMethod(HttpMethod method) {
    switch (method) {
        case HttpMethod::kGet: return HTTP_METHOD_GET;
        case HttpMethod::kPost: return HTTP_METHOD_POST;
        case HttpMethod::kPut: return HTTP_METHOD_PUT;
        case HttpMethod::kDelete: return HTTP_METHOD_DELETE;
    }
    return HTTP_METHOD_GET;  // unreachable, silences -Wreturn-type paranoia
}

const char* MethodName(HttpMethod method) {
    switch (method) {
        case HttpMethod::kGet: return "GET";
        case HttpMethod::kPost: return "POST";
        case HttpMethod::kPut: return "PUT";
        case HttpMethod::kDelete: return "DELETE";
    }
    return "?";
}

/// Body accumulator handed to the event handler for one request.
struct RequestContext {
    std::string body;
    bool oversized = false;
};

esp_err_t OnHttpEvent(esp_http_client_event_t* evt) {
    auto* ctx = static_cast<RequestContext*>(evt->user_data);
    if (ctx == nullptr) {
        return ESP_OK;
    }
    switch (evt->event_id) {
        case HTTP_EVENT_ON_HEADERS_COMPLETE:
            if (esp_http_client_get_content_length(evt->client) >
                static_cast<int64_t>(kMaxResponseBytes)) {
                ctx->oversized = true;
            }
            break;
        case HTTP_EVENT_ON_DATA:
            if (!ctx->oversized) {
                if (ctx->body.size() + static_cast<size_t>(evt->data_len) > kMaxResponseBytes) {
                    ctx->oversized = true;
                } else {
                    ctx->body.append(static_cast<const char*>(evt->data),
                                     static_cast<size_t>(evt->data_len));
                }
            }
            break;
        default: break;
    }
    return ESP_OK;
}

/// One reusable connection to a single scheme://host:port.
struct HostConnection {
    /// Serializes requests on this host; perform() runs fully under it.
    std::mutex mutex;
    esp_http_client_handle_t client = nullptr;
    std::atomic<int64_t> last_used_ms{0};
    ~HostConnection() {
        if (client != nullptr) {
            esp_http_client_cleanup(client);
        }
    }
};

using HostConnectionPtr = std::shared_ptr<HostConnection>;

std::mutex& PoolMutex() {
    static std::mutex mutex;
    return mutex;
}

std::map<std::string, HostConnectionPtr>& Pool() {
    static std::map<std::string, HostConnectionPtr> pool;
    return pool;
}

int64_t NowMs() {
    return esp_timer_get_time() / 1000;
}

/// Connection identity: everything before the path ("scheme://host[:port]").
/// Empty when the URL has no parseable authority.
std::string PoolKey(const std::string& url) {
    const size_t scheme_end = url.find("://");
    if (scheme_end == std::string::npos || scheme_end == 0) {
        return "";
    }
    const size_t auth_start = scheme_end + 3;
    const size_t auth_end = url.find_first_of("/?#", auth_start);
    const size_t auth_stop = auth_end == std::string::npos ? url.size() : auth_end;
    if (auth_stop <= auth_start) {
        return "";
    }
    return url.substr(0, auth_stop);
}

HostConnectionPtr AcquireConnection(const std::string& key) {
    std::lock_guard<std::mutex> lock(PoolMutex());
    std::map<std::string, HostConnectionPtr>& pool = Pool();
    auto it = pool.find(key);
    if (it == pool.end()) {
        if (pool.size() >= kMaxPooledHosts) {
            // Evict least-recently-used; an in-flight request keeps its entry
            // alive (shared_ptr) until it finishes.
            auto victim = pool.begin();
            for (auto entry = pool.begin(); entry != pool.end(); ++entry) {
                if (entry->second->last_used_ms.load() < victim->second->last_used_ms.load()) {
                    victim = entry;
                }
            }
            pool.erase(victim);
        }
        it = pool.emplace(key, std::make_shared<HostConnection>()).first;
    }
    return it->second;
}

/// Destroys the handle (caller holds conn->mutex); events fired during
/// cleanup must not see a dead request context, hence the user_data clear.
void ResetConnection(const HostConnectionPtr& conn) {
    if (conn->client != nullptr) {
        esp_http_client_set_user_data(conn->client, nullptr);
        esp_http_client_cleanup(conn->client);
        conn->client = nullptr;
    }
}

/// Clears per-request state so the pooled handle is pristine between requests.
void ResetRequestState(esp_http_client_handle_t client,
                       const std::vector<const char*>& header_names) {
    for (const char* name : header_names) {
        // Host/User-Agent belong to esp_http_client (set at init/set_url).
        if (strcasecmp(name, "Host") == 0 || strcasecmp(name, "User-Agent") == 0) {
            continue;
        }
        esp_http_client_delete_header(client, name);
    }
    // Drops the post-field pointer (it dangles into the caller's body) and
    // the Content-Type header; NOT_FOUND from an absent header is fine.
    (void)esp_http_client_set_post_field(client, nullptr, 0);
}

}  // namespace

HttpClient::Result HttpClient::Request(const std::string& url, HttpMethod method,
                                       const std::vector<Header>& headers,
                                       const std::string& body) {
    Result result;

    const bool body_method = method == HttpMethod::kPost || method == HttpMethod::kPut;
    const bool send_body = !body.empty() && body_method;
    if (!body.empty() && !body_method) {
        // A GET/DELETE body would corrupt the request line — drop it loudly.
        ESP_LOGW(TAG, "body with %s dropped (%u bytes)", MethodName(method),
                 static_cast<unsigned>(body.size()));
    }

    // The URL embeds secrets (token in path, usercode in query); log the key, not the URL.
    const std::string key = PoolKey(url);
    if (key.empty()) {
        ESP_LOGE(TAG, "unparseable URL for %s — URL not logged", MethodName(method));
        return result;
    }

    const HostConnectionPtr conn = AcquireConnection(key);
    std::lock_guard<std::mutex> host_lock(conn->mutex);

    if (conn->client != nullptr && NowMs() - conn->last_used_ms.load() > kIdleReconnectMs) {
        ResetConnection(conn);
    }

    if (conn->client == nullptr) {
        // buffer_size_tx must cover a long ?encdata=... query beyond the 512-byte default.
        esp_http_client_config_t config = {};
        config.url = url.c_str();
        config.method = ToEspMethod(method);
        config.timeout_ms = kHttpTimeoutMs;
        config.crt_bundle_attach = esp_crt_bundle_attach;
        config.buffer_size = 1024;
        config.buffer_size_tx = 2048;
        config.keep_alive_enable = true;
        config.disable_auto_redirect = true;  // 3xx must surface as non-2xx
        config.event_handler = OnHttpEvent;
        conn->client = esp_http_client_init(&config);
        if (conn->client == nullptr) {
            ESP_LOGE(TAG, "esp_http_client_init failed for %s %s", MethodName(method), key.c_str());
            return result;
        }
    }
    esp_http_client_handle_t client = conn->client;

    RequestContext ctx;
    std::vector<const char*> set_header_names;

    if (esp_http_client_set_user_data(client, &ctx) != ESP_OK ||
        esp_http_client_set_url(client, url.c_str()) != ESP_OK ||
        esp_http_client_set_method(client, ToEspMethod(method)) != ESP_OK) {
        ESP_LOGE(TAG, "request setup failed for %s %s", MethodName(method), key.c_str());
        ResetConnection(conn);
        return result;
    }

    for (const Header& header : headers) {
        // Empty values are sent as-is (X-sid).
        if (esp_http_client_set_header(client, header.name.c_str(), header.value.c_str()) !=
            ESP_OK) {
            ESP_LOGE(TAG, "set_header(%s) failed", header.name.c_str());
            ResetConnection(conn);
            return result;
        }
        set_header_names.push_back(header.name.c_str());
    }

    if (send_body) {
        // Set before set_post_field so it is not replaced by the x-www-form-urlencoded default.
        (void)esp_http_client_set_header(client, "Content-Type", "application/json");
        if (esp_http_client_set_post_field(client, body.data(), static_cast<int>(body.size())) !=
            ESP_OK) {
            ESP_LOGE(TAG, "set_post_field failed (%u bytes)", static_cast<unsigned>(body.size()));
            ResetConnection(conn);
            return result;
        }
    } else {
        // Clears a stale post field + Content-Type left by a previous request.
        (void)esp_http_client_set_post_field(client, nullptr, 0);
    }

    const esp_err_t err = esp_http_client_perform(client);
    // user_data points at ctx only while perform runs; events fired by the
    // close/cleanup calls below must not dereference it.
    (void)esp_http_client_set_user_data(client, nullptr);
    bool transport_error = err != ESP_OK;

    if (err == ESP_ERR_NOT_SUPPORTED) {
        // 401 (no credentials): perform stops before draining; the arrived
        // body is in ctx.body. read() is unsafe here (raw_len>0, raw_data NULL).
        if (esp_http_client_is_complete_data_received(client)) {
            transport_error = false;
            // perform returned before its end-of-response state fixup; close
            // (state -> INIT, tls destroyed) so the next request reconnects.
            (void)esp_http_client_close(client);
        }
    }

    result.status_code = esp_http_client_get_status_code(client);
    if (result.status_code < 0) {
        result.status_code = 0;  // -1 = status line never parsed
    }

    if (transport_error) {
        // Body (partial on failure) — callers surface it in error logs.
        result.body = std::move(ctx.body);
        ESP_LOGE(TAG, "%s %s: request failed (err=0x%x)", MethodName(method), key.c_str(), err);
        ResetConnection(conn);
        return result;
    }

    result.body = std::move(ctx.body);
    if (ctx.oversized) {
        ESP_LOGE(TAG, "response exceeds %u bytes", static_cast<unsigned>(kMaxResponseBytes));
    } else {
        result.network_ok = true;
    }

    ResetRequestState(client, set_header_names);
    conn->last_used_ms.store(NowMs());
    ESP_LOGD(TAG, "%s %s -> %d (%u bytes)", MethodName(method), key.c_str(), result.status_code,
             static_cast<unsigned>(result.body.size()));
    return result;
}

}  // namespace tuya_cloud
