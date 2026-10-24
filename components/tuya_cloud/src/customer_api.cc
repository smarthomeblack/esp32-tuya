#include "tuya_cloud/customer_api.h"

#include <esp_log.h>
#include <esp_random.h>
#include <esp_timer.h>

#include <cJSON.h>

#include <cstdint>
#include <string>
#include <vector>

#include "tuya_cloud/constants.h"
#include "tuya_cloud/time_sync.h"
#include "tuya_crypto/aes_gcm.h"
#include "tuya_crypto/hmac_sha256.h"
#include "tuya_crypto/json_fields.h"
#include "tuya_crypto/json_guard.h"
#include "tuya_crypto/json_writer.h"
#include "tuya_crypto/md5.h"
#include "tuya_crypto/uuid.h"

#include "signing.h"

static const char* TAG = "CustomerApi";

namespace tuya_cloud {

namespace {

// Tuya 48-char nonce alphabet (excludes I L O U V g l o q u v 0 1 9).
constexpr char kNonceAlphabet[] = "ABCDEFGHJKMNPQRSTWXYZabcdefhijkmnprstwxyz2345678";
constexpr size_t kNonceAlphabetSize = sizeof(kNonceAlphabet) - 1;
static_assert(kNonceAlphabetSize == 48, "Tuya nonce alphabet must be 48 chars");
constexpr size_t kNonceLen = 12;

// Sign header order.
constexpr const char* kSignHeaders[] = {"X-appKey", "X-requestId", "X-sid", "X-time", "X-token"};
constexpr size_t kSignHeaderCount = sizeof(kSignHeaders) / sizeof(kSignHeaders[0]);

/// 12 random chars from the Tuya alphabet; public per-message value, not a secret.
std::string RandomNonce12() {
    uint8_t raw[kNonceLen];
    esp_fill_random(raw, sizeof(raw));
    std::string nonce;
    nonce.reserve(kNonceLen);
    for (size_t i = 0; i < kNonceLen; ++i) {
        nonce.push_back(kNonceAlphabet[raw[i] % kNonceAlphabetSize]);
    }
    return nonce;
}

/// Request key material: hash_key feeds the final HMAC; secret (16 hex
/// chars as raw AES-128 bytes) encrypts this request's encdata, response included.
struct DerivedSecrets {
    std::string hash_key;
    std::string secret;
};

DerivedSecrets DeriveSecrets(const std::string& rid, const std::string& refresh_token) {
    DerivedSecrets out;
    if (!tuya_crypto::Md5Hex(rid + refresh_token, out.hash_key)) {
        return out;
    }
    std::string hmac_hex;
    if (!tuya_crypto::HmacSha256Hex(rid, out.hash_key, hmac_hex)) {
        out.hash_key.clear();
        return out;
    }
    out.secret = hmac_hex.substr(0, 16);
    return out;
}

std::string TruncateForLog(const std::string& s, size_t max_chars) {
    if (s.size() <= max_chars) {
        return s;
    }
    return s.substr(0, max_chars) + "...(" + std::to_string(s.size()) + "B)";
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

}  // namespace

int64_t StartCommandDeadline() {
    return esp_timer_get_time() / 1000 + kCloudCommandDeadlineMs;
}

bool DeadlineExceeded(int64_t deadline_ms) {
    // esp_timer ms (monotonic): a wall-clock deadline would jump with SNTP steps.
    return deadline_ms != 0 && esp_timer_get_time() / 1000 >= deadline_ms;
}

std::string PercentEncode(std::string_view value) {
    static constexpr char kHex[] = "0123456789ABCDEF";
    std::string out;
    out.reserve(value.size());
    for (const char ch : value) {
        const unsigned char c = static_cast<unsigned char>(ch);
        const bool unreserved = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                                (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' ||
                                c == '~';
        if (unreserved) {
            out.push_back(static_cast<char>(c));
        } else {
            out.push_back('%');
            out.push_back(kHex[c >> 4]);
            out.push_back(kHex[c & 0x0F]);
        }
    }
    return out;
}

BuiltRequest BuildSignedRequest(const std::string& endpoint, std::string_view path,
                                const std::string& rid, const std::string& access_token,
                                const std::string& refresh_token, int64_t time_ms,
                                const std::string& nonce12, const std::string& params_json,
                                const std::string& body_json) {
    BuiltRequest out;
    const DerivedSecrets secrets = DeriveSecrets(rid, refresh_token);

    // Plaintext -> encdata; when both are present the body gets an
    // independent nonce (IV reuse under one key is fatal).
    std::string query_encdata;
    if (!params_json.empty()) {
        if (!tuya_crypto::AesGcmEncryptPackWithNonce(secrets.secret, nonce12, params_json,
                                                     query_encdata)) {
            ESP_LOGE(TAG, "query encdata pack failed");
        }
    }
    std::string body_encdata;
    if (!body_json.empty()) {
        if (params_json.empty()) {
            if (!tuya_crypto::AesGcmEncryptPackWithNonce(secrets.secret, nonce12, body_json,
                                                         body_encdata)) {
                ESP_LOGE(TAG, "body encdata pack failed");
            }
        } else {
            if (!tuya_crypto::AesGcmEncryptPack(secrets.secret, body_json, body_encdata)) {
                ESP_LOGE(TAG, "body encdata pack failed");
            }
        }
    }

    // X-sid is always "" — sent as a header but skipped by the sign because it is empty.
    out.app_key = std::string(kClientId);
    out.request_id = rid;
    out.sid = "";
    out.time = std::to_string(time_ms);
    out.token = access_token;

    // Sign input: "h=v" pairs joined by "||" (empty values skipped, trailing
    // "||" stripped); encdata appended with NO separator.
    std::string sign_input;
    sign_input.reserve(256);
    const std::string values[kSignHeaderCount] = {out.app_key, out.request_id, out.sid, out.time,
                                                  out.token};
    for (size_t i = 0; i < kSignHeaderCount; ++i) {
        if (!values[i].empty()) {
            sign_input += kSignHeaders[i];
            sign_input += '=';
            sign_input += values[i];
            sign_input += "||";
        }
    }
    if (sign_input.size() >= 2 && sign_input.compare(sign_input.size() - 2, 2, "||") == 0) {
        sign_input.resize(sign_input.size() - 2);
    }
    sign_input += query_encdata;
    sign_input += body_encdata;
    if (!tuya_crypto::HmacSha256Hex(secrets.hash_key, sign_input, out.sign)) {
        ESP_LOGE(TAG, "sign HMAC failed");
    }

    // url = endpoint + path [+ ?encdata=...].
    out.url = endpoint;
    out.url += path;
    if (!query_encdata.empty()) {
        out.url += "?encdata=";
        out.url += PercentEncode(query_encdata);
    }

    // body = {"encdata": "..."} (compact JSON).
    if (!body_encdata.empty()) {
        tuya_crypto::JsonWriter writer;
        writer.BeginObject();
        writer.Key("encdata");
        writer.String(body_encdata);
        writer.EndObject();
        if (writer.HasError()) {
            ESP_LOGE(TAG, "body JsonWriter failed");
            out.body.clear();
        } else {
            out.body = writer.Str();
        }
    }
    return out;
}

void CustomerApi::Configure(const std::string& endpoint, const TokenInfo& token) {
    {
        std::lock_guard<std::mutex> lock(endpoint_mutex_);
        endpoint_ = endpoint;
    }
    auth_failed_ = false;
    token_manager_.SetToken(token);
    ESP_LOGI(TAG, "Configured: endpoint=%s uid=%s expire_ms=%lld", endpoint.c_str(),
             token.uid.c_str(), static_cast<long long>(token.ExpireMs()));
}

bool CustomerApi::IsConfigured() const {
    std::lock_guard<std::mutex> lock(endpoint_mutex_);
    return !endpoint_.empty() && !token_manager_.GetToken().refresh_token.empty();
}

void CustomerApi::Reset() {
    // Fires the token listener with an EMPTY token (the persistence listener ignores it).
    token_manager_.SetToken(TokenInfo{});
    {
        std::lock_guard<std::mutex> lock(endpoint_mutex_);
        endpoint_.clear();
    }
    auth_failed_ = false;
    ESP_LOGI(TAG, "Reset: endpoint/token/auth-failed cleared (API unconfigured)");
}

void CustomerApi::SetTokenListener(TokenListener listener) {
    token_manager_.SetListener(std::move(listener));
}

void CustomerApi::SetOnAuthFailed(AuthFailedCallback callback) {
    on_auth_failed_ = std::move(callback);
}

ApiResponse CustomerApi::Request(HttpMethod method, std::string_view path,
                                 const std::string& params_json, const std::string& body_json,
                                 int64_t deadline_ms) {
    // Serializes the whole pipeline: one request in flight at a time; the
    // refresh/execute stages run inside without re-locking (see the header).
    std::lock_guard<std::mutex> request_lock(request_mutex_);
    if (!IsConfigured()) {
        ESP_LOGE(TAG, "Request(%s) before Configure() — rejecting", MethodName(method));
        ApiResponse resp;
        resp.ok = false;
        resp.msg = "not configured";
        return resp;
    }
    RefreshTokenIfNeeded(deadline_ms);
    return ExecuteRequest(method, path, params_json, body_json, deadline_ms);
}

void CustomerApi::RefreshTokenIfNeeded(int64_t deadline_ms) {
    // Single-flight: the mutex serializes refresh attempts, the IsRefreshing guard stops re-entry.
    TokenInfo fresh;
    bool token_refreshed = false;
    {
        std::lock_guard<std::mutex> lock(refresh_mutex_);
        if (token_manager_.IsRefreshing()) {
            return;
        }
        if (!token_manager_.NeedsRefresh(TimeSync::NowMs())) {
            return;
        }

        token_manager_.SetRefreshing(true);
        struct ResetGuard {
            TokenManager& manager;
            ~ResetGuard() { manager.SetRefreshing(false); }  // finally semantics
        } reset_guard{token_manager_};

        const TokenInfo old_token = token_manager_.GetToken();
        ESP_LOGI(TAG, "Token near expiry (expire_ms=%lld, now=%lld) — refreshing",
                 static_cast<long long>(old_token.ExpireMs()),
                 static_cast<long long>(TimeSync::NowMs()));

        // Spec: GET /v1.0/m/token/{refresh_token} with X-token = OLD access token;
        // routed through ExecuteRequest so the nested call skips this section.
        const std::string path = std::string(kPathTokenRefresh) + old_token.refresh_token;
        ApiResponse resp = ExecuteRequest(HttpMethod::kGet, path, "", "", deadline_ms);

        if (!resp.ok || resp.result == nullptr || !cJSON_IsObject(resp.result)) {
            // Swallowed; the pending request proceeds with the old token.
            ESP_LOGE(TAG, "Token refresh failed: %s", resp.msg.c_str());
            return;
        }

        // Result fields: expireTime (s), uid, accessToken, refreshToken; top-level t (ms).
        fresh.t = resp.t;
        fresh.expire_time = tuya_crypto::GetInt64Field(resp.result, "expireTime");
        fresh.uid = tuya_crypto::GetStringField(resp.result, "uid");
        fresh.access_token = tuya_crypto::GetStringField(resp.result, "accessToken");
        fresh.refresh_token = tuya_crypto::GetStringField(resp.result, "refreshToken");

        if (fresh.t <= 0 || fresh.expire_time <= 0 || fresh.access_token.empty() ||
            fresh.refresh_token.empty()) {
            ESP_LOGE(TAG, "Token refresh returned an unusable token — keeping the old one");
            return;
        }

        token_refreshed = true;
        ESP_LOGI(TAG, "Token refreshed: uid=%s expires in %llds (t=%lld)", fresh.uid.c_str(),
                 static_cast<long long>(fresh.expire_time), static_cast<long long>(fresh.t));
    }

    if (token_refreshed) {
        // Propagate OUTSIDE refresh_mutex_ so the listener can call back into Request().
        token_manager_.SetToken(fresh);
    }
}

ApiResponse CustomerApi::ExecuteRequest(HttpMethod method, std::string_view path,
                                        const std::string& params_json,
                                        const std::string& body_json, int64_t deadline_ms) {
    // Redact only a bare token suffix; subpaths (terminal revoke) log verbatim.
    const std::string_view suffix = path.starts_with(kPathTokenRefresh)
                                        ? path.substr(kPathTokenRefresh.size())
                                        : std::string_view();
    std::string log_path = !suffix.empty() && suffix.find('/') == std::string_view::npos
                               ? std::string(kPathTokenRefresh) + "<redacted>"
                               : std::string(path);
    // The ?encdata= query is a credential — log the path only.
    const size_t query_pos = log_path.find('?');
    if (query_pos != std::string::npos) {
        log_path.resize(query_pos);
    }
    if (DeadlineExceeded(deadline_ms)) {
        ESP_LOGW(TAG, "chain deadline exceeded — skipping %s %s", MethodName(method),
                 log_path.c_str());
        ApiResponse resp;
        resp.msg = "deadline exceeded";
        return resp;
    }
    const TokenInfo token = token_manager_.GetToken();
    // Snapshot the endpoint under its own lock. Lock order: request_mutex_ -> endpoint_mutex_.
    std::string endpoint_snapshot;
    {
        std::lock_guard<std::mutex> lock(endpoint_mutex_);
        endpoint_snapshot = endpoint_;
    }

    // Steps 1-8 above with fresh randomness and wall clock.
    const std::string rid = tuya_crypto::Uuid4();
    const std::string nonce = RandomNonce12();
    const int64_t time_ms = TimeSync::NowMs();
    const BuiltRequest req =
        BuildSignedRequest(endpoint_snapshot, path, rid, token.access_token, token.refresh_token,
                           time_ms, nonce, params_json, body_json);

    // A signing failure must never reach the network; req.sign empty covers
    // the no-encdata paths (e.g. the token-refresh GET).
    if (req.sign.empty() ||
        (!params_json.empty() && req.url.find("?encdata=") == std::string::npos) ||
        (!body_json.empty() && req.body.empty())) {
        ESP_LOGE(TAG, "pipeline build failed (sign/encdata missing) — aborting request");
        ApiResponse resp;
        resp.msg = "sign/encdata build failed";
        return resp;
    }

    // The response is decrypted with THIS request's secret.
    const DerivedSecrets secrets = DeriveSecrets(rid, token.refresh_token);

    std::vector<HttpClient::Header> headers;
    headers.reserve(6);
    headers.push_back({"X-appKey", req.app_key});
    headers.push_back({"X-requestId", req.request_id});
    headers.push_back({"X-sid", req.sid});  // always sent, even empty
    headers.push_back({"X-time", req.time});
    headers.push_back({"X-sign", req.sign});
    if (!req.token.empty()) {
        headers.push_back({"X-token", req.token});
    }

    const int64_t start_us = esp_timer_get_time();
    const HttpClient::Result http = http_client_.Request(req.url, method, headers, req.body);
    const int64_t elapsed_ms = (esp_timer_get_time() - start_us) / 1000;

    if (!http.network_ok || http.status_code < 200 || http.status_code >= 300) {
        // Redacted path in the log — never tokens/sign.
        ESP_LOGE(TAG, "%s %s failed: net_ok=%d status=%d (%u ms) body=%.200s", MethodName(method),
                 log_path.c_str(), http.network_ok ? 1 : 0, http.status_code,
                 static_cast<unsigned>(elapsed_ms), http.body.c_str());
        ApiResponse resp;
        resp.msg =
            "http " + std::to_string(http.status_code) + ": " + TruncateForLog(http.body, 200);
        return resp;
    }

    ESP_LOGI(TAG, "%s %s -> %d (%u ms, %u B)", MethodName(method), log_path.c_str(),
             http.status_code, static_cast<unsigned>(elapsed_ms),
             static_cast<unsigned>(http.body.size()));

    // ---- Response handling ----
    ApiResponse resp;
    if (!tuya_crypto::JsonNestingWithinLimit(http.body.c_str(), http.body.size(),
                                             tuya_crypto::kMaxJsonDepth)) {
        ESP_LOGE(TAG, "response body nesting over %d (%u B)", tuya_crypto::kMaxJsonDepth,
                 static_cast<unsigned>(http.body.size()));
        resp.msg = "bad_json";
        return resp;
    }
    cJSON* root = cJSON_ParseWithLength(http.body.c_str(), http.body.size());
    if (root == nullptr) {
        ESP_LOGE(TAG, "response body is not valid JSON (%u B)",
                 static_cast<unsigned>(http.body.size()));
        resp.msg = "bad_json";
        return resp;
    }
    // RAII; the handed-out `result` is re-parsed from the plaintext, never a subtree of root.
    tuya_crypto::JsonRoot root_guard{root};

    const cJSON* success_item = cJSON_GetObjectItemCaseSensitive(root, "success");
    resp.success = cJSON_IsTrue(success_item) ? true : false;
    resp.code = tuya_crypto::GetInt64Field(root, "code");
    resp.t = tuya_crypto::GetInt64Field(root, "t");
    resp.msg = tuya_crypto::GetStringField(root, "msg");

    if (!resp.success) {
        // "sign invalid" (case-sensitive substring) -> fire the re-auth callback.
        if (resp.msg.find("sign invalid") != std::string::npos) {
            ESP_LOGE(TAG, "auth failure (sign invalid) — re-authentication required");
            auth_failed_ = true;
            if (on_auth_failed_) {
                on_auth_failed_();
            }
        } else {
            ESP_LOGW(TAG, "api error: code=%lld msg=%s", static_cast<long long>(resp.code),
                     resp.msg.c_str());
        }
        return resp;
    }

    resp.ok = true;

    // result: encrypted string, or missing/null for empty results.
    const cJSON* result_item = cJSON_GetObjectItemCaseSensitive(root, "result");
    if (result_item == nullptr || cJSON_IsNull(result_item)) {
        return resp;  // ok=true, result=nullptr
    }
    if (!cJSON_IsString(result_item) || result_item->valuestring == nullptr) {
        ESP_LOGE(TAG, "unexpected result type (not a string)");
        resp.ok = false;
        resp.msg = "unexpected result type";
        return resp;
    }
    std::string plaintext;
    if (!tuya_crypto::AesGcmDecryptPack(secrets.secret, result_item->valuestring, plaintext)) {
        ESP_LOGE(TAG, "result decryption failed (wrong key or corrupted data)");
        resp.ok = false;
        resp.msg = "result decrypt failed";
        return resp;
    }
    // JSON-parse the plaintext; a non-JSON body yields result=nullptr.
    if (!tuya_crypto::JsonNestingWithinLimit(plaintext.c_str(), plaintext.size(),
                                             tuya_crypto::kMaxJsonDepth)) {
        ESP_LOGE(TAG, "result nesting over %d — request treated as failed",
                 tuya_crypto::kMaxJsonDepth);
        resp.ok = false;
        resp.msg = "bad_json";
        return resp;
    }
    cJSON* parsed = cJSON_ParseWithLength(plaintext.c_str(), plaintext.size());
    if (parsed != nullptr) {
        resp.result = parsed;  // ApiResponse owns it
    }
    return resp;
}

}  // namespace tuya_cloud
