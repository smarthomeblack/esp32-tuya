#include "application.h"

#include <esp_log.h>
#include <esp_system.h>
#include <esp_task_wdt.h>

#include <freertos/task.h>

#include <atomic>
#include <string>

#include "boot_self_test.h"
#include "hub_core/device_registry.h"
#include "hub_core/device_store.h"
#include "hub_core/main_thread.h"
#include "hub_core/psram_resource.h"
#include "hub_events.h"
#include "hub_services/command_service.h"
#include "hub_services/discovery.h"
#include "sys/cpu_monitor.h"
#include "sys/led_status.h"
#include "sys/log_capture.h"
#include "sys/settings.h"
#include "sys/system_info.h"
#include "sys/wifi_provision.h"
#include "sys/wifi_station.h"
#include "tuya_cloud/cloud_auth.h"
#include "tuya_cloud/credentials.h"
#include "tuya_mqtt/tuya_mqtt_client.h"
#include "web_server/api_auth.h"
#include "web_server/web_server.h"

static const char* TAG = "Application";

static const char* kStateNames[kDeviceStateCount] = {
    "Starting", "WifiConfig", "NeedsSetup", "Idle", "CloudConnecting", "Linked", "FatalError",
};

constexpr int64_t kCloudRetryPeriodMs = 30000;
constexpr int64_t kCloudSlowRetryMs = 10LL * 60 * 1000;
constexpr uint32_t kMaxCloudBootRetries = 5;
// MQTT push self-heal backoff cap: every 30 s until the client runs again.
constexpr int64_t kMqttPushRetryMaxMs = 30000;
// Schedule() queue cap; overflow drops the OLDEST job.
constexpr size_t kScheduleQueueCap = 512;
// Critical-queue sanity cap: latched producers keep it at a handful; a
// breach means a latch leak (logged loudly, jobs are still never dropped).
constexpr size_t kScheduleCriticalQueueCap = 16;

namespace {

/// /pen vs /sta is frozen at subscribe time — one Stop+Start cycle; the latch
/// is held for the whole cycle, so producers landing here are the next trigger.
void ScheduleMqttResubscribe(Application& app, std::atomic<bool>& resubscribe_pending,
                             int64_t& next_retry_ms) {
    app.ScheduleCritical([&app, &resubscribe_pending, &next_retry_ms] {
        ESP_LOGI(TAG, "MQTT resubscribe cycle (new device(s) committed)");
        tuya_mqtt::TuyaMqttClient::GetInstance().Stop();  // ~5s, main loop only
        // Cleared before the Start: Stop may have timed out (the task still
        // runs), so a refused Start must let the next producer re-trigger.
        resubscribe_pending.store(false);
        if (!tuya_mqtt::TuyaMqttClient::GetInstance().Start()) {
            // Stop timed out mid-shutdown — the clock-tick self-heal finishes it.
            next_retry_ms =
                static_cast<int64_t>(sys::SystemInfo::GetUptimeMs()) + kMqttPushRetryFirstMs;
            ESP_LOGW(TAG, "MQTT start refused after resubscribe — self-heal retry");
        }
    });
}

}  // namespace

std::string Application::CurrentAccountUid() {
    tuya_cloud::Credentials creds;
    creds.Load();  // populates token_.uid even on a partial set
    return creds.token().uid;
}

Application::Application() {
    event_group_ = xEventGroupCreate();
    if (event_group_ == nullptr) {
        // Cannot happen under normal operation (OOM at boot) — unrecoverable.
        ESP_LOGE(TAG, "Failed to create event group");
        abort();
    }

    // 1Hz clock timer — the callback only sets an event bit (never does work).
    const esp_timer_create_args_t timer_args = {
        .callback =
            [](void* arg) {
                auto* app = static_cast<Application*>(arg);
                xEventGroupSetBits(app->event_group_, MAIN_EVENT_CLOCK_TICK);
            },
        .arg = this,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "hub_clock",
        .skip_unhandled_events = true,
    };
    ESP_ERROR_CHECK(esp_timer_create(&timer_args, &clock_timer_));
}

