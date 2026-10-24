#include "tuya_cloud/cloud_auth.h"

#include <esp_log.h>

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <memory>
#include <string>
#include <utility>

#include "sys/settings.h"
#include "tuya_cloud/constants.h"
#include "tuya_cloud/customer_api.h"
#include "tuya_cloud/http_client.h"
#include "tuya_cloud/time_sync.h"
#include "tuya_crypto/json_writer.h"

#include "login_control.h"

static const char* TAG = "CloudAuth";

namespace tuya_cloud {

namespace {

// ---- Login flow ----

/// QR token request: 1 attempt + up to 3 retries, 2s apart.
constexpr int kQrRequestRetries = 3;
constexpr uint32_t kQrRequestRetryDelayMs = 2000;

/// Poll cadence: 1-5s (3s chosen here).
constexpr uint32_t kPollIntervalMs = 3000;

/// 60 polls x 3s ≈ 180s per QR; max 2 refreshes, then kFailed.
constexpr int kPollsPerQr = 60;
constexpr int kMaxQrRefreshes = 2;

/// "tuyaSmart--qrLogin?token={qrcode}" — the payload the app scans.
constexpr char kQrPayloadPrefix[] = "tuyaSmart--qrLogin?token=";

/// Login task: detached, 12KB stack (esp_http_client + mbedTLS + cJSON + NVS).
constexpr uint32_t kLoginTaskStackBytes = 12288;
constexpr UBaseType_t kLoginTaskPriority = 3;

/// Bounded wait for a previous login task to stop (stragglers handled by the generation guard).
constexpr uint32_t kLoginStopWaitMs = 3000;

// ---- Boot path (StartCloudBoot) ----

/// Time sync: 15s per attempt, 1 + 5 retries 30s apart.
constexpr uint32_t kTimeSyncTimeoutMs = 15000;
constexpr int kTimeSyncRetries = 5;
constexpr uint32_t kTimeSyncRetrySpacingMs = 30000;

/// Sanity round (load + configure + homes verify): 1 + 5 retries, 30s apart; time sync not
/// repeated.
constexpr int kCloudBootRetries = 5;
constexpr uint32_t kCloudBootRetryDelayMs = 30000;

const char* StateName(AuthState state) {
    switch (state) {
        case AuthState::kIdle: return "Idle";
        case AuthState::kWaitingScan: return "WaitingScan";
        case AuthState::kLinked: return "Linked";
        case AuthState::kFailed: return "Failed";
    }
    return "?";
}

/// Heap argument block (the task outlives the StartLogin call).
struct LoginTaskArgs {
    CloudAuthManager* self;
    std::string user_code;
    int generation;
};

}  // namespace

void CloudAuthManager::SetState(AuthState state, const std::string& error) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (status_.state == state && status_.error == error) {
            return;  // no change -> no notification
        }
        status_.state = state;
        status_.error = error;
    }
    if (error.empty()) {
        ESP_LOGI(TAG, "Auth state -> %s", StateName(state));
    } else {
        ESP_LOGI(TAG, "Auth state -> %s (%s)", StateName(state), error.c_str());
    }
    PublishStatus();
}

void CloudAuthManager::PublishStatus() {
    AuthStatus snapshot;
    std::function<void(const AuthStatus&)> listener;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        snapshot = status_;
        listener = listener_;
    }
    if (listener) {
        // Invoked on THIS task — subscribers must marshal (LISTENER CONTRACT).
        listener(snapshot);
    }
}

AuthStatus CloudAuthManager::GetStatus() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return status_;
}

bool CloudAuthManager::NeedsProvisioning() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return !has_credentials_;
}

void CloudAuthManager::SetListener(std::function<void(const AuthStatus&)> cb) {
    std::lock_guard<std::mutex> lock(mutex_);
    listener_ = std::move(cb);
}

