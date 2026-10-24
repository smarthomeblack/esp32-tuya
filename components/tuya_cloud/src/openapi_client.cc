#include "tuya_cloud/openapi_client.h"

#include <esp_log.h>
#include <esp_timer.h>

#include <cJSON.h>

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "tuya_cloud/credentials.h"
#include "tuya_cloud/customer_api.h"  // DeadlineExceeded
#include "tuya_cloud/openapi_constants.h"
#include "tuya_cloud/time_sync.h"
#include "tuya_crypto/hmac_sha256.h"
#include "tuya_crypto/json_fields.h"
#include "tuya_crypto/json_guard.h"
#include "tuya_crypto/json_writer.h"
#include "tuya_crypto/md5.h"
#include "tuya_crypto/sha256.h"

#include "signing.h"

static const char* TAG = "TuyaIoT";

namespace tuya_cloud {

namespace {

// Token re-login margin: the last 5 minutes of validity.
constexpr int64_t kOpenApiRefreshMarginMs = 300000;

// Error codes marking the token invalid: one re-login + retry.
constexpr int64_t kOpenApiCodeTokenInvalid = 1010;
constexpr int64_t kOpenApiCodeSignInvalid = 1004;
constexpr int64_t kOpenApiCodeSignInvalid2 = -9999999;

// Background login task budget: 12 KB covers esp_http_client + mbedTLS + cJSON.
constexpr uint32_t kBackgroundLoginStackBytes = 12288;
constexpr UBaseType_t kBackgroundLoginTaskPriority = 3;

const char* MethodName(HttpMethod method) {
    switch (method) {
        case HttpMethod::kGet: return "GET";
        case HttpMethod::kPost: return "POST";
        case HttpMethod::kPut: return "PUT";
        case HttpMethod::kDelete: return "DELETE";
    }
    return "?";
}

std::string TruncateForLog(const std::string& s, size_t max_chars) {
    if (s.size() <= max_chars) {
        return s;
    }
    return s.substr(0, max_chars) + "...(" + std::to_string(s.size()) + "B)";
}

void SortParams(OpenApiParams& params) {
    // Sort by key for the sign string; the URL follows suit.
    std::sort(params.begin(), params.end(),
              [](const auto& a, const auto& b) { return a.first < b.first; });
}

/// Uppercases an ASCII hex string in place.
std::string ToUpperAscii(std::string s) {
    for (char& c : s) {
        c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    }
    return s;
}

/// Server-returned URLs must stay on TLS; a non-https value is unusable.
bool IsHttpsUrl(const std::string& url) {
    return url.starts_with("https://");
}

}  // namespace

// Pure pipeline stages (golden-vector tested in openapi_self_test.cc).

std::string BuildOpenApiSignPath(const std::string& path, const OpenApiParams& params) {
    if (params.empty()) {
        return path;
    }
    OpenApiParams sorted = params;
    SortParams(sorted);
    std::string out;
    out.reserve(path.size() + 16);
    out += path;
    out += '?';
    for (size_t i = 0; i < sorted.size(); ++i) {
        if (i > 0) {
            out += '&';
        }
        // RAW values — the sign string never percent-encodes.
        out += sorted[i].first;
        out += '=';
        out += sorted[i].second;
    }
    return out;
}

std::string BuildOpenApiUrl(const std::string& endpoint, const std::string& path,
                            const OpenApiParams& params) {
    std::string url;
    url.reserve(endpoint.size() + path.size() + 32);
    url += endpoint;
    url += path;
    if (params.empty()) {
        return url;
    }
    OpenApiParams sorted = params;
    SortParams(sorted);
    url += '?';
    for (size_t i = 0; i < sorted.size(); ++i) {
        if (i > 0) {
            url += '&';
        }
        // Transport encoding: RFC 3986 percent-encoding (space -> %20).
        url += PercentEncode(sorted[i].first);
        url += '=';
        url += PercentEncode(sorted[i].second);
    }
    return url;
}

std::string ResolveOpenApiTemplate(const std::string& path_template, const std::string& uid,
                                   const std::string& home_id, const std::string& room_id) {
    std::string path = path_template;
    const auto replace_all = [&path](std::string_view placeholder, const std::string& value) {
        size_t pos = 0;
        while ((pos = path.find(placeholder.data(), pos, placeholder.size())) !=
               std::string::npos) {
            path.replace(pos, placeholder.size(), value);
            pos += value.size();
        }
    };
    replace_all("{uid}", uid);
    // Only meaningful for the home/room endpoints; harmlessly absent elsewhere.
    replace_all("{home_id}", home_id);
    replace_all("{room_id}", room_id);
    return path;
}

std::string BuildOpenApiSign(const std::string& client_id, const std::string& access_token,
                             const std::string& access_secret, int64_t t_ms,
                             const std::string& method, const std::string& body_json,
                             const std::string& path_with_params) {
    // Body hash: sha256 hexdigest of the exact body string sent ("" for GET), lowercase.
    std::string body_sha;
    if (!tuya_crypto::Sha256Hex(body_json, body_sha)) {
        return "";  // primitive failure already logged
    }

    // message = client_id + access_token + str(t) + method + "\n" + body_sha +
    // "\n" + "\n" + path_with_params; an empty access_token is not concatenated.
    std::string message;
    message.reserve(client_id.size() + access_token.size() + method.size() + body_sha.size() +
                    path_with_params.size() + 16);
    message += client_id;
    message += access_token;
    message += std::to_string(t_ms);
    message += method;
    message += '\n';
    message += body_sha;
    message += '\n';
    message += '\n';
    message += path_with_params;

    std::string sign;
    if (!tuya_crypto::HmacSha256Hex(access_secret, message, sign)) {
        return "";  // primitive failure already logged
    }
    // hexdigest().upper() — HmacSha256Hex is lowercase.
    return ToUpperAscii(std::move(sign));
}

bool OpenApiTokenNeedsRefresh(const OpenApiToken& token, int64_t now_ms) {
    if (!token.Valid() || token.expire_time <= 0) {
        return true;
    }
    // Expiry = t (ms) + expire_time (s) * 1000, minus the 5-minute margin.
    const int64_t expire_ms = token.t + token.expire_time * 1000;
    return now_ms >= expire_ms - kOpenApiRefreshMarginMs;
}

std::string BuildOpenApiLoginBodySmartHome(const std::string& username, const std::string& password,
                                           const std::string& country_code,
                                           const std::string& schema) {
    std::string pw_md5;
    if (!tuya_crypto::Md5Hex(password, pw_md5)) {
        return "";
    }
    tuya_crypto::JsonWriter writer;
    writer.BeginObject();
    writer.Key("username");
    writer.String(username);
    writer.Key("password");
    writer.String(pw_md5);
    writer.Key("country_code");
    writer.String(country_code);
    writer.Key("schema");
    writer.String(schema);
    writer.EndObject();
    if (writer.HasError()) {
        return "";
    }
    return writer.Str();
}

std::string BuildOpenApiLoginBodyCustom(const std::string& username, const std::string& password) {
    std::string pw_sha;
    if (!tuya_crypto::Sha256Hex(password, pw_sha)) {
        return "";
    }
    tuya_crypto::JsonWriter writer;
    writer.BeginObject();
    writer.Key("username");
    writer.String(username);
    writer.Key("password");
    writer.String(pw_sha);
    writer.EndObject();
    if (writer.HasError()) {
        return "";
    }
    return writer.Str();
}

void OpenApiClient::Configure(const OpenApiCredentials& creds) {
    std::lock_guard<std::mutex> lock(mutex_);
    creds_ = creds;
    if (creds_.datacenter.empty()) {
        creds_.datacenter = std::string(kOpenApiDcDefault);
    }
    // A stored token_ is kept on purpose; ClearToken() is the explicit account-switch path.
    logged_in_ = false;
    UpdateSnapshotLocked();
    // Never log the secret/password — identifier and region only.
    ESP_LOGI(TAG, "Configured: access_id=%s datacenter=%s (token uid=%s)", creds_.access_id.c_str(),
             creds_.datacenter.c_str(), token_.uid.c_str());
}

bool OpenApiClient::IsConfigured() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return !creds_.access_id.empty() && !creds_.access_secret.empty();
}