void Application::Start() {
    bool expected = false;
    if (!started_.compare_exchange_strong(expected, true)) {
        ESP_LOGW(TAG, "Start() called more than once — ignoring");
        return;
    }
    // PSRAM pool as the pmr default BEFORE the self-tests below: they
    // build Devices (any earlier pmr container would pin the wrong pool).
    hub_core::InitHubPsramPmrDefault();
    sys::LedStatus::GetInstance().Start();
    SetState(kDeviceStateStarting);
    // Log sink + CPU sampler BEFORE the self-tests (capture their output).
    sys::LogCapture::Start();
    sys::CpuMonitor::Start();

    // Main event loop task; 12KB stack (HTTP + JSON jobs run here).
    BaseType_t ok = xTaskCreate(
        [](void* arg) {
            auto* app = static_cast<Application*>(arg);
            app->MainEventLoop();
            vTaskDelete(nullptr);
        },
        "main_event_loop", 3072 * 4, this, 3, nullptr);
    ESP_ERROR_CHECK(ok == pdPASS ? ESP_OK : ESP_FAIL);

    ESP_ERROR_CHECK(esp_timer_start_periodic(clock_timer_, 1000000));

    // Self-test gates: a failure parks in FatalError (no network).
    if (!boot_self_test::RunAllGates()) {
        SetState(kDeviceStateFatalError);
        return;
    }

    tuya_cloud::CloudAuthManager::GetInstance().SetListener(
        [](const tuya_cloud::AuthStatus& status) {
            // A link/unlink transition must survive a schedule-queue overflow.
            Application::GetInstance().ScheduleStateJob(
                [status] { Application::GetInstance().OnAuthStatus(status); });
        });

    hub_core::DiscoveryService::GetInstance().SetProgressListener(
        [](const hub_core::DiscoveryProgress& progress) {
            Application::GetInstance().Schedule(
                [progress] { Application::GetInstance().OnDiscoveryProgress(progress); });
        });

    hub_core::DeviceRegistry::GetInstance().SetEventListener(
        [](const hub_core::RegistryEventPayload& event) {
            Application::GetInstance().Schedule(
                [event] { Application::GetInstance().OnRegistryEvent(event); });
        });

    tuya_mqtt::TuyaMqttClient::GetInstance().SetStatusListener(
        [](const tuya_mqtt::MqttStatus& status) {
            Application::GetInstance().Schedule(
                [status] { Application::GetInstance().OnMqttStatus(status); });
        });

    // REST handlers marshal onto this loop; registered before StartNetwork.
    web_server::WebServer::GetInstance().SetScheduler(
        [](std::function<void()> job) { Application::GetInstance().Schedule(std::move(job)); });

    // Never-drop lane (initially unused) for future must-complete jobs.
    web_server::WebServer::GetInstance().SetCriticalScheduler([](std::function<void()> job) {
        Application::GetInstance().ScheduleCritical(std::move(job));
    });

    // POST /api/rooms/refresh: spawn the Tuya IoT room fetch off the httpd task.
    web_server::WebServer::GetInstance().SetRoomRefreshTrigger(
        []() { return Application::GetInstance().ScheduleRoomRefresh(); });

    // Provisioning blocks forever (the only exit is a reboot). The initial
    // API key must exist first — the setup portal displays it.
    web_server::rest_api::InitApiKey();
    StartNetwork();

    ESP_LOGI(TAG, "Hub ready (state=%s)", kStateNames[state_.load()]);
    ESP_LOGI(TAG, "Firmware v%s (IDF %s, reset reason: %s)",
             sys::SystemInfo::GetAppVersion().c_str(), sys::SystemInfo::GetIdfVersion().c_str(),
             sys::SystemInfo::GetResetReason().c_str());
}