void CloudAuthManager::StartLogin(const std::string& user_code) {
    if (user_code.empty()) {
        ESP_LOGW(TAG, "StartLogin with an empty user_code — ignoring");
        return;
    }

    // Restart semantics: stop a running attempt first (cancel_ + the
    // generation bump below invalidates any straggler).
    if (task_running_.load()) {
        cancel_.store(true);
        for (uint32_t waited_ms = 0; waited_ms < kLoginStopWaitMs && task_running_.load();
             waited_ms += 20) {
            vTaskDelay(pdMS_TO_TICKS(20));
        }
        if (task_running_.load()) {
            ESP_LOGW(TAG, "Previous login task still winding down — restarting anyway");
        }
    }

    cancel_.store(false);
    const int generation = login_generation_.fetch_add(1) + 1;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        status_.user_code = user_code;
        status_.qr_payload.clear();
        status_.username.clear();
        status_.error.clear();
    }

    // Wire the CustomerApi callbacks before the first session (Tuya refresh
    // tokens are single-use). Guarded, so idempotent.
    EnsureApiCallbacksWired();

    // Ownership: released into the task below; failures free it here.
    auto args = std::make_unique<LoginTaskArgs>(this, user_code, generation);
    task_running_.store(true);
    const BaseType_t created = xTaskCreate(
        [](void* arg) {
            std::unique_ptr<LoginTaskArgs> task_args(static_cast<LoginTaskArgs*>(arg));
            CloudAuthManager* self = task_args->self;
            const int generation = task_args->generation;
            std::string user_code = std::move(task_args->user_code);
            // Free before AuthTask — nothing after vTaskDelete(nullptr) runs.
            task_args.reset();
            self->AuthTask(std::move(user_code), generation);
            // Only the CURRENT attempt clears the running flag.
            if (self->login_generation_.load() == generation) {
                self->task_running_.store(false);
            }
            vTaskDelete(nullptr);  // detached: the task deletes itself
        },
        "tuya_login", kLoginTaskStackBytes, args.get(), kLoginTaskPriority, nullptr);
    if (created != pdPASS) {
        if (login_generation_.load() == generation) {
            task_running_.store(false);
        }
        ESP_LOGE(TAG, "xTaskCreate(tuya_login) failed");
        SetState(AuthState::kFailed, "failed to create the login task");
        return;
    }
    args.release();  // handed off — the task owns the args now
    ESP_LOGI(TAG, "Login started (user_code=%.2s…, task stack %u B)", user_code.c_str(),
             static_cast<unsigned>(kLoginTaskStackBytes));
}

void CloudAuthManager::Cancel() {
    cancel_.store(true);
    ESP_LOGI(TAG, "Login cancel requested");
}

