#pragma once

#include <cJSON.h>

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "tuya_cloud/http_client.h"
#include "tuya_cloud/openapi_constants.h"

namespace tuya_cloud {

/// Query params (name, value) in caller order; the sign string sorts them
/// by name.
using OpenApiParams = std::vector<std::pair<std::string, std::string>>;

/// OpenAPI token from a login response. `t` is the server response
/// timestamp in MILLISECONDS; `expire_time` is RELATIVE SECONDS; ExpireMs() is absolute.
struct OpenApiToken {
    std::string access_token;
    /// Stored but unused: re-login is the refresh.
    std::string refresh_token;
    std::string uid;
    /// Non-empty platform_url overrides the configured datacenter.
    std::string platform_url;
    int64_t expire_time = 0;  // seconds, RELATIVE to t
    int64_t t = 0;            // server ms of the login response

    bool Valid() const { return !access_token.empty() && !uid.empty(); }

    /// Absolute expiry, ms since epoch.
    int64_t ExpireMs() const { return t + expire_time * 1000; }
};

/// OpenAPI account credentials; persisted in NVS namespace "openapi".
struct OpenApiCredentials {
    std::string access_id;      // client_id
    std::string access_secret;  // HMAC key
    std::string username;       // login credentials (SMART_HOME)
    std::string password;
    std::string country_code;
    std::string schema;      // "tuyaSmart" | "smartlife"
    std::string datacenter;  // initial guess (kOpenApiDc*)

    bool HasLogin() const {
        return !access_id.empty() && !access_secret.empty() && !username.empty();
    }
};

/// Wire result: the plaintext {success, code, msg, result, t} envelope.
/// `ok` = HTTP 2xx AND top-level success == true; `result` is owned (freed by the destructor).
struct OpenApiResponse {
    bool ok = false;
    /// Body parsed as the envelope object (even on non-2xx).
    bool envelope = false;
    /// Server error code; 1010/1004 trigger one re-login + retry.
    int64_t code = 0;
    std::string msg;
    /// Owned result subtree; null when absent/not JSON.
    cJSON* result = nullptr;
    /// Server timestamp, ms.
    int64_t t = 0;

    OpenApiResponse() = default;
    ~OpenApiResponse() { cJSON_Delete(result); }

    OpenApiResponse(const OpenApiResponse&) = delete;
    OpenApiResponse& operator=(const OpenApiResponse&) = delete;

    OpenApiResponse(OpenApiResponse&& other) noexcept
        : ok(other.ok), envelope(other.envelope), code(other.code), msg(std::move(other.msg)),
          result(std::exchange(other.result, nullptr)), t(other.t) {}

    OpenApiResponse& operator=(OpenApiResponse&& other) noexcept {
        if (this != &other) {
            ok = other.ok;
            envelope = other.envelope;
            code = other.code;
            msg = std::move(other.msg);
            t = other.t;
            cJSON_Delete(result);
            result = std::exchange(other.result, nullptr);
        }
        return *this;
    }
};

/// Tuya OpenAPI client: HMAC-SHA256-signed requests, user login, re-login.
/// THREAD-SAFETY: public entry points take mutex_; *Locked helpers never re-lock.
class OpenApiClient {
public:
    static OpenApiClient& GetInstance() {
        static OpenApiClient instance;
        return instance;
    }
    OpenApiClient() = default;
    OpenApiClient(const OpenApiClient&) = delete;
    OpenApiClient& operator=(const OpenApiClient&) = delete;

    /// Stores the account credentials (empty datacenter -> kOpenApiDcDefault).
    /// Does NOT clear a loaded token — call ClearToken() when switching.
    void Configure(const OpenApiCredentials& creds);

    bool IsConfigured() const;

    bool HasToken() const;

    /// SMART_HOME login (MD5 password) with one CUSTOM fallback (SHA-256).
    /// NOT auto-persisted — call SaveToken().
    bool Login();

    /// Signed GET; `path_template` may contain "{uid}", "{home_id}", "{room_id}".
    OpenApiResponse Get(const std::string& path_template, const OpenApiParams& params = {},
                        const std::string& home_id = "", const std::string& room_id = "");

    /// Signed POST; body_json is the exact string signed AND sent.
    /// `deadline_ms`: absolute esp_timer-ms chain cap (0 = unbounded); legs skip past it.
    OpenApiResponse Post(const std::string& path_template, const std::string& body_json,
                         int64_t deadline_ms = 0);