void Application::StartNetwork() {
    bool force_ap = false;
    std::string ssid;
    {
        sys::Settings settings("wifi", true);
        force_ap = settings.GetBool("force_ap", false);
        if (force_ap) {
            // One-shot flag: consumed so the next boot takes the station path.
            settings.SetBool("force_ap", false);
        }
        ssid = settings.GetString("ssid", "");
    }

    if (ssid.empty() || force_ap) {
        SetState(kDeviceStateWifiConfig);
        // Never returns (the only exit is a reboot from the portal).
        sys::WifiProvision::GetInstance().Start();
        return;
    }

    auto& station = sys::WifiStation::GetInstance();
    if (!station.Start() || !station.WaitForConnected(60000)) {
        ESP_LOGW(TAG, "WiFi connection failed — entering provisioning mode");
        station.Stop();
        SetState(kDeviceStateWifiConfig);
        // Never returns (the only exit is a reboot from the portal).
        sys::WifiProvision::GetInstance().Start();
        return;
    }

    ESP_LOGI(TAG, "WiFi connected: ssid=%s ip=%s rssi=%d", station.GetSsid().c_str(),
             station.GetIp().c_str(), static_cast<int>(station.GetRssi()));

    // Up before cloud bring-up (UI reachable while the link comes up).
    // Not FatalError: the hub must stay alive for diagnostics logs.
    if (!web_server::WebServer::GetInstance().Start(80)) {
        ESP_LOGE(TAG, "Web server start failed — one retry in 1 s");
        ScheduleCritical([] {
            vTaskDelay(pdMS_TO_TICKS(1000));
            if (!web_server::WebServer::GetInstance().Start(80)) {
                ESP_LOGE(TAG, "Web server retry failed — REST/WS unavailable this boot");
            }
        });
    }

    // Blocking with internal retries — fine on the app_main task here.
    StartCloud();
}

bool Application::StartCloud() {
    // A queued retry can run after the state has moved on — re-validate.
    if (GetState() != kDeviceStateIdle && GetState() != kDeviceStateStarting) {
        ESP_LOGD(TAG, "StartCloud skipped — state moved on (state=%d)",
                 static_cast<int>(GetState()));
        return cloud_linked_.load();
    }
    if (cloud_linked_.load()) {
        return true;
    }
    // BLOCKING (network, internal retries) — never from an httpd handler.
    const bool boot_linked = tuya_cloud::CloudAuthManager::GetInstance().StartCloudBoot();
    // The main loop may wipe the credentials while this blocking verify runs:
    // a stale success must not publish Linked (it would push with no account).
    const bool linked =
        boot_linked && !tuya_cloud::CloudAuthManager::GetInstance().NeedsProvisioning();
    if (boot_linked && !linked) {
        ESP_LOGW(TAG, "Cloud boot verified but credentials are gone — staying unprovisioned");
    }
    if (linked && lost_link_relink_) {
        // Re-verify after a lost link. The auth state is still kLinked, so no
        // listener event arrives: re-link here and end this attempt first.
        restoring_registry_.store(false);
        lost_link_relink_ = false;
        ESP_LOGW(TAG, "Cloud re-verified after a lost link — relinking the push session");
        StartPushSession();
        return true;
    }
    // Restoring latch: holds queued saves off an empty snapshot while the
    // wipe runs. Paired (not a bare clear) so a concurrent restore's own armed
    // clear landing after it still ends with saves enabled.
    const bool was_linked = cloud_linked_.load();
    restoring_registry_.store(true);
    cloud_linked_.store(linked);
    // The retry window starts when this attempt ends (spacing from completion);
    // a later slow-cadence window (armed by the tick) is never pulled forward.
    const int64_t now_ms = static_cast<int64_t>(sys::SystemInfo::GetUptimeMs());
    if (cloud_next_retry_ms_.load() < now_ms + kCloudRetryPeriodMs) {
        cloud_next_retry_ms_.store(now_ms + kCloudRetryPeriodMs);
    }

    if (linked) {
        SetState(kDeviceStateLinked);
        // Restore persisted OpenAPI credentials/token (no network here).
        {
            tuya_cloud::OpenApiCredentials oa_creds;
            if (tuya_cloud::OpenApiCredentialsStore::Load(oa_creds)) {
                tuya_cloud::OpenApiClient::GetInstance().Configure(oa_creds);
                tuya_cloud::OpenApiClient::GetInstance().LoadToken();
                ESP_LOGI(TAG, "Tuya IoT client restored from NVS (uid=%s)",
                         tuya_cloud::OpenApiClient::GetInstance().snapshot_uid().c_str());
            }
        }
        // The restored token may be expired — spawn the re-login now.
        if (tuya_cloud::OpenApiClient::GetInstance().snapshot_configured() &&
            !tuya_cloud::OpenApiClient::GetInstance().snapshot_has_token()) {
            tuya_cloud::OpenApiClient::GetInstance().LoginInBackground();
        }
        // Restore from the snapshot first; scan only when unrestorable.
        // `was_linked` came from before this attempt set cloud_linked_.
        if (!RestoreRegistryFromStore(was_linked)) {
            StartDeviceDiscovery();
        }
        // Rooms ride the Tuya IoT credential (a no-op while it is not connected).
        ScheduleRoomRefresh();
    } else if (tuya_cloud::CloudAuthManager::GetInstance().NeedsProvisioning()) {
        SetState(kDeviceStateNeedsSetup);
    } else {
        ScheduleStoreSave("cloud_fail");  // pre-wipe snapshot, fallback only
        // A NULL snapshot: runs on the cloud-boot task. Set before the wipe so
        // its ClearAll marshals job-less (the registry is already empty); that
        // ClearAll also re-arms the restore, so the retry reads the snapshot
        // before the boot retry re-links.
        SetState(kDeviceStateIdle);
        // clear-before-set: the retry latch stays clear-monotonic.
        if (cloud_linked_.exchange(false) || was_linked) {
            // The previous session's push cannot be healed: a re-verify that
            // succeeds again re-notifies kLinked only on an auth-state change.
            ESP_LOGW(TAG, "Cloud re-verify failed after a lost link — resetting the push session");
            hub_core::DeviceRegistry::GetInstance().ClearAll();
            mqtt_push_started_ = false;
            registry_restore_done_.store(false);
            // Fail-closed: no watchdog; if the snapshot is really gone the boot
            // retry brings the home back with a rescan.
            mqtt_push_next_retry_ms_ = 0;
            // The boot retry (Idle + tick) re-verifies, then re-links by itself.
            lost_link_relink_ = true;
        }
    }
    // After the branch's own scheduling, so a queued save still sees the hold.
    restoring_registry_.store(false);
    return linked;
}