bool OpenApiClient::HasToken() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return token_.Valid();
}

bool OpenApiClient::Login() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (logged_in_) {
        ESP_LOGD(TAG, "Login() while a session login exists — re-login");
    }
    snapshot_login_in_flight_.store(true);
    const bool ok = LoginLocked(0);
    snapshot_login_in_flight_.store(false);
    return ok;
}

bool OpenApiClient::LoginInBackground() {
    // Spawn path: deliberately NO mutex_ (the httpd status handler calls this).
    if (!snapshot_configured_.load()) {
        return false;
    }
    // Single-flight: at most one "oa_login" task alive; a second call while
    // one runs reports "a login is running".
    bool expected = false;
    if (!login_task_running_.compare_exchange_strong(expected, true)) {
        return true;
    }
    // A stale abort from a previous attempt must not kill this spawn.
    login_abort_.store(false);
    ESP_LOGI(TAG, "Spawning background Tuya IoT login (oa_login task)");
    const BaseType_t created =
        xTaskCreate(BackgroundLoginTaskTrampoline, "oa_login", kBackgroundLoginStackBytes, nullptr,
                    kBackgroundLoginTaskPriority, nullptr);
    if (created != pdPASS) {
        // The next request's proactive re-login recovers.
        login_task_running_.store(false);
        ESP_LOGE(TAG, "xTaskCreate(oa_login) failed — login deferred to the next request");
        return false;
    }
    return true;
}

