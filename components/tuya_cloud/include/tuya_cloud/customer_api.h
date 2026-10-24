#pragma once

#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <string_view>

#include "tuya_cloud/api_response.h"
#include "tuya_cloud/credentials.h"
#include "tuya_cloud/http_client.h"
#include "tuya_cloud/token_manager.h"

namespace tuya_cloud {

/// Starts the total budget for one user-facing command/scene chain: absolute
/// esp_timer ms from the chain's ENTRY (SendDeviceCommands/TriggerScene).
int64_t StartCommandDeadline();

/// True when a chain deadline has passed; 0 = unbounded (background chains).
bool DeadlineExceeded(int64_t deadline_ms);

/// Tuya "device sharing" customer API: encrypted/signed request pipeline with token refresh.
/// THREAD-SAFETY: Request() serializes on request_mutex_; ExecuteRequest skips it (refresh runs
/// inside).
class CustomerApi {
public:
    static CustomerApi& GetInstance() {
        static CustomerApi instance;
        return instance;
    }
    // Default-constructible (stack/test instances); copying is not.
    CustomerApi() = default;
    CustomerApi(const CustomerApi&) = delete;
    CustomerApi& operator=(const CustomerApi&) = delete;

    /// Called with the refreshed token so the owner can persist it.
    using TokenListener = std::function<void(const TokenInfo&)>;

    /// Fired when a response reports "sign invalid" — credentials are dead, user must
    /// re-authenticate.
    using AuthFailedCallback = std::function<void()>;

    /// Sets the endpoint (used verbatim) and current token; both non-empty mark the API configured.
    void Configure(const std::string& endpoint, const TokenInfo& token);

    bool IsConfigured() const;

    /// Clears the Configure() state. NOTE: fires the token listener with an EMPTY token — ignore
    /// it.
    void Reset();

    void SetTokenListener(TokenListener listener);
    void SetOnAuthFailed(AuthFailedCallback callback);

    /// True after a "sign invalid" response (until the next Configure()).
    bool AuthFailed() const { return auth_failed_; }

    /// Full pipeline: refresh-if-needed, signing, HTTP, response handling.
    /// `params_json`/`body_json` are pre-serialized compact JSON ("" = absent). Thread-safe.
    /// `deadline_ms`: absolute esp_timer-ms chain cap (0 = unbounded); legs skip past it.
    ApiResponse Request(HttpMethod method, std::string_view path,
                        const std::string& params_json = "", const std::string& body_json = "",
                        int64_t deadline_ms = 0);

private:
    /// The pipeline without the refresh stage (also used by the internal refresh call).
    ApiResponse ExecuteRequest(HttpMethod method, std::string_view path,
                               const std::string& params_json, const std::string& body_json,
                               int64_t deadline_ms);

    /// Single-flight token refresh; failure is swallowed — the request proceeds with the old token.
    void RefreshTokenIfNeeded(int64_t deadline_ms);

    HttpClient http_client_;
    TokenManager token_manager_;
    /// Written by Configure/Reset while a serialized Request may read it.
    /// Lock order when both are held: request_mutex_ -> endpoint_mutex_.
    std::string endpoint_;
    mutable std::mutex endpoint_mutex_;
    std::mutex refresh_mutex_;
    /// Serializes the public Request() (held across refresh + execute);
    /// the internal ExecuteRequest path does NOT take it.
    std::mutex request_mutex_;
    AuthFailedCallback on_auth_failed_;
    bool auth_failed_ = false;
};

}  // namespace tuya_cloud