void Application::OnAuthStatus(const tuya_cloud::AuthStatus& status) {
    // The link event is marshalled from the verify task, so it can be stale:
    // never publish linked once the credentials are gone (no push, no wipe).
    if (status.state == tuya_cloud::AuthState::kLinked &&
        tuya_cloud::CloudAuthManager::GetInstance().NeedsProvisioning()) {
        ESP_LOGW(TAG, "Auth reported linked but credentials are gone — ignoring");
        return;
    }
    // WS event first — SetState below fires a second status event.
    web_server::WebServer::GetInstance().BroadcastAuthStatus(status);
    switch (status.state) {
        case tuya_cloud::AuthState::kWaitingScan: SetState(kDeviceStateCloudConnecting); break;
        case tuya_cloud::AuthState::kLinked: StartPushSession(); break;
        case tuya_cloud::AuthState::kFailed: SetState(kDeviceStateNeedsSetup); break;
        case tuya_cloud::AuthState::kIdle:
            if (tuya_cloud::CloudAuthManager::GetInstance().NeedsProvisioning()) {
                // Credentials gone: stop account services, reset latches
                // (registry+store survive; a different-account relink wipes them).
                tuya_mqtt::TuyaMqttClient::GetInstance().Stop();
                mqtt_push_started_ = false;
                mqtt_push_next_retry_ms_ = 0;
                registry_restore_done_ = false;
                store_dirty_ = false;
                SetState(kDeviceStateNeedsSetup);
            } else {
                SetState(kDeviceStateIdle);
            }
            break;
    }
}

void Application::StartPushSession() {
    // A lost established session may retry its registry; the first link attempt
    // must not (that re-arm would loop on an unrestorable snapshot).
    const bool session_was_linked = cloud_linked_.exchange(true);
    // A relink to a different account must not reuse the old registry.
    if (!StoreOwnerMatchesCurrentAccount()) {
        // Re-armed BEFORE the Clear events marshal: any restore the wipe ran
        // must not satisfy the restore below.
        registry_restore_done_.store(false);
        hub_core::DeviceRegistry::GetInstance().ClearAll();
        hub_core::DeviceStore::GetInstance().Clear();
        ClearStoreOwnerUid();
        // The old account's transport and scene cache must not carry over.
        tuya_mqtt::TuyaMqttClient::GetInstance().Stop();  // ~5 s, main loop only
        mqtt_push_started_ = false;
        mqtt_push_next_retry_ms_ = 0;
        mqtt_resubscribe_pending_.store(false);
        hub_core::CommandService::GetInstance().ClearScenes();
        ESP_LOGW(TAG, "Registry snapshot belongs to a different account — wiped");
    }
    SetState(kDeviceStateLinked);
    // QR-login landing: restore-from-snapshot or scan, then push —
    // even with 0 devices (the transport must come up before commit).
    if (hub_core::DeviceRegistry::GetInstance().Count() == 0 &&
        !RestoreRegistryFromStore(session_was_linked)) {
        StartDeviceDiscovery();
    }
    if (!mqtt_push_started_) {
        StartMqttPush();
    }
}