void OpenApiClient::BackgroundLoginTaskTrampoline(void* /*arg*/) {
    OpenApiClient& client = GetInstance();
    client.BackgroundLoginTaskBody();
    // LAST action before vTaskDelete: release the single-flight guard.
    client.login_task_running_.store(false);
    vTaskDelete(nullptr);  // detached: the task deletes itself
}

void OpenApiClient::BackgroundLoginTaskBody() {
    // Configure from the PERSISTED credentials, never a racing in-memory copy.
    OpenApiCredentials creds;
    if (!OpenApiCredentialsStore::Load(creds)) {
        ESP_LOGE(TAG, "Tuya IoT login aborted — credentials vanished from NVS");
        return;
    }
    Configure(creds);
    if (Login()) {
        if (login_abort_.load()) {
            // A factory reset wiped NVS mid-login — the fresh token must not survive.
            ClearToken();
            ESP_LOGW(TAG, "Tuya IoT login aborted after success — token dropped, not persisted");
            return;
        }
        SaveToken();
        ESP_LOGI(TAG, "Tuya IoT login OK (uid=%s)", token().uid.c_str());
        return;
    }
    // Login FAILED: drop any token from a PREVIOUS credential set.
    ClearToken();
    ESP_LOGE(TAG, "Tuya IoT login FAILED — token absent; re-save the credentials to retry");
}

OpenApiResponse OpenApiClient::Get(const std::string& path_template, const OpenApiParams& params,
                                   const std::string& home_id, const std::string& room_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    return RequestLocked(HttpMethod::kGet, path_template, params, "", 0, home_id, room_id);
}

OpenApiResponse OpenApiClient::Post(const std::string& path_template, const std::string& body_json,
                                    int64_t deadline_ms) {
    std::lock_guard<std::mutex> lock(mutex_);
    return RequestLocked(HttpMethod::kPost, path_template, {}, body_json, deadline_ms);
}

bool OpenApiClient::SaveToken() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!token_.Valid()) {
        ESP_LOGW(TAG, "SaveToken without a valid token — nothing to persist");
        return false;
    }
    OpenApiCredentialsStore::SaveToken(token_);
    return true;
}