    // ---- Token persistence (NVS namespace "openapi"; separate from "tuya") ----

    /// Persists the in-memory token; false when no valid token is held.
    bool SaveToken();

    /// Loads a persisted token; false when absent or already expired (stale uid/platform_url are
    /// still loaded).
    bool LoadToken();

    /// Drops the in-memory token AND its persisted copy.
    void ClearToken();

    /// Thread-safe by-value snapshots.
    OpenApiToken token() const;
    OpenApiCredentials credentials() const;

    /// Spawns the detached single-flight "oa_login" task (load, Configure, ClearToken, Login,
    /// SaveToken). Lock-free spawn (safe from the httpd status handler).
    bool LoginInBackground();

    /// Flags the RUNNING background login to drop its token instead of
    /// persisting it (factory-reset wipe race).
    void AbortLogin() { login_abort_.store(true); }

    // ---- Lock-free status snapshot (read WITHOUT mutex_; may be ms stale) ----
    bool snapshot_configured() const { return snapshot_configured_.load(); }
    /// True when a token usable at the last snapshot update is held (expired counts as "not logged
    /// in").
    bool snapshot_has_token() const { return snapshot_has_token_.load(); }
    bool snapshot_login_in_flight() const { return snapshot_login_in_flight_.load(); }
    /// Cached uid/endpoint for display (empty = unknown yet).
    std::string snapshot_uid() const;
    std::string snapshot_endpoint() const;

    /// Lock-free, one-directional expiry re-check for periodic pollers:
    /// flips snapshot_has_token_ to false once the held token has expired.
    void RefreshTokenSnapshotForExpiry();

private:
    /// Shared Get/Post pipeline (caller holds mutex_): configure check, proactive re-login,
    /// request, reactive 1010/1004 re-login + retry. `deadline_ms` 0 = unbounded.
    /// `home_id`/`room_id` resolve the same placeholders as `path_template`.
    OpenApiResponse RequestLocked(HttpMethod method, const std::string& path_template,
                                  const OpenApiParams& params, const std::string& body_json,
                                  int64_t deadline_ms, const std::string& home_id = "",
                                  const std::string& room_id = "");

    /// Login (caller holds mutex_).
    bool LoginLocked(int64_t deadline_ms);

    /// Extracts and stores the token fields from a login response.
    bool StoreLoginToken(const OpenApiResponse& resp);

    /// One request: resolve template, sign, HTTP, parse. Re-login policy lives in RequestLocked().
    OpenApiResponse DoRequestLocked(HttpMethod method, const std::string& path_template,
                                    const OpenApiParams& params, const std::string& body_json,
                                    int64_t deadline_ms, const std::string& home_id = "",
                                    const std::string& room_id = "");

    /// platform_url when set, else the configured datacenter, else default.
    std::string ActiveEndpointLocked() const;

    /// Body of the detached login task spawned by LoginInBackground().
    void BackgroundLoginTaskBody();

    /// FreeRTOS trampoline for the "oa_login" task.
    static void BackgroundLoginTaskTrampoline(void* /*arg*/);

    OpenApiCredentials creds_;
    OpenApiToken token_;
    /// Serializes every public entry point.
    mutable std::mutex mutex_;
    bool logged_in_ = false;

    // ---- Lock-free snapshot state (written under mutex_, read anywhere) ----
    std::atomic<bool> snapshot_configured_{false};
    std::atomic<bool> snapshot_has_token_{false};
    std::atomic<bool> snapshot_login_in_flight_{false};
    /// Mirrored token_.ExpireMs() for lock-free expiry checks; 0 when no usable token is held.
    std::atomic<int64_t> snapshot_token_expire_ms_{0};
    /// Guards the cached snapshot strings.
    mutable std::mutex snapshot_strings_mutex_;
    std::string snapshot_uid_;
    std::string snapshot_endpoint_;

    /// Single-flight guard: true for the whole "oa_login" task lifetime.
    std::atomic<bool> login_task_running_{false};

    /// Abort flag for the RUNNING background login only; LoginInBackground
    /// resets it when spawning the next attempt.
    std::atomic<bool> login_abort_{false};

    /// Refreshes snapshot_* from the guarded state (call under mutex_).
    void UpdateSnapshotLocked();

    HttpClient http_client_;
};

}  // namespace tuya_cloud
