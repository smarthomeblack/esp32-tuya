#include "tuya_cloud/credentials.h"

#include <esp_log.h>

#include <cerrno>
#include <cstdlib>
#include <string>

#include "sys/settings.h"

static const char* TAG = "Credentials";

namespace tuya_cloud {

std::atomic<bool> Credentials::persistence_suppressed_{false};

namespace {

// NVS namespace and keys (names fit the 15-char NVS limit).
constexpr const char* kNvsNamespace = "tuya";
constexpr const char* kKeyUserCode = "user_code";
constexpr const char* kKeyTerminalId = "terminal_id";
constexpr const char* kKeyEndpoint = "endpoint";
constexpr const char* kKeyT = "t";
constexpr const char* kKeyUid = "uid";
constexpr const char* kKeyExpireTime = "expire_time";
constexpr const char* kKeyAccessToken = "access_token";
constexpr const char* kKeyRefreshToken = "refresh_token";

// OpenAPI store: dedicated NVS namespace.
constexpr const char* kOpenApiNvsNamespace = "openapi";
constexpr const char* kOaKeyAccessId = "access_id";
constexpr const char* kOaKeyAccessSecret = "access_secret";
constexpr const char* kOaKeyUsername = "username";
constexpr const char* kOaKeyPassword = "password";
constexpr const char* kOaKeyCountryCode = "country_code";
constexpr const char* kOaKeySchema = "schema";
constexpr const char* kOaKeyDatacenter = "datacenter";
constexpr const char* kOaKeyAccessToken = "access_token";
constexpr const char* kOaKeyRefreshToken = "refresh_token";
constexpr const char* kOaKeyUid = "uid";
constexpr const char* kOaKeyPlatformUrl = "platform_url";
constexpr const char* kOaKeyExpireTime = "expire_time";
constexpr const char* kOaKeyT = "t";

/// Parses a decimal-string int64; 0 for empty/garbage ("not present").
int64_t ParseInt64(const std::string& text) {
    if (text.empty()) {
        return 0;
    }
    errno = 0;
    char* end = nullptr;
    const long long value = std::strtoll(text.c_str(), &end, 10);
    if (errno != 0 || end == nullptr || *end != '\0') {
        return 0;
    }
    return static_cast<int64_t>(value);
}

void WriteTokenKeys(sys::Settings& settings, const TokenInfo& info) {
    // int64 as decimal strings — t exceeds INT32_MAX (an int-typed write would truncate).
    settings.SetString(kKeyT, std::to_string(info.t));
    settings.SetString(kKeyUid, info.uid);
    settings.SetString(kKeyExpireTime, std::to_string(info.expire_time));
    settings.SetString(kKeyAccessToken, info.access_token);
    settings.SetString(kKeyRefreshToken, info.refresh_token);
}

}  // namespace

bool Credentials::Load() {
    sys::Settings settings(kNvsNamespace, false);
    user_code_ = settings.GetString(kKeyUserCode);
    terminal_id_ = settings.GetString(kKeyTerminalId);
    endpoint_ = settings.GetString(kKeyEndpoint);
    token_.t = ParseInt64(settings.GetString(kKeyT));
    token_.uid = settings.GetString(kKeyUid);
    token_.access_token = settings.GetString(kKeyAccessToken);
    token_.refresh_token = settings.GetString(kKeyRefreshToken);
    token_.expire_time = ParseInt64(settings.GetString(kKeyExpireTime));

    const bool all_present = !user_code_.empty() && !terminal_id_.empty() && !endpoint_.empty() &&
                             !token_.uid.empty() && !token_.access_token.empty() &&
                             !token_.refresh_token.empty() && token_.t != 0 &&
                             token_.expire_time != 0;
    if (!all_present) {
        ESP_LOGW(TAG, "Credentials incomplete (endpoint=%s uid=%s token=%s) — not provisioned",
                 endpoint_.empty() ? "<missing>" : "present",
                 token_.uid.empty() ? "<missing>" : "present",
                 HasToken() ? "present" : "<missing>");
    }
    return all_present;
}

void Credentials::SaveTokenInfo(const TokenInfo& info) {
    token_ = info;
    {
        sys::Settings settings(kNvsNamespace, true);
        WriteTokenKeys(settings, info);
    }
    ESP_LOGI(TAG, "Token info saved (uid=%s, expires in %llds)", info.uid.c_str(),
             static_cast<long long>(info.expire_time));
}

bool Credentials::SaveLoginInfo(const std::string& user_code, const std::string& terminal_id,
                                const std::string& endpoint, const TokenInfo& info) {
    if (persistence_suppressed_.load()) {
        ESP_LOGW(TAG, "Login info save suppressed (factory reset)");
        return false;  // no NVS write after the wipe
    }
    user_code_ = user_code;
    terminal_id_ = terminal_id;
    endpoint_ = endpoint;
    token_ = info;
    {
        sys::Settings settings(kNvsNamespace, true);
        settings.SetString(kKeyUserCode, user_code);
        settings.SetString(kKeyTerminalId, terminal_id);
        settings.SetString(kKeyEndpoint, endpoint);
        WriteTokenKeys(settings, info);
    }
    ESP_LOGI(TAG, "Login info saved (endpoint=%s uid=%s)", endpoint.c_str(), info.uid.c_str());
    return true;
}

void Credentials::Clear() {
    {
        sys::Settings settings(kNvsNamespace, true);
        settings.EraseAll();
    }
    user_code_.clear();
    terminal_id_.clear();
    endpoint_.clear();
    token_ = TokenInfo{};
    ESP_LOGI(TAG, "Credentials cleared");
}

void Credentials::SuppressPersistence() {
    persistence_suppressed_.store(true);
}

bool Credentials::PersistenceSuppressed() {
    return persistence_suppressed_.load();
}

bool Credentials::HasToken() const {
    return !token_.access_token.empty() && !token_.refresh_token.empty();
}

bool OpenApiCredentialsStore::Load(OpenApiCredentials& creds) {
    sys::Settings settings(kOpenApiNvsNamespace, false);
    creds.access_id = settings.GetString(kOaKeyAccessId);
    creds.access_secret = settings.GetString(kOaKeyAccessSecret);
    creds.username = settings.GetString(kOaKeyUsername);
    creds.password = settings.GetString(kOaKeyPassword);
    creds.country_code = settings.GetString(kOaKeyCountryCode);
    creds.schema = settings.GetString(kOaKeySchema);
    creds.datacenter = settings.GetString(kOaKeyDatacenter);

    const bool all_present =
        !creds.access_id.empty() && !creds.access_secret.empty() && !creds.username.empty();
    if (!all_present) {
        ESP_LOGW(TAG,
                 "Tuya IoT credentials incomplete (access_id=%s username=%s) — not provisioned",
                 creds.access_id.empty() ? "<missing>" : "present",
                 creds.username.empty() ? "<missing>" : "present");
    }
    return all_present;
}

void OpenApiCredentialsStore::Save(const OpenApiCredentials& creds) {
    bool switched_account = false;
    {
        sys::Settings settings(kOpenApiNvsNamespace, true);
        // A different access_id is a different account: its token must not survive.
        switched_account = settings.GetString(kOaKeyAccessId) != creds.access_id;
        if (switched_account) {
            settings.EraseKey(kOaKeyAccessToken);
            settings.EraseKey(kOaKeyRefreshToken);
            settings.EraseKey(kOaKeyUid);
            settings.EraseKey(kOaKeyPlatformUrl);
        }
        settings.SetString(kOaKeyAccessId, creds.access_id);
        settings.SetString(kOaKeyAccessSecret, creds.access_secret);
        settings.SetString(kOaKeyUsername, creds.username);
        settings.SetString(kOaKeyPassword, creds.password);
        settings.SetString(kOaKeyCountryCode, creds.country_code);
        settings.SetString(kOaKeySchema, creds.schema);
        settings.SetString(kOaKeyDatacenter, creds.datacenter);
    }
    if (switched_account) {
        ESP_LOGI(TAG, "Tuya IoT access_id changed — old account's token keys erased from NVS");
    }
    // Never log the secret/password.
    ESP_LOGI(TAG, "Tuya IoT credentials saved (access_id=%s datacenter=%s)",
             creds.access_id.c_str(), creds.datacenter.c_str());
}

void OpenApiCredentialsStore::SaveToken(const OpenApiToken& token) {
    {
        sys::Settings settings(kOpenApiNvsNamespace, true);
        // int64 as decimal strings (t exceeds INT32_MAX).
        settings.SetString(kOaKeyAccessToken, token.access_token);
        settings.SetString(kOaKeyRefreshToken, token.refresh_token);
        settings.SetString(kOaKeyUid, token.uid);
        settings.SetString(kOaKeyPlatformUrl, token.platform_url);
        settings.SetString(kOaKeyExpireTime, std::to_string(token.expire_time));
        settings.SetString(kOaKeyT, std::to_string(token.t));
    }
    ESP_LOGI(TAG, "Tuya IoT token saved (uid=%s, expires in %llds)", token.uid.c_str(),
             static_cast<long long>(token.expire_time));
}

bool OpenApiCredentialsStore::LoadToken(OpenApiToken& token) {
    sys::Settings settings(kOpenApiNvsNamespace, false);
    token.access_token = settings.GetString(kOaKeyAccessToken);
    token.refresh_token = settings.GetString(kOaKeyRefreshToken);
    token.uid = settings.GetString(kOaKeyUid);
    token.platform_url = settings.GetString(kOaKeyPlatformUrl);
    token.expire_time = ParseInt64(settings.GetString(kOaKeyExpireTime));
    token.t = ParseInt64(settings.GetString(kOaKeyT));

    const bool complete =
        !token.access_token.empty() && !token.uid.empty() && token.t != 0 && token.expire_time != 0;
    if (!complete) {
        ESP_LOGW(TAG, "Tuya IoT token incomplete (uid=%s t=%lld expire=%lld) — not persisted",
                 token.uid.empty() ? "<missing>" : "present", static_cast<long long>(token.t),
                 static_cast<long long>(token.expire_time));
    }
    return complete;
}

void OpenApiCredentialsStore::ClearToken() {
    {
        sys::Settings settings(kOpenApiNvsNamespace, true);
        settings.EraseKey(kOaKeyAccessToken);
        settings.EraseKey(kOaKeyRefreshToken);
        settings.EraseKey(kOaKeyUid);
        settings.EraseKey(kOaKeyPlatformUrl);
        settings.EraseKey(kOaKeyExpireTime);
        settings.EraseKey(kOaKeyT);
    }
    ESP_LOGI(TAG, "Tuya IoT token erased from NVS");
}

void OpenApiCredentialsStore::ClearAll() {
    {
        sys::Settings settings(kOpenApiNvsNamespace, true);
        settings.EraseAll();
    }
    ESP_LOGI(TAG, "Tuya IoT credentials + token cleared");
}

}  // namespace tuya_cloud
