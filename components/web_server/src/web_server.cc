// WebServer implementation: httpd lifecycle, URI registration, mDNS.
// REST/WS handlers live in rest_api.cc / ws_handler.cc.

#include "web_server/web_server.h"

#include <mdns.h>

#include <esp_log.h>

#include <cctype>
#include <cstdio>
#include <string>
#include <utility>

#include "rest_api.h"
#include "rest_api_internal.h"
#include "sys/system_info.h"
#include "web_assets/assets.h"
#include "ws_handler.h"

static const char* TAG = "WebServer";

namespace web_server {

namespace {

/// MIME by file extension.
const char* MimeFor(const char* name) {
    const std::string file(name);
    auto ends_with = [&file](const char* suffix) {
        const size_t len = strlen(suffix);
        return file.size() >= len && file.compare(file.size() - len, len, suffix) == 0;
    };
    if (ends_with(".html") || ends_with(".htm"))
        return "text/html; charset=utf-8";
    if (ends_with(".css"))
        return "text/css";
    if (ends_with(".js") || ends_with(".mjs"))
        return "text/javascript";
    if (ends_with(".json") || ends_with(".map"))
        return "application/json";
    if (ends_with(".svg"))
        return "image/svg+xml";
    if (ends_with(".png"))
        return "image/png";
    if (ends_with(".jpg") || ends_with(".jpeg"))
        return "image/jpeg";
    if (ends_with(".ico"))
        return "image/x-icon";
    if (ends_with(".woff2"))
        return "font/woff2";
    if (ends_with(".txt"))
        return "text/plain; charset=utf-8";
    return "application/octet-stream";
}

int HexDigit(char c) {
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}

/// Percent-decoding for the URL path ("%XX" only; queries stay raw).
std::string UrlDecode(const std::string& in) {
    std::string out;
    out.reserve(in.size());
    for (size_t i = 0; i < in.size(); i++) {
        if (in[i] == '%' && i + 2 < in.size()) {
            const int hi = HexDigit(in[i + 1]);
            const int lo = HexDigit(in[i + 2]);
            if (hi >= 0 && lo >= 0) {
                out.push_back(static_cast<char>((hi << 4) | lo));
                i += 2;
                continue;
            }
        }
        out.push_back(in[i]);
    }
    return out;
}

/// GET catch-all — static assets from the WAS1 image (gzip + ETag);
/// unknown non-API paths -> index.html (SPA), unknown /api/ -> JSON 404.
esp_err_t HandleAsset(httpd_req_t* req) {
    // Path without the query string (req->uri carries the full target).
    std::string path(req->uri);
    const size_t query = path.find('?');
    if (query != std::string::npos) {
        path.resize(query);
    }
    if (!path.empty() && path[0] == '/') {
        path.erase(0, 1);
    }
    if (path.empty()) {
        path = "index.html";  // "/"
    }
    path = UrlDecode(path);

    const WebAsset* asset = nullptr;
    if (path.find("..") == std::string::npos) {
        // Traversal guard (redundant: the asset table is fixed).
        asset = FindAsset(path.c_str());
    }
    if (asset == nullptr) {
        if (path.compare(0, 4, "api/") == 0) {
            httpd_resp_set_status(req, "404 Not Found");
            httpd_resp_set_type(req, "application/json");
            httpd_resp_set_hdr(req, "Cache-Control", "no-store");
            return httpd_resp_sendstr(
                req, "{\"success\":false,\"error\":{\"code\":\"not_found\",\"message\":"
                     "\"Unknown path\"}}");
        }
        asset = FindAsset("index.html");  // SPA fallback
    }
    if (asset == nullptr) {
        // Recovery page: no valid asset store — the only case it serves.
        asset = GetAssetFallbackPage();
    }
    if (asset == nullptr) {
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "No asset embedded");
        return ESP_OK;
    }

    // ETag from the asset name (quoted per RFC 7232).
    char etag[80];
    snprintf(etag, sizeof(etag), "\"%.64s\"", asset->name);
    httpd_resp_set_type(req, MimeFor(asset->name));
    httpd_resp_set_hdr(req, "Content-Encoding", "gzip");
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
    httpd_resp_set_hdr(req, "ETag", etag);
    return httpd_resp_send(req, reinterpret_cast<const char*>(asset->data),
                           static_cast<ssize_t>(asset->size));
}

}  // namespace

