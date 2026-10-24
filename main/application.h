#pragma once

#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/event_groups.h>

#include <atomic>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <string>

#include "sys/device_state.h"

namespace tuya_cloud {
struct AuthStatus;  // tuya_cloud/cloud_auth.h
}

namespace hub_core {
struct DiscoveryProgress;     // hub_services/discovery.h
struct RegistryEventPayload;  // hub_core/device_registry.h
}  // namespace hub_core

namespace tuya_mqtt {
struct MqttStatus;  // tuya_mqtt/tuya_mqtt_client.h
}

/// MQTT push self-heal: first retry 5 s after a refused Start().
constexpr int64_t kMqttPushRetryFirstMs = 5000;

/// Hub application core: singleton owning the main event loop task.
/// All cross-task work is marshalled onto the loop via Schedule().
class Application {
public:
    static Application& GetInstance() {
        static Application instance;
        return instance;
    }
    Application(const Application&) = delete;
    Application& operator=(const Application&) = delete;

    /// Called once from app_main(). Returns silently if already started.
    void Start();

    /// Thread-safe: queue a job to run on the main event loop.
    void Schedule(std::function<void()> callback);

    /// Thread-safe: queue a critical job — never dropped; every producer
    /// must coalesce via a latch (see kScheduleCriticalQueueCap).
    void ScheduleCritical(std::function<void()> callback);

    /// Thread-safe: queue a state transition (SetState / link) — an
    /// overflow must never swallow it, so it rides the never-drop lane.
    void ScheduleStateJob(std::function<void()> callback);

    void SetState(DeviceState state);
    DeviceState GetState() const { return state_.load(std::memory_order_relaxed); }

private:
    Application();

    void MainEventLoop();
    void OnClockTick();

    /// Logs per-task stack headroom every 60s; returns the smallest mark.
    void LogStackHighWaterMarks();

    void StartNetwork();

    /// Blocking cloud bring-up (retries built in); never from httpd tasks.
    bool StartCloud();

    /// Spawns the one-shot "tuya_cloudboot" retry task (StartCloudBoot
    /// blocks minutes — never on the main loop).
    void ScheduleCloudBootRetry();

    /// Retry task body: the blocking StartCloud bring-up, off the loop.
    void RunCloudBootRetry();

    static void CloudBootRetryTask(void* arg);

    void OnAuthStatus(const tuya_cloud::AuthStatus& status);

    /// Publishes Linked and brings up the push session: relink wipe, registry
    /// restore (or scan) and the MQTT start. kLinked path and the lost-link
    /// re-verify both land here.
    void StartPushSession();

    /// Starts discovery on its own task; idempotent.
    void StartDeviceDiscovery();

    void OnDiscoveryProgress(const hub_core::DiscoveryProgress& progress);

    void OnMqttStatus(const tuya_mqtt::MqttStatus& status);

    /// Queues the one-shot presence refresh once per MQTT connect. Main loop only.
    void SchedulePresenceRefresh();

    /// Heals registry online flags from the cloud; runs on the "tuya_presence" task.
    void RunPresenceRefresh();

    static void PresenceRefreshTask(void* arg);

    /// Spawns the one-shot "tuya_scenes" task (blocking per-home scene GETs).
    void ScheduleSceneFetch();

    /// Fetches scenes only while homes exist and the cache is empty; the
    /// kLinked-time fetch is skipped with no homes and retried from here.
    void FetchScenesIfEmpty();

    /// Scene fetch task body; blocks on the cloud.
    void RunSceneFetch();

    static void SceneFetchTask(void* arg);

    /// Spawns the one-shot "tuya_rooms" task (rooms + room devices via Tuya
    /// IoT); false when one is already in flight or the spawn failed.
    bool ScheduleRoomRefresh();

    /// Room refresh task body; blocks on the cloud. A complete per-home sweep
    /// is the only thing allowed to clear an assignment.
    void RunRoomRefresh();

    static void RoomRefreshTask(void* arg);

    /// Re-arms a task whose spawn failed (the caller still holds its latch).
    /// Main loop only.
    void ScheduleSpawnRetry(bool presence);

    /// Push start: MQTT + scene fetch. False when StartMqttPush could not
    /// begin (never attempted / the tick must retry). Main loop only.
    bool StartMqttPush();

    /// Starts MQTT, arms the tick retry on a refusal, and fetches scenes.
    /// Main loop only.
    bool StartMqttPushTransport();

    /// Registry events: WS broadcast + persistence triggers; kDeviceAdded starts the push.
    void OnRegistryEvent(const hub_core::RegistryEventPayload& event);