bool OpenApiClient::LoadToken() {
    std::lock_guard<std::mutex> lock(mutex_);
    OpenApiToken loaded;
    if (!OpenApiCredentialsStore::LoadToken(loaded)) {
        return false;  // absent / structurally incomplete (logged by the store)
    }
    // Keep the stale fields (uid, platform_url) even when expired — the next
    // Login() keeps the account's real endpoint.
    if (!loaded.platform_url.empty() && !IsHttpsUrl(loaded.platform_url)) {
        // A legacy persisted value must never steer traffic.
        ESP_LOGW(TAG, "persisted platform_url is not https — ignored");
        loaded.platform_url.clear();
    }
    token_ = loaded;
    const bool fresh = !OpenApiTokenNeedsRefresh(token_, TimeSync::NowMs());
    logged_in_ = fresh;
    // The snapshot must reflect the RESTORED token too (boot path).
    UpdateSnapshotLocked();
    if (fresh) {
        ESP_LOGI(TAG, "Tuya IoT token loaded: uid=%s expires in %llds", token_.uid.c_str(),
                 static_cast<long long>(token_.expire_time));
    } else {
        ESP_LOGW(TAG, "Tuya IoT token loaded but expired/near-expiry — re-login on next request");
    }
    return fresh;
}

void OpenApiClient::ClearToken() {
    std::lock_guard<std::mutex> lock(mutex_);
    token_ = OpenApiToken{};
    logged_in_ = false;
    OpenApiCredentialsStore::ClearToken();
    UpdateSnapshotLocked();
    ESP_LOGI(TAG, "Tuya IoT token cleared (memory + NVS)");
}

OpenApiToken OpenApiClient::token() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return token_;
}

OpenApiCredentials OpenApiClient::credentials() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return creds_;
}

// ---- internals (mutex_ already held by the callers) ----

OpenApiResponse OpenApiClient::RequestLocked(HttpMethod method, const std::string& path_template,
                                             const OpenApiParams& params,
                                             const std::string& body_json, int64_t deadline_ms,
                                             const std::string& home_id,
                                             const std::string& room_id) {
    if (creds_.access_id.empty() || creds_.access_secret.empty()) {
        ESP_LOGE(TAG, "%s(%s) before Configure() — rejecting", MethodName(method),
                 path_template.c_str());
        OpenApiResponse resp;
        resp.msg = "not configured";
        return resp;
    }

    // Proactive re-login: on failure abort — a request with a known-dead
    // token only burns a 60 s timeout.
    if (OpenApiTokenNeedsRefresh(token_, TimeSync::NowMs()) && creds_.HasLogin()) {
        snapshot_login_in_flight_.store(true);
        const bool logged_in = LoginLocked(deadline_ms);
        snapshot_login_in_flight_.store(false);
        if (!logged_in) {
            OpenApiResponse resp;
            resp.msg = "re-login failed";
            return resp;
        }
    }

    OpenApiResponse resp =
        DoRequestLocked(method, path_template, params, body_json, deadline_ms, home_id, room_id);

    // Reactive re-login: the server answered a token-invalid code — one
    // re-login + retry. The login requests never pass through here (no recursion).
    if (!resp.ok &&
        (resp.code == kOpenApiCodeTokenInvalid || resp.code == kOpenApiCodeSignInvalid ||
         resp.code == kOpenApiCodeSignInvalid2) &&
        creds_.HasLogin()) {
        ESP_LOGW(TAG, "token invalid (code=%lld) — re-login and retry once",
                 static_cast<long long>(resp.code));
        snapshot_login_in_flight_.store(true);
        const bool logged_in = LoginLocked(deadline_ms);
        snapshot_login_in_flight_.store(false);
        if (logged_in) {
            resp = DoRequestLocked(method, path_template, params, body_json, deadline_ms, home_id,
                                   room_id);
        }
    }
    return resp;
}