void CloudAuthManager::AuthTask(std::string user_code, int generation) {
    SetState(AuthState::kWaitingScan);

    // Initial QR + up to kMaxQrRefreshes fresh ones.
    const int qr_rounds = kMaxQrRefreshes + 1;
    for (int round = 0; round < qr_rounds; ++round) {
        if (ShouldStopLogin(generation)) {
            ExitLoginTask(generation);
            return;
        }

        // --- QR token request (plaintext POST), retried ---
        LoginQrResult qr;
        bool qr_ok = false;
        for (int attempt = 0; attempt <= kQrRequestRetries; ++attempt) {
            if (ShouldStopLogin(generation)) {
                ExitLoginTask(generation);
                return;
            }
            if (attempt > 0) {
                vTaskDelay(pdMS_TO_TICKS(kQrRequestRetryDelayMs));
            }
            qr = LoginControlQrRequest(user_code);
            if (qr.network_ok && qr.success) {
                qr_ok = true;
                break;
            }
            ESP_LOGW(TAG, "QR request failed (attempt %d/%d): code=%lld msg=%s", attempt + 1,
                     kQrRequestRetries + 1, static_cast<long long>(qr.code), qr.msg.c_str());
        }
        if (!qr_ok) {
            const std::string reason = qr.msg.empty() ? "unknown error" : qr.msg.c_str();
            SetState(AuthState::kFailed, "QR token request failed: " + reason);
            return;
        }

        // Cancel/supersede checkpoint before the publish: a straggler
        // must not overwrite the NEW attempt's QR payload.
        if (ShouldStopLogin(generation)) {
            ExitLoginTask(generation);
            return;
        }

        // Publish the QR payload (state unchanged — notifies explicitly).
        {
            std::lock_guard<std::mutex> lock(mutex_);
            status_.qr_payload = kQrPayloadPrefix;
            status_.qr_payload += qr.qrcode;
        }
        ESP_LOGI(TAG, "QR ready (round %d/%d) — polling every %us for ~%us", round + 1, qr_rounds,
                 static_cast<unsigned>(kPollIntervalMs / 1000),
                 static_cast<unsigned>(kPollIntervalMs * kPollsPerQr / 1000));
        PublishStatus();

        // --- Poll loop (not-scanned and transient failures both poll on) ---
        for (int poll = 0; poll < kPollsPerQr; ++poll) {
            if (poll > 0) {
                vTaskDelay(pdMS_TO_TICKS(kPollIntervalMs));
            }
            if (ShouldStopLogin(generation)) {
                ExitLoginTask(generation);
                return;
            }
            LoginPollResult result = LoginControlPoll(qr.qrcode, user_code);
            if (ShouldStopLogin(generation)) {
                // A restart/cancel owns the state machine now.
                ExitLoginTask(generation);
                return;
            }
            if (result.network_ok && result.success) {
                // Narrows the window before the NVS write; the save gate covers the rest.
                if (ShouldStopLogin(generation)) {
                    ExitLoginTask(generation);
                    return;
                }
                Credentials creds;
                if (!creds.SaveLoginInfo(user_code, result.terminal_id, result.endpoint,
                                         result.token)) {
                    // Suppressed (factory reset): never arm the app for the wiped account.
                    ExitLoginTask(generation);
                    return;
                }
                CustomerApi::GetInstance().Configure(result.endpoint, result.token);
                // Account name shares the credentials' namespace; the gate keeps a racing
                // factory reset from resurrecting it after the wipe.
                if (!result.username.empty() && !Credentials::PersistenceSuppressed()) {
                    sys::Settings settings("tuya", true);
                    settings.SetString("username", result.username);
                }
                {
                    std::lock_guard<std::mutex> lock(mutex_);
                    status_.username = result.username;
                    has_credentials_ = true;
                }
                SetState(AuthState::kLinked);
                return;
            }
            // Not approved yet — poll on.
        }

        if (round + 1 < qr_rounds) {
            ESP_LOGW(TAG, "QR not scanned within %us — requesting a fresh one",
                     static_cast<unsigned>(kPollIntervalMs * kPollsPerQr / 1000));
            std::lock_guard<std::mutex> lock(mutex_);
            status_.qr_payload.clear();  // stale; the next round publishes a new one
        }
    }
    SetState(AuthState::kFailed, "login timed out: the QR was never scanned/approved");
}

bool CloudAuthManager::ShouldStopLogin(int generation) const {
    if (cancel_.load()) {
        return true;
    }
    return login_generation_.load() != generation;
}

void CloudAuthManager::ExitLoginTask(int generation) {
    if (login_generation_.load() != generation) {
        // Superseded — the new task owns the state; exit silently.
        ESP_LOGI(TAG, "Login task superseded — exiting silently");
        return;
    }
    // Cancel() path: back to kIdle, no error.
    {
        std::lock_guard<std::mutex> lock(mutex_);
        status_.qr_payload.clear();
        status_.user_code.clear();
    }
    SetState(AuthState::kIdle);
}

bool CloudAuthManager::SyncTimeWithRetries() {
    TimeSync time_sync;
    for (int retry = 0; retry <= kTimeSyncRetries; ++retry) {
        if (retry > 0) {
            ESP_LOGW(TAG, "Time sync retry %d/%d in %us", retry, kTimeSyncRetries,
                     static_cast<unsigned>(kTimeSyncRetrySpacingMs / 1000));
            vTaskDelay(pdMS_TO_TICKS(kTimeSyncRetrySpacingMs));
        }
        if (time_sync.SyncBlocking(kTimeSyncTimeoutMs)) {
            return true;
        }
    }
    ESP_LOGE(TAG, "Time sync failed after %d attempts — aborting cloud boot (state unchanged)",
             kTimeSyncRetries + 1);
    return false;
}