bool WebServer::Start(uint16_t port) {
    if (server_ != nullptr) {
        return true;  // idempotent — already running
    }

    // Map the asset image BEFORE httpd starts (FindAsset serves from mmap).
    const esp_err_t asset_err = WebAssetsInit();
    if (asset_err != ESP_OK) {
        ESP_LOGW(TAG, "Asset store unavailable (%s) — the fallback page will serve",
                 esp_err_to_name(asset_err));
    }

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = port;
    // 31 /api routes + /ws + the static wildcard = 33 of 33 — at the httpd
    // handler cap, no spare; the 30 s socket timeouts cover the waits.
    config.max_uri_handlers = 33;
    config.stack_size = 8192;  // JSON building inside the handlers
    config.max_open_sockets = ws::kMaxOpenSockets;
    config.lru_purge_enable = true;
    config.recv_wait_timeout = 30;
    config.send_wait_timeout = 30;
    // Enables the trailing-wildcard sub-routes + the asset catch-all.
    config.uri_match_fn = httpd_uri_match_wildcard;

    if (httpd_start(&server_, &config) != ESP_OK) {
        ESP_LOGE(TAG, "httpd_start failed (port %u)", static_cast<unsigned>(port));
        server_ = nullptr;
        return false;
    }
    port_ = port;
    RegisterUris();
    InitMdns();
    ESP_LOGI(TAG, "Web server started on port %u", static_cast<unsigned>(port));
    return true;
}

void WebServer::Stop() {
    if (server_ == nullptr) {
        return;
    }
    // Null first so a concurrent Broadcast sees the shutdown.
    httpd_handle_t server = server_;
    server_ = nullptr;
    httpd_stop(server);
    ws::ClearClients();
    ESP_LOGI(TAG, "Web server stopped");
}

void WebServer::SetScheduler(Scheduler scheduler) {
    rest_api::SetScheduler(std::move(scheduler));
}

void WebServer::SetCriticalScheduler(Scheduler scheduler) {
    rest_api::SetCriticalScheduler(std::move(scheduler));
}

void WebServer::SetDeviceState(DeviceState state) {
    rest_api::SetDeviceState(state);
}

void WebServer::SetRoomRefreshTrigger(std::function<bool()> trigger) {
    rest_api::SetRoomRefreshTrigger(std::move(trigger));
}

void WebServer::BroadcastStatus() {
    ws::Broadcast(server_, "status", rest_api::BuildStatusObject());
}

void WebServer::BroadcastAuthStatus(const tuya_cloud::AuthStatus& status) {
    ws::Broadcast(server_, "auth_status", rest_api::BuildAuthStatusObject(status));
}

void WebServer::BroadcastDiscoveryProgress(const hub_core::DiscoveryProgress& progress) {
    ws::Broadcast(server_, "discovery_progress", rest_api::BuildDiscoveryProgressObject(progress));
}

void WebServer::BroadcastMqttStatus(const tuya_mqtt::MqttStatus& status) {
    ws::Broadcast(server_, "mqtt_status", rest_api::BuildMqttStatusObject(status));
}

void WebServer::BroadcastRegistryEvent(const hub_core::RegistryEventPayload& event) {
    const char* name = rest_api::RegistryEventName(event.event);
    if (name == nullptr) {
        return;  // kHomesUpdated / kDevicesCleared — not in the WS contract
    }
    ws::Broadcast(server_, name, rest_api::BuildRegistryEventData(event));
}

void WebServer::RegisterUris() {
    // Order matters: the catch-all wildcard must be registered LAST or
    // httpd rejects every specific URI that matches it.
    rest_api::RegisterHandlers(server_);
    ws::RegisterHandler(server_);

    httpd_uri_t uri_assets = {};
    uri_assets.uri = "/*";
    uri_assets.method = HTTP_GET;
    uri_assets.handler = &HandleAsset;
    uri_assets.user_ctx = nullptr;
    esp_err_t err = httpd_register_uri_handler(server_, &uri_assets);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "register static wildcard: %s", esp_err_to_name(err));
    }
}

void WebServer::InitMdns() {
    if (mdns_initialized_) {
        return;  // one-shot: a Stop/Start cycle keeps the announcement
    }
    mdns_initialized_ = true;

    // Hostname "tuyahub-<mac4>"; mDNS hostnames are lowercase.
    std::string mac = sys::SystemInfo::GetMacAddress();  // "AA:BB:CC:DD:EE:FF"
    std::string code;
    for (char c : mac) {
        if (c != ':') {
            code.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
        }
    }
    if (code.size() > 4) {
        code = code.substr(code.size() - 4);  // last 4 hex digits, no colons
    }
    const std::string hostname = "tuyahub-" + code;  // e.g. "tuyahub-1a2b"

    if (mdns_init() != ESP_OK) {
        ESP_LOGW(TAG, "mdns_init failed — reach the UI via the device IP");
        return;
    }
    mdns_hostname_set(hostname.c_str());
    mdns_instance_name_set("Tuya Hub");
    mdns_service_add(nullptr, "_http", "_tcp", port_, nullptr, 0);
    ESP_LOGI(TAG, "mDNS: http://%s.local", hostname.c_str());
}

}  // namespace web_server