    /// Restores from the store snapshot (uid mismatch wipes it); true when populated;
    /// single-flight. `was_linked` = session state captured BEFORE it was published.
    bool RestoreRegistryFromStore(bool was_linked);

    /// Re-arms the restore latch + the push start after a failed restore on a
    /// linked session. Main loop only.
    void RestoreFailedKeepRetry(bool was_cloud_linked);

    /// Peeks the store snapshot's owner uid (no restore); false only when
    /// both uids are non-empty and differ (the kLinked relink wipe condition).
    bool StoreOwnerMatchesCurrentAccount();

    /// Serializes the registry to registry.json (blocking ~1-2 s). Main loop only.
    void SaveRegistryToStore(const char* why);

    /// Queues one coalesced save job; `why` labels the log. Main loop only.
    void ScheduleStoreSave(const char* why);

    /// The account uid from persisted credentials (the only public source).
    static std::string CurrentAccountUid();

    void SetStoreOwnerUid(const std::string& uid);

    std::string GetStoreOwnerUid();

    void ClearStoreOwnerUid();

    EventGroupHandle_t event_group_ = nullptr;
    esp_timer_handle_t clock_timer_ = nullptr;
    std::atomic<bool> started_{false};
    std::mutex mutex_;
    std::deque<std::function<void()>> tasks_;
    // Latched-producer jobs (ScheduleCritical): never dropped.
    std::deque<std::function<void()>> critical_tasks_;
    // Schedule-queue overflow drops (oldest-first); guarded by mutex_.
    uint32_t schedule_dropped_ = 0;
    std::atomic<DeviceState> state_{kDeviceStateStarting};
    uint32_t tick_count_ = 0;

    // Cloud-boot retry bookkeeping; cloud_next_retry_ms_ is atomic (shared with the boot path).
    std::atomic<bool> cloud_linked_{false};
    uint32_t cloud_boot_retries_ = 0;
    bool cloud_wifi_was_connected_ = false;
    std::atomic<int64_t> cloud_next_retry_ms_{0};
    // Single-flight latch of the cloud-boot retry task.
    std::atomic<bool> cloud_boot_retry_running_{false};
    // Set when a failed re-verify dropped the link: the next boot retry re-links
    // explicitly (the auth state stays kLinked, so no event would arrive).
    std::atomic<bool> lost_link_relink_{false};

    // Suppresses discovery progress logs to one line per 10%.
    int discovery_last_percent_ = -1;

    // registry_restore_done_: single-flight restore latch per link session;
    // restoring_registry_: suppresses saves while a snapshot is applied.
    std::atomic<bool> registry_restore_done_{false};
    std::atomic<bool> restoring_registry_{false};
    // store_save_pending_: save coalescing flag; store_dirty_: 60s debounce flag,
    // re-armed by a failed save so the next tick retries.
    std::atomic<bool> store_save_pending_{false};
    std::atomic<bool> store_dirty_{false};
    std::atomic<bool> store_save_retry_logged_{false};  // one WARN per failing streak
    // Set once the push transport starts; reset by the unlink teardown.
    bool mqtt_push_started_ = false;
    // MQTT self-heal: next Start() retry time (0 = not armed) + attempt
    // count; backoff 5 s -> 30 s. Main loop only (guarded by the loop).
    int64_t mqtt_push_next_retry_ms_ = 0;
    uint32_t mqtt_push_retries_ = 0;
    // Commit-batch MQTT resubscribe coalescing flag.
    std::atomic<bool> mqtt_resubscribe_pending_{false};
    // Single-flight latch of the presence-refresh task.
    std::atomic<bool> presence_refresh_running_{false};
    // Single-flight latch of the scene-fetch task.
    std::atomic<bool> scene_fetch_running_{false};
    // Single-flight latch of the room-refresh task.
    std::atomic<bool> room_refresh_running_{false};
    // One-shot 30 s retry timer for a partial fetch; lazily created once.
    esp_timer_handle_t scene_retry_timer_ = nullptr;
    // One-shot 60 s retry for a failed presence/scene task spawn; lazily created.
    esp_timer_handle_t spawn_retry_timer_ = nullptr;
    // Task family the armed spawn retry re-spawns (true = presence).
    std::atomic<bool> spawn_retry_presence_{false};
    // Retries armed in the current cycle (scene task only, latch-guarded).
    uint32_t scene_fetch_retries_ = 0;
    // Set by a retry dispatch; the fetch it spawns keeps the retry budget.
    std::atomic<bool> scene_fetch_is_retry_{false};
    // Account uid the registry content was established under; stamped into every save.
    // Mutex-guarded.
    std::string store_owner_uid_;
    mutable std::mutex store_owner_mutex_;
};