bool CloudAuthManager::StartCloudBoot() {
    // Inspect NVS FIRST so a time-sync failure lands in the right state:
    // a provisioned device must park in Idle (retryable), not NeedsSetup.
    {
        Credentials creds;
        std::lock_guard<std::mutex> lock(mutex_);
        has_credentials_ = creds.HasToken();
    }

    // Step 1: wall clock (X-time / token expiry are ms-epoch). Failure is
    // transient — state untouched, caller decides.
    if (!SyncTimeWithRetries()) {
        return false;
    }

    // Steps 2-5: load credentials, wire callbacks, configure, verify.
    for (int retry = 0; retry <= kCloudBootRetries; ++retry) {
        if (retry > 0) {
            ESP_LOGW(TAG, "Cloud boot retry %d/%d in %us", retry, kCloudBootRetries,
                     static_cast<unsigned>(kCloudBootRetryDelayMs / 1000));
            vTaskDelay(pdMS_TO_TICKS(kCloudBootRetryDelayMs));
        }

        // Step 2: credentials. Absent/partial -> NeedsSetup (no retry).
        Credentials creds;
        if (!creds.Load()) {
            {
                std::lock_guard<std::mutex> lock(mutex_);
                has_credentials_ = false;
            }
            SetState(AuthState::kIdle);  // announces the reset if state differed
            ESP_LOGI(TAG, "No stored credentials — device needs setup (QR login)");
            return false;
        }
        {
            std::lock_guard<std::mutex> lock(mutex_);
            has_credentials_ = true;
            status_.user_code = creds.user_code();
            // Restore the persisted account name; only fill when empty
            // (a same-boot re-link may hold the fresh poll value).
            if (status_.username.empty()) {
                sys::Settings settings("tuya", false);
                status_.username = settings.GetString("username", "");
            }
        }

        // Steps 3+5: token persistence on refresh + the "sign invalid" hook.
        EnsureApiCallbacksWired();

        // Idempotent: also clears a stale auth-failed flag.
        CustomerApi::GetInstance().Configure(creds.endpoint(), creds.token());

        // Step 4: sanity check — the homes list through the full pipeline.
        ApiResponse resp = CustomerApi::GetInstance().Request(HttpMethod::kGet, kPathHomes, "", "");
        if (resp.ok) {
            ESP_LOGI(TAG, "Cloud boot verified via %.*s — account linked",
                     static_cast<int>(kPathHomes.size()), kPathHomes.data());
            SetState(AuthState::kLinked);
            return true;
        }
        // "sign invalid": HandleAuthFailed() already ran inside the failing request;
        // detect it by the message or the credentials being gone.
        if (resp.msg.find("sign invalid") != std::string::npos || !has_credentials_) {
            HandleAuthFailed();
            return false;
        }
        ESP_LOGW(TAG, "Cloud sanity check failed: %s — retrying", resp.msg.c_str());
    }

    // Transient exhaustion: state untouched (kIdle) — the caller decides.
    ESP_LOGE(TAG, "Cloud boot failed after %d attempts (transient) — staying unlinked",
             kCloudBootRetries + 1);
    return false;
}

void CloudAuthManager::HandleAuthFailed() {
    // "sign invalid": the stored credentials are dead — clear them and
    // require a new QR login. Idempotent.
    ESP_LOGE(TAG, "Auth failure (sign invalid) — clearing credentials; re-provisioning required");
    Credentials creds;
    creds.Clear();
    CustomerApi::GetInstance().Reset();
    {
        std::lock_guard<std::mutex> lock(mutex_);
        has_credentials_ = false;
        status_.username.clear();
        status_.qr_payload.clear();
    }
    SetState(AuthState::kIdle, "authentication failed (sign invalid)");
}

