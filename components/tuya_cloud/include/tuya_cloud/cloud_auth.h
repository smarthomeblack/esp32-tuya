#pragma once

#include <atomic>
#include <functional>
#include <mutex>
#include <string>

#include "tuya_cloud/credentials.h"

namespace tuya_cloud {

/// Login-engine state (QR login flow).
enum class AuthState {
    kIdle,         ///< No login attempt running; not linked (or never was).
    kWaitingScan,  ///< QR issued; waiting for the user to scan + approve.
    kLinked,       ///< Credentials stored, CustomerApi configured and verified.
    kFailed,       ///< Login attempt exhausted its retries (see error).
};

/// Snapshot of the login engine (mutex-guarded copy; safe from any task).
struct AuthStatus {
    AuthState state = AuthState::kIdle;
    std::string user_code;   ///< current login attempt (or the stored user_code once linked)
    std::string qr_payload;  ///< "tuyaSmart--qrLogin?token=..."
    std::string username;    ///< account name from the poll result (persisted, restored at boot)
    std::string error;       ///< last failure description
};

/// QR-login engine, boot-time cloud bring-up and auth-failure handler.
/// LISTENER CONTRACT: callbacks run synchronously on the auth task — fast, no UI state.
class CloudAuthManager {
public:
    static CloudAuthManager& GetInstance() {
        static CloudAuthManager instance;
        return instance;
    }
    CloudAuthManager(const CloudAuthManager&) = delete;
    CloudAuthManager& operator=(const CloudAuthManager&) = delete;

    /// Starts the detached login task (restarts any running one); empty user_code rejected.
    void StartLogin(const std::string& user_code);

    /// Asks a running login task to stop (kIdle when still current).
    void Cancel();

    /// Mutex-guarded snapshot of the login engine state.
    AuthStatus GetStatus() const;

    /// True when no stored credentials — the device needs the QR login flow.
    bool NeedsProvisioning() const;

    /// BLOCKING — never call from an httpd handler. True only when the account ends up kLinked;
    /// a transient failure leaves state untouched (caller decides to retry).
    bool StartCloudBoot();

    /// Clears credentials first, then a best-effort terminal revoke; resets CustomerApi, kIdle.
    void Unlink();

    /// Clears dead credentials ("sign invalid"), resets the API, kIdle. Idempotent.
    void HandleAuthFailed();

    /// One-way until reboot: stops the token-refresh listener from persisting to NVS
    /// (a factory reset's in-flight refresh must not recreate the erased keys).
    void SuppressTokenPersistence();

    /// Registers the state-change listener (see LISTENER CONTRACT above).
    void SetListener(std::function<void(const AuthStatus&)> cb);

private:
    CloudAuthManager() = default;

    /// Updates state (+error) and notifies the listener when it changed.
    void SetState(AuthState state, const std::string& error = "");

    /// Notifies the listener with the current snapshot (QR payload only).
    void PublishStatus();

    /// Login task body; a newer StartLogin supersedes this generation (silent exit).
    void AuthTask(std::string user_code, int generation);

    /// Cancel() was called or a newer StartLogin superseded this attempt.
    bool ShouldStopLogin(int generation) const;

    /// Exit path for a cancelled/superseded login.
    void ExitLoginTask(int generation);

    /// Wires the CustomerApi token-persistence + auth-failed callbacks once (guarded).
    void EnsureApiCallbacksWired();

    /// Time sync with its retry loop; false after all retries failed.
    bool SyncTimeWithRetries();

    mutable std::mutex mutex_;  // guards status_, has_credentials_, listener_
    AuthStatus status_;
    bool has_credentials_ = false;
    std::function<void(const AuthStatus&)> listener_;

    std::atomic<bool> cancel_{false};
    std::atomic<int> login_generation_{0};
    std::atomic<bool> task_running_{false};
    std::atomic<bool> api_callbacks_wired_{false};
    std::atomic<bool> suppress_token_persistence_{false};
};

}  // namespace tuya_cloud