bool OpenApiClient::LoginLocked(int64_t deadline_ms) {
    if (!creds_.HasLogin()) {
        ESP_LOGE(TAG, "Login() without login credentials (Configure first)");
        return false;
    }

    // SMART_HOME first: MD5 password.
    const std::string smart_body = BuildOpenApiLoginBodySmartHome(
        creds_.username, creds_.password, creds_.country_code, creds_.schema);
    if (smart_body.empty()) {
        ESP_LOGE(TAG, "SMART_HOME login body build failed (crypto primitive)");
        return false;
    }
    OpenApiResponse resp = DoRequestLocked(HttpMethod::kPost, std::string(kOpenApiLoginSmartHome),
                                           {}, smart_body, deadline_ms);
    if (resp.ok) {
        return StoreLoginToken(resp);
    }
    ESP_LOGW(TAG, "SMART_HOME login rejected: code=%lld msg=%s", static_cast<long long>(resp.code),
             resp.msg.c_str());

    // CUSTOM fallback (SHA-256 password): only when the server actually answered —
    // a transport failure is an endpoint/network problem the same-endpoint fallback cannot fix.
    if (!resp.envelope) {
        logged_in_ = false;
        return false;
    }
    const std::string custom_body = BuildOpenApiLoginBodyCustom(creds_.username, creds_.password);
    if (custom_body.empty()) {
        ESP_LOGE(TAG, "CUSTOM login body build failed (crypto primitive)");
        return false;
    }
    OpenApiResponse custom = DoRequestLocked(HttpMethod::kPost, std::string(kOpenApiLoginCustom),
                                             {}, custom_body, deadline_ms);
    if (custom.ok) {
        return StoreLoginToken(custom);  // incomplete-result failure logged inside
    }
    ESP_LOGW(TAG, "CUSTOM login rejected: code=%lld msg=%s", static_cast<long long>(custom.code),
             custom.msg.c_str());
    logged_in_ = false;
    return false;
}

bool OpenApiClient::StoreLoginToken(const OpenApiResponse& resp) {
    // result fields; nothing sensitive logged — lengths only.
    if (resp.result == nullptr || !cJSON_IsObject(resp.result)) {
        ESP_LOGE(TAG, "login success without a result object");
        logged_in_ = false;
        return false;
    }
    OpenApiToken tok;
    tok.access_token = tuya_crypto::GetStringField(resp.result, "access_token");
    tok.refresh_token = tuya_crypto::GetStringField(resp.result, "refresh_token");
    tok.uid = tuya_crypto::GetStringField(resp.result, "uid");
    // "expire" first, "expire_time" fallback.
    tok.expire_time = tuya_crypto::GetInt64Field(resp.result, "expire");
    if (tok.expire_time <= 0) {
        tok.expire_time = tuya_crypto::GetInt64Field(resp.result, "expire_time");
    }
    tok.platform_url = tuya_crypto::GetStringField(resp.result, "platform_url");
    if (!tok.platform_url.empty() && !IsHttpsUrl(tok.platform_url)) {
        ESP_LOGW(TAG, "login platform_url is not https — rejected");
        // Keep the previous https endpoint; else the datacenter/default chain applies.
        tok.platform_url = IsHttpsUrl(token_.platform_url) ? token_.platform_url : std::string();
    }
    tok.t = resp.t;

    if (tok.access_token.empty() || tok.uid.empty() || tok.expire_time <= 0 || tok.t <= 0) {
        ESP_LOGE(TAG,
                 "login success flag set but the result is unusable (at=%u rt=%u uid=%u exp=%lld "
                 "t=%lld)",
                 static_cast<unsigned>(tok.access_token.size()),
                 static_cast<unsigned>(tok.refresh_token.size()),
                 static_cast<unsigned>(tok.uid.size()), static_cast<long long>(tok.expire_time),
                 static_cast<long long>(tok.t));
        logged_in_ = false;
        return false;
    }

    token_ = tok;
    logged_in_ = true;
    UpdateSnapshotLocked();
    // platform_url wins over the datacenter guess (ActiveEndpointLocked()).
    ESP_LOGI(TAG, "Tuya IoT login ok: uid=%s expires in %llds endpoint=%s", tok.uid.c_str(),
             static_cast<long long>(tok.expire_time), ActiveEndpointLocked().c_str());
    return true;
}