void Application::StartDeviceDiscovery() {
    // Spawns the task; a second active-run call is rejected inside.
    hub_core::DiscoveryService::GetInstance().Start();
}

void Application::OnDiscoveryProgress(const hub_core::DiscoveryProgress& progress) {
    web_server::WebServer::GetInstance().BroadcastDiscoveryProgress(progress);
    // Log cadence: one line per crossed 10% step, plus the terminal event.
    if (progress.failed) {
        ESP_LOGE(TAG, "Discovery failed: %s", progress.error.c_str());
        discovery_last_percent_ = -1;  // re-arm
        return;
    }
    if (progress.done) {
        ESP_LOGI(TAG, "Discovery done: %d home(s), %d device(s)", progress.homes_total,
                 progress.devices_done);
        discovery_last_percent_ = -1;  // re-arm
        // The scan only STAGES; the registry waits for the user's commit.
        ESP_LOGI(TAG, "Candidates staged — waiting for user selection");
        return;
    }
    const int step = progress.percent / 10;
    if (step != discovery_last_percent_) {
        discovery_last_percent_ = step;
        ESP_LOGI(TAG, "Discovery %d%%: homes %d/%d devices %d/%d", progress.percent,
                 progress.homes_done, progress.homes_total, progress.devices_done,
                 progress.devices_total);
    }
}

void Application::OnMqttStatus(const tuya_mqtt::MqttStatus& status) {
    // WS "mqtt_status" event — the UI's MQTT dot.
    web_server::WebServer::GetInstance().BroadcastMqttStatus(status);
    if (status.connected) {
        ESP_LOGI(TAG, "MQTT connected");
        // The broker pushes online/offline only on state changes —
        // refresh once per connect to heal stale online flags.
        SchedulePresenceRefresh();
    } else if (status.error_code != 0) {
        ESP_LOGW(TAG, "MQTT connection refused (rc=%d): %s", status.error_code,
                 status.error.c_str());
    } else if (!status.error.empty()) {
        ESP_LOGW(TAG, "MQTT disconnected: %s", status.error.c_str());
    } else {
        ESP_LOGI(TAG, "MQTT disconnected");
    }
}

