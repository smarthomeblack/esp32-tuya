#pragma once

#include <atomic>
#include <cstdint>
#include <string>

#include "tuya_cloud/openapi_client.h"  // OpenApiToken, OpenApiCredentials

namespace tuya_cloud {

/// Token state. `t` is the server response timestamp in MILLISECONDS, `expire_time` in SECONDS.
struct TokenInfo {
    int64_t t = 0;
    std::string uid;
    std::string access_token;
    std::string refresh_token;
    int64_t expire_time = 0;  // seconds

    /// Absolute expiry, ms since epoch.
    int64_t ExpireMs() const { return t + expire_time * 1000; }
};

/// Durable per-account credentials in NVS namespace "tuya" via sys::Settings.
/// t/expire_time stored as STRINGS (Settings has no int64 accessor); not thread-safe — load once at
/// boot.
class Credentials {
public:
    /// True only when the complete set is present; partial data still populates.
    bool Load();

    /// Persists the token_info keys — rewritten on every token refresh.
    void SaveTokenInfo(const TokenInfo& info);

    /// Persists the full login result; updates the in-memory copy. False (no write) when
    /// persistence is suppressed.
    bool SaveLoginInfo(const std::string& user_code, const std::string& terminal_id,
                       const std::string& endpoint, const TokenInfo& info);

    /// Erases the whole "tuya" NVS namespace and clears the in-memory copy.
    void Clear();

    /// One-way until reboot: refuses every later SaveLoginInfo write.
    static void SuppressPersistence();

    /// True once SuppressPersistence() ran (factory reset in progress).
    static bool PersistenceSuppressed();

    bool HasToken() const;

    const std::string& user_code() const { return user_code_; }
    const std::string& terminal_id() const { return terminal_id_; }
    const std::string& endpoint() const { return endpoint_; }
    const TokenInfo& token() const { return token_; }

private:
    /// Process-wide factory-reset gate checked by SaveLoginInfo.
    static std::atomic<bool> persistence_suppressed_;
    std::string user_code_;
    std::string terminal_id_;
    std::string endpoint_;
    TokenInfo token_;
};

/// NVS persistence for the OpenAPI domain (namespace "openapi", separate from the sharing "tuya"
/// namespace). t/expire_time stored as decimal STRINGS; password stored PLAINTEXT. Static, no
/// locking.
class OpenApiCredentialsStore {
public:
    /// True only when the complete login set is present; partial data still populates.
    static bool Load(OpenApiCredentials& creds);

    /// Persists the whole credential set (an empty password is allowed).
    static void Save(const OpenApiCredentials& creds);

    /// Persists the token keys (re-login is the refresh in this flow).
    static void SaveToken(const OpenApiToken& token);

    /// True only when structurally complete; expiry is the caller's judgment.
    static bool LoadToken(OpenApiToken& token);

    /// Erases ONLY the token keys (keeps the account credentials).
    static void ClearToken();

    /// Erases the whole "openapi" NVS namespace (credentials + token).
    static void ClearAll();
};

}  // namespace tuya_cloud