OpenApiResponse OpenApiClient::DoRequestLocked(HttpMethod method, const std::string& path_template,
                                               const OpenApiParams& params,
                                               const std::string& body_json, int64_t deadline_ms,
                                               const std::string& home_id,
                                               const std::string& room_id) {
    OpenApiResponse resp;
    if (DeadlineExceeded(deadline_ms)) {
        ESP_LOGW(TAG, "chain deadline exceeded — skipping %s %s", MethodName(method),
                 path_template.c_str());
        resp.msg = "deadline exceeded";
        return resp;
    }
    // {uid} -> token uid, {home_id}/{room_id} -> caller values (raw; a
    // tokenless home request yields a malformed path — caller misuse).
    const std::string path = ResolveOpenApiTemplate(path_template, token_.uid, home_id, room_id);
    const std::string sign_path = BuildOpenApiSignPath(path, params);
    const std::string url = BuildOpenApiUrl(ActiveEndpointLocked(), path, params);

    // The token joins the sign only while valid; the access_token HEADER is
    // still sent empty on the login requests.
    const int64_t now_ms = TimeSync::NowMs();
    const bool token_in_sign = token_.Valid() && !OpenApiTokenNeedsRefresh(token_, now_ms);
    const std::string access_token = token_in_sign ? token_.access_token : std::string();

    const std::string sign = BuildOpenApiSign(creds_.access_id, access_token, creds_.access_secret,
                                              now_ms, MethodName(method), body_json, sign_path);
    if (sign.empty() || url.empty()) {
        ESP_LOGE(TAG, "sign/URL build failed — aborting request");
        resp.msg = "sign build failed";
        return resp;
    }

    // Header set.
    std::vector<HttpClient::Header> headers;
    headers.reserve(6);
    headers.push_back({"client_id", creds_.access_id});
    headers.push_back({"access_token", access_token});  // "" on login, still sent
    headers.push_back({"sign", sign});
    headers.push_back({"sign_method", "HMAC-SHA256"});
    headers.push_back({"t", std::to_string(now_ms)});
    headers.push_back({"lang", "en"});

    const int64_t start_us = esp_timer_get_time();
    const HttpClient::Result http = http_client_.Request(url, method, headers, body_json);
    const int64_t elapsed_ms = (esp_timer_get_time() - start_us) / 1000;

    const bool http_ok = http.network_ok && http.status_code >= 200 && http.status_code < 300;
    if (!http_ok) {
        // Path only in the log — never tokens/sign.
        ESP_LOGE(TAG, "%s %s failed: net_ok=%d status=%d (%u ms) body=%.200s", MethodName(method),
                 path.c_str(), http.network_ok ? 1 : 0, http.status_code,
                 static_cast<unsigned>(elapsed_ms), http.body.c_str());
    } else {
        ESP_LOGI(TAG, "%s %s -> %d (%u ms, %u B)", MethodName(method), path.c_str(),
                 http.status_code, static_cast<unsigned>(elapsed_ms),
                 static_cast<unsigned>(http.body.size()));
    }

    // Envelope parse; runs even on non-2xx so the error code survives for the re-login policy.
    if (!tuya_crypto::JsonNestingWithinLimit(http.body.c_str(), http.body.size(),
                                             tuya_crypto::kMaxJsonDepth)) {
        if (http_ok) {
            resp.msg = "bad_json";
            ESP_LOGE(TAG, "response body nesting over %d (%u B)", tuya_crypto::kMaxJsonDepth,
                     static_cast<unsigned>(http.body.size()));
        } else {
            resp.msg =
                "http " + std::to_string(http.status_code) + ": " + TruncateForLog(http.body, 200);
        }
        return resp;
    }
    cJSON* root = cJSON_ParseWithLength(http.body.c_str(), http.body.size());
    if (root == nullptr || !cJSON_IsObject(root)) {
        cJSON_Delete(root);
        if (http_ok) {
            resp.msg = "bad_json";
            ESP_LOGE(TAG, "response body is not valid JSON (%u B)",
                     static_cast<unsigned>(http.body.size()));
        } else {
            resp.msg =
                "http " + std::to_string(http.status_code) + ": " + TruncateForLog(http.body, 200);
        }
        return resp;
    }
    // RAII frees the tree; the handed-out `result` is a DUPLICATED subtree.
    tuya_crypto::JsonRoot guard{root};
    resp.envelope = true;
    const cJSON* success_item = cJSON_GetObjectItemCaseSensitive(root, "success");
    const bool success = cJSON_IsTrue(success_item) ? true : false;
    resp.code = tuya_crypto::GetInt64Field(root, "code");
    resp.msg = tuya_crypto::GetStringField(root, "msg");
    resp.t = tuya_crypto::GetInt64Field(root, "t");

    if (!http_ok) {
        // Non-2xx with an envelope: keep the server's code/msg, prefixed with the status.
        resp.msg = "http " + std::to_string(http.status_code) + ": " + resp.msg;
        return resp;
    }
    if (!success) {
        ESP_LOGW(TAG, "api error: code=%lld msg=%s", static_cast<long long>(resp.code),
                 resp.msg.c_str());
        return resp;
    }

    resp.ok = true;

    // result: object/array -> duplicated cJSON; absent/null -> empty (ok=true).
    const cJSON* result_item = cJSON_GetObjectItemCaseSensitive(root, "result");
    if (result_item == nullptr || cJSON_IsNull(result_item)) {
        return resp;  // ok=true, result=nullptr
    }
    if (cJSON_IsObject(result_item) || cJSON_IsArray(result_item)) {
        resp.result = cJSON_Duplicate(result_item, /*recurse=*/1);
        if (resp.result == nullptr) {
            ESP_LOGE(TAG, "result duplication failed (OOM?)");
            resp.ok = false;
            resp.msg = "result copy failed";
            return resp;
        }
    }
    return resp;
}