void Application::OnRegistryEvent(const hub_core::RegistryEventPayload& event) {
    if (event.event == hub_core::RegistryEvent::kDeviceUpdated && event.changed_codes.empty()) {
        // Model/rename refresh: other sessions refetch /api/devices on the
        // device_added wire event (device_update with props:[] is a no-op).
        hub_core::RegistryEventPayload refresh = event;
        refresh.event = hub_core::RegistryEvent::kDeviceAdded;
        web_server::WebServer::GetInstance().BroadcastRegistryEvent(refresh);
    } else {
        web_server::WebServer::GetInstance().BroadcastRegistryEvent(event);
    }
    // Events double as persistence triggers; kDeviceAdded also starts the
    // push. Restore-time events skip saves (snapshot is already stored).
    switch (event.event) {
        case hub_core::RegistryEvent::kDeviceAdded:
            ESP_LOGI(TAG, "Device added: %s", event.dev_id.c_str());
            // Start the push once per link session (flag prevents a storm).
            if (!mqtt_push_started_) {
                StartMqttPush();
            } else if (!restoring_registry_.load() && !mqtt_resubscribe_pending_.exchange(true)) {
                ScheduleMqttResubscribe(*this, mqtt_resubscribe_pending_, mqtt_push_next_retry_ms_);
            }
            // First device stamps the owner uid; never re-stamped.
            if (GetStoreOwnerUid().empty()) {
                SetStoreOwnerUid(CurrentAccountUid());
            }
            // A home/device exists now — the kLinked-time fetch was skipped.
            FetchScenesIfEmpty();
            if (!restoring_registry_.load()) {
                ScheduleStoreSave("device_add");
            }
            break;
        case hub_core::RegistryEvent::kDeviceRemoved:
            ESP_LOGW(TAG, "Device removed: %s", event.dev_id.c_str());
            // Empty registry does NOT stop MQTT; the owner is void.
            if (hub_core::DeviceRegistry::GetInstance().Count() == 0) {
                ClearStoreOwnerUid();
            }
            ScheduleStoreSave("device_remove");
            break;
        case hub_core::RegistryEvent::kDeviceUpdated:
            ESP_LOGD(TAG, "Device updated: %s (%d code(s))", event.dev_id.c_str(),
                     static_cast<int>(event.changed_codes.size()));
            if (restoring_registry_.load()) {
                break;  // a restore upsert — already stored
            }
            if (event.changed_codes.empty()) {
                // Rename/entity selection: save now; an entity toggle flips /pen vs /sta.
                ScheduleStoreSave("entity_select");
                if (mqtt_push_started_ && !restoring_registry_.load() &&
                    !mqtt_resubscribe_pending_.exchange(true)) {
                    ScheduleMqttResubscribe(*this, mqtt_resubscribe_pending_,
                                            mqtt_push_next_retry_ms_);
                }
            } else {
                // Status churn — deferred behind the 60 s debounce.
                store_dirty_.store(true);
            }
            break;
        case hub_core::RegistryEvent::kDeviceOnlineChanged:
            ESP_LOGI(TAG, "Device online changed: %s", event.dev_id.c_str());
            if (!restoring_registry_.load()) {
                store_dirty_.store(true);  // debounced like status churn
            }
            break;
        case hub_core::RegistryEvent::kRoomsUpdated:
            ESP_LOGI(TAG, "Rooms updated: home %s", event.home_id.c_str());
            if (!restoring_registry_.load()) {
                ScheduleStoreSave("rooms");
            }
            break;
        case hub_core::RegistryEvent::kHomesUpdated:
            ESP_LOGI(TAG, "Homes updated");
            // First homes after an empty link: fetch the scene list now.
            FetchScenesIfEmpty();
            if (!restoring_registry_.load()) {
                ScheduleStoreSave("homes");
            }
            break;
        case hub_core::RegistryEvent::kDevicesCleared:
            ESP_LOGW(TAG, "Device registry cleared");
            break;
    }
}

void Application::Schedule(std::function<void()> callback) {
    uint32_t dropped_total = 0;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (tasks_.size() >= kScheduleQueueCap) {
            // Queue is dominated by transient event/broadcast jobs — the
            // newest carry the freshest state, so shed the oldest.
            tasks_.pop_front();
            ++schedule_dropped_;
            dropped_total = schedule_dropped_;
        }
        tasks_.push_back(std::move(callback));
    }
    if (dropped_total != 0) {
        ESP_LOGW(TAG, "Schedule queue overflow (cap %u) — dropped an oldest job (%u total)",
                 static_cast<unsigned>(kScheduleQueueCap), static_cast<unsigned>(dropped_total));
    }
    xEventGroupSetBits(event_group_, MAIN_EVENT_SCHEDULE);
}

void Application::ScheduleCritical(std::function<void()> callback) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        // Never dropped: over-cap means a producer latch is leaking.
        if (critical_tasks_.size() >= kScheduleCriticalQueueCap) {
            ESP_LOGE(TAG, "Critical queue over cap (%u) — job kept; a latch is leaking",
                     static_cast<unsigned>(kScheduleCriticalQueueCap));
        }
        critical_tasks_.push_back(std::move(callback));
    }
    xEventGroupSetBits(event_group_, MAIN_EVENT_SCHEDULE);
}

void Application::ScheduleStateJob(std::function<void()> callback) {
    // State transitions (SetState / link / self-heal start) must survive a
    // schedule overflow: they ride the never-drop lane, coalesced by their latches.
    ScheduleCritical(std::move(callback));
}