void CloudAuthManager::Unlink() {
    Credentials creds;
    const bool loaded = creds.Load();
    const std::string endpoint = creds.endpoint();
    const TokenInfo token = creds.token();
    const std::string terminal_id = creds.terminal_id();
    creds.Clear();
    {
        std::lock_guard<std::mutex> lock(mutex_);
        has_credentials_ = false;
        status_.user_code.clear();
        status_.qr_payload.clear();
        status_.username.clear();
    }

    // Clear-first: the straggler's cancel-path kIdle publish must see NeedsProvisioning()
    // true; the post-wait generation bump silences stragglers that outlived the wait.
    if (task_running_.load()) {
        cancel_.store(true);
        for (uint32_t waited_ms = 0; waited_ms < kLoginStopWaitMs && task_running_.load();
             waited_ms += 20) {
            vTaskDelay(pdMS_TO_TICKS(20));
        }
        if (task_running_.load()) {
            ESP_LOGW(TAG, "Login task still winding down — unlink proceeds");
        }
        (void)login_generation_.fetch_add(1);
    }

    // Terminal revoke, best effort — the result is ignored; removal always proceeds.
    // KNOWN DEVIATION: the body uses the stored access token while Request() may refresh first.
    if (loaded) {
        CustomerApi::GetInstance().Configure(endpoint, token);
        tuya_crypto::JsonWriter writer;
        writer.BeginObject();
        writer.Key("accessToken");
        writer.String(token.access_token);
        writer.Key("terminalId");
        writer.String(terminal_id);
        writer.EndObject();
        if (writer.HasError()) {
            ESP_LOGE(TAG, "terminal-revoke body build failed");
        } else {
            ApiResponse resp = CustomerApi::GetInstance().Request(
                HttpMethod::kPost, kPathTerminalRevoke, "", writer.Str());
            if (resp.ok) {
                ESP_LOGI(TAG, "Terminal revoke accepted");
            } else {
                ESP_LOGW(TAG, "Terminal revoke failed (ignored): %s", resp.msg.c_str());
            }
        }
    } else {
        ESP_LOGW(TAG, "Unlink without stored credentials — skipping the revoke call");
    }
    CustomerApi::GetInstance().Reset();
    // The revoke may refresh the token, and the listener re-persists: erase again.
    {
        sys::Settings tuya("tuya", true);
        if (!tuya.GetString("access_token").empty()) {
            tuya.EraseAll();
            ESP_LOGW(TAG, "Revoke refresh re-saved credentials — erased again");
        }
    }
    SetState(AuthState::kIdle);
}

void CloudAuthManager::SuppressTokenPersistence() {
    // One-way until reboot; only the factory reset needs it.
    suppress_token_persistence_.store(true);
    // The login task writes through Credentials, not the refresh listener.
    Credentials::SuppressPersistence();
    ESP_LOGW(TAG, "Token persistence suppressed until reboot");
}

void CloudAuthManager::EnsureApiCallbacksWired() {
    bool expected = false;
    if (!api_callbacks_wired_.compare_exchange_strong(expected, true)) {
        return;  // already wired
    }
    CustomerApi& api = CustomerApi::GetInstance();

    // Token persistence on every refresh. The empty guard matters: Reset()
    // deliberately stores an EMPTY token, which must not wipe the NVS keys.
    api.SetTokenListener([this](const TokenInfo& info) {
        if (info.access_token.empty() || info.refresh_token.empty()) {
            return;
        }
        if (suppress_token_persistence_.load()) {
            ESP_LOGW(TAG, "Token refresh not persisted (factory reset)");
            return;
        }
        Credentials creds;
        creds.SaveTokenInfo(info);
    });

    // "sign invalid" anywhere in the encrypted pipeline -> auth is dead.
    api.SetOnAuthFailed([]() { CloudAuthManager::GetInstance().HandleAuthFailed(); });
}

}  // namespace tuya_cloud