std::string OpenApiClient::ActiveEndpointLocked() const {
    // platform_url overrides the configured datacenter (default set in Configure()).
    if (IsHttpsUrl(token_.platform_url)) {
        return token_.platform_url;
    }
    if (!token_.platform_url.empty()) {
        ESP_LOGW(TAG, "platform_url is not https — ignored, using the configured endpoint");
    }
    if (!creds_.datacenter.empty()) {
        return creds_.datacenter;
    }
    return std::string(kOpenApiDcDefault);
}

// ---- Lock-free snapshot (status reads never block behind a login) ----

void OpenApiClient::UpdateSnapshotLocked() {
    // Called under mutex_ after every state change; the atomics may trail by
    // milliseconds — fine for UI status.
    snapshot_configured_.store(!creds_.access_id.empty() && !creds_.access_secret.empty());
    // "has_token" is USABILITY, not structure: an expired/near-expiry token
    // counts as "not logged in" (RefreshTokenSnapshotForExpiry catches later expiry).
    const bool usable = token_.Valid() && !OpenApiTokenNeedsRefresh(token_, TimeSync::NowMs());
    snapshot_has_token_.store(usable);
    // Mirror the absolute expiry so the lock-free re-check needs no mutex.
    snapshot_token_expire_ms_.store(usable ? token_.ExpireMs() : 0);
    {
        std::lock_guard<std::mutex> slock(snapshot_strings_mutex_);
        snapshot_uid_ = token_.uid;
        snapshot_endpoint_ = ActiveEndpointLocked();
    }
}

std::string OpenApiClient::snapshot_uid() const {
    std::lock_guard<std::mutex> slock(snapshot_strings_mutex_);
    return snapshot_uid_;
}

std::string OpenApiClient::snapshot_endpoint() const {
    std::lock_guard<std::mutex> slock(snapshot_strings_mutex_);
    return snapshot_endpoint_;
}

void OpenApiClient::RefreshTokenSnapshotForExpiry() {
    // One-directional re-check; skipping while a login is in flight avoids the
    // common race (StoreLoginToken recomputes the snapshot).
    if (!snapshot_has_token_.load() || snapshot_login_in_flight_.load()) {
        return;  // nothing to expire / being replaced right now
    }
    const int64_t expire_ms = snapshot_token_expire_ms_.load();
    if (expire_ms <= 0) {
        return;  // expiry unknown (never mirrored)
    }
    // Same margin as OpenApiTokenNeedsRefresh.
    if (TimeSync::NowMs() >= expire_ms - kOpenApiRefreshMarginMs) {
        snapshot_has_token_.store(false);
        ESP_LOGD(TAG, "Tuya IoT token expired since the last snapshot update — flagged");
    }
}

}  // namespace tuya_cloud