void Application::MainEventLoop() {
    hub_core::SetMainThread(xTaskGetCurrentTaskHandle());
    // Main loop only: worker tasks legitimately block for work; idle-task
    // starvation is covered by the auto-subscribed idle tasks + PANIC.
    const esp_err_t wdt_err = esp_task_wdt_add(nullptr);
    if (wdt_err != ESP_OK) {
        ESP_LOGW(TAG, "Task watchdog subscribe failed: %s", esp_err_to_name(wdt_err));
    }
    while (true) {
        EventBits_t bits =
            xEventGroupWaitBits(event_group_, MAIN_EVENT_SCHEDULE | MAIN_EVENT_CLOCK_TICK,
                                pdTRUE,                // clear bits on return
                                pdFALSE,               // wake on any bit
                                pdMS_TO_TICKS(5000));  // bounded wait: wake to feed the watchdog

        esp_task_wdt_reset();

        if (bits & MAIN_EVENT_SCHEDULE) {
            std::deque<std::function<void()>> jobs;
            std::deque<std::function<void()>> critical_jobs;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                jobs = std::move(tasks_);
                critical_jobs = std::move(critical_tasks_);
            }
            // Transient jobs first (freshest state out); critical jobs
            // last (coalesced, heavy, never dropped).
            // A long drain is healthy under load — feed inside the loops; a
            // hung job never returns to a feed point.
            size_t drained = 0;
            for (auto& job : jobs) {
                job();
                if (++drained % 32 == 0) {
                    esp_task_wdt_reset();
                }
            }
            for (auto& job : critical_jobs) {
                job();
                // Heavy jobs chain up (resub ~5 s + save ~2 s) — feed after each.
                esp_task_wdt_reset();
            }
        }

        if (bits & MAIN_EVENT_CLOCK_TICK) {
            OnClockTick();
        }

        // Re-feed after the drain: a 5 s wait plus a job batch must not trip.
        esp_task_wdt_reset();
    }
}

void Application::OnClockTick() {
    tick_count_++;
    // Health log every 10s; stack high-water marks every 60s.
    if (tick_count_ % 10 == 0) {
        ESP_LOGI(TAG, "tick: state=%s uptime=%us heap_int=%u heap_psram=%u heap_int_min=%u",
                 kStateNames[state_.load()],
                 static_cast<unsigned>(sys::SystemInfo::GetUptimeMs() / 1000),
                 sys::SystemInfo::GetFreeHeapInternal(), sys::SystemInfo::GetFreeHeapPsram(),
                 sys::SystemInfo::GetMinFreeHeapInternal());
        // Fresh full-status snapshot to every connected WS client.
        web_server::WebServer::GetInstance().BroadcastStatus();
    }
    if (tick_count_ % 60 == 0) {
        LogStackHighWaterMarks();
    }

    // Debounced status persistence: at most one flush per 60-tick gate;
    // the suppression test comes first so no churn is lost mid-wipe.
    if (tick_count_ % 60 == 0 && !hub_core::DeviceStore::GetInstance().SavesSuppressed() &&
        store_dirty_.exchange(false)) {
        ScheduleStoreSave("status_debounce");
    }

    // A WiFi reconnect re-arms the fast budget and clears the pending window.
    const bool wifi_now = sys::WifiStation::GetInstance().IsConnected();
    if (wifi_now && !cloud_wifi_was_connected_) {
        cloud_boot_retries_ = 0;
        cloud_next_retry_ms_.store(0);
    }
    cloud_wifi_was_connected_ = wifi_now;

    // Cloud-boot retry: 5 fast retries every 30 s, then one every 10 min;
    // a WiFi reconnect re-arms the fast budget.
    if (!cloud_linked_.load() && state_.load() == kDeviceStateIdle &&
        sys::WifiStation::GetInstance().IsConnected() &&
        static_cast<int64_t>(sys::SystemInfo::GetUptimeMs()) >= cloud_next_retry_ms_.load() &&
        !cloud_boot_retry_running_.load()) {
        const bool fast = cloud_boot_retries_ < kMaxCloudBootRetries;
        cloud_boot_retries_++;
        // Push the window first so no tick queues a duplicate.
        cloud_next_retry_ms_.store(static_cast<int64_t>(sys::SystemInfo::GetUptimeMs()) +
                                   (fast ? kCloudRetryPeriodMs : kCloudSlowRetryMs));
        ESP_LOGW(TAG, "Cloud boot retry %u (%s)", static_cast<unsigned>(cloud_boot_retries_),
                 fast ? "fast" : "slow");
        // Off the loop: StartCloudBoot blocks minutes (time-sync + HTTP retries).
        ScheduleCloudBootRetry();
    }

    // MQTT push self-heal: a resubscribe straddling the mgmt-task shutdown
    // refuses Start(); retry from the tick until it runs.
    if (mqtt_push_next_retry_ms_ != 0 && state_.load() == kDeviceStateLinked &&
        !tuya_mqtt::TuyaMqttClient::GetInstance().IsRunning() &&
        static_cast<int64_t>(sys::SystemInfo::GetUptimeMs()) >= mqtt_push_next_retry_ms_) {
        // Push the window first so no tick queues a duplicate.
        mqtt_push_next_retry_ms_ =
            static_cast<int64_t>(sys::SystemInfo::GetUptimeMs()) + kMqttPushRetryMaxMs;
        ScheduleStateJob([this] {
            if (GetState() != kDeviceStateLinked) {
                mqtt_push_next_retry_ms_ = 0;  // the session moved on — stop
                return;
            }
            if (tuya_mqtt::TuyaMqttClient::GetInstance().IsRunning()) {
                mqtt_push_next_retry_ms_ = 0;
                return;
            }
            // No credentials to build a config from: the next linked session
            // re-arms (a Start here would only fail and re-log warn).
            if (!cloud_linked_.load()) {
                mqtt_push_next_retry_ms_ = 0;
                return;
            }
            if (tuya_mqtt::TuyaMqttClient::GetInstance().Start()) {
                mqtt_push_started_ = true;
                mqtt_push_next_retry_ms_ = 0;
                ESP_LOGI(TAG, "MQTT push started (self-heal retry)");
            } else {
                mqtt_push_retries_++;
                mqtt_push_next_retry_ms_ =
                    static_cast<int64_t>(sys::SystemInfo::GetUptimeMs()) + kMqttPushRetryMaxMs;
                ESP_LOGW(TAG, "MQTT push start refused (retry %u) — next attempt in 30 s",
                         static_cast<unsigned>(mqtt_push_retries_));
            }
        });
    }

    // OpenAPI keepalive: the token expires after 2 h — check every 10 min.
    if (tick_count_ % 600 == 0) {
        tuya_cloud::OpenApiClient& oa_client = tuya_cloud::OpenApiClient::GetInstance();
        // Lock-free expiry re-check of the snapshot's has_token.
        oa_client.RefreshTokenSnapshotForExpiry();
        if (oa_client.snapshot_configured() && !oa_client.snapshot_has_token() &&
            !oa_client.snapshot_login_in_flight()) {
            oa_client.LoginInBackground();
        }
    }
}

void Application::SetState(DeviceState state) {
    DeviceState current = state_.load();
    if (current == state) {
        return;
    }
    ESP_LOGI(TAG, "State: %s -> %s", kStateNames[current], kStateNames[state]);
    state_.store(state);
    sys::LedStatus::GetInstance().OnStateChanged(state);
    web_server::WebServer::GetInstance().SetDeviceState(state);
    web_server::WebServer::GetInstance().BroadcastStatus();
}

void Application::LogStackHighWaterMarks() {
    // uxTaskGetStackHighWaterMark returns BYTES: IDF v6's portSTACK_TYPE
    // is uint8_t, so the sizeof(StackType_t) math below is a 1x no-op.
    struct TaskProbe {
        const char* name;
    };
    static const TaskProbe probes[] = {
        {"main_event_loop"}, {"tuya_disco"},  {"tuya_login"},     {"tuya_mqtt_mgmt"},
        {"mqtt_task"},       {"httpd"},       {"esp_timer"},      {"dns_server"},
        {"tuya_presence"},   {"tuya_scenes"}, {"tuya_cloudboot"},
    };
    UBaseType_t smallest = UINT32_MAX;
    const char* smallest_name = "";
    for (const TaskProbe& probe : probes) {
        TaskHandle_t handle = xTaskGetHandle(probe.name);
        if (handle == nullptr) {
            continue;  // task not running (yet)
        }
        UBaseType_t mark = uxTaskGetStackHighWaterMark(handle);
        if (mark == 0) {
            continue;  // no stack tracking
        }
        if (mark * sizeof(StackType_t) < 2048) {
            // Under 2 KB headroom at the deepest point.
            ESP_LOGW(TAG, "stack low-water: %s has only %u bytes left", probe.name,
                     static_cast<unsigned>(mark * sizeof(StackType_t)));
        }
        if (mark < smallest) {
            smallest = mark;
            smallest_name = probe.name;
        }
    }
    if (smallest != UINT32_MAX) {
        ESP_LOGI(TAG, "stack high-water: min=%s %u bytes", smallest_name,
                 static_cast<unsigned>(smallest * sizeof(StackType_t)));
    }
}
