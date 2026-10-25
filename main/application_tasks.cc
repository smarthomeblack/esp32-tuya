#include "application.h"

#include <cJSON.h>
#include <esp_log.h>
#include <esp_timer.h>

#include <freertos/task.h>

#include <cstdint>
#include <string>
#include <vector>

#include "hub_core/device_registry.h"
#include "hub_core/parse.h"
#include "hub_core/presence_status.h"
#include "hub_services/command_service.h"
#include "sys/system_info.h"
#include "tuya_cloud/api_response.h"
#include "tuya_cloud/constants.h"
#include "tuya_cloud/customer_api.h"
#include "tuya_cloud/openapi_client.h"
#include "tuya_cloud/paths.h"
#include "tuya_crypto/json_fields.h"

// Log tag mirrors Application so the boot log stays byte-identical.
static const char* TAG = "Application";

namespace {

// Same task budget as discovery (esp_http_client + mbedTLS + cJSON).
constexpr uint32_t kPresenceTaskStackBytes = 12288;
constexpr UBaseType_t kPresenceTaskPriority = 3;
constexpr uint32_t kSceneFetchTaskStackBytes = 12288;
constexpr UBaseType_t kSceneFetchTaskPriority = 3;
constexpr uint32_t kCloudBootTaskStackBytes = 12288;
constexpr UBaseType_t kCloudBootTaskPriority = 3;
// 1106 = permission deny: the room service is not enabled for the IoT project.
constexpr int64_t kOpenApiCodePermissionDeny = 1106;
constexpr uint32_t kRoomTaskStackBytes = 12288;
constexpr UBaseType_t kRoomTaskPriority = 3;
// Partial scene-fetch retry: 30 s one-shot delay, max 3 per session.
constexpr int64_t kSceneFetchRetryDelayUs = 30 * 1000000;
constexpr uint32_t kSceneFetchRetryCap = 3;
// Spawn-failure re-arm (presence/scene): one retry a minute; the shared latch
// refuses every trigger until the re-armed retry frees it.
constexpr int64_t kSpawnRetryDelayUs = 60 * 1000000;

/// Registry -> JSON of a typed API response. The client Duplicates it, so the
/// parse must run before the next request frees the tree.
bool RoomResultText(const tuya_cloud::OpenApiResponse& resp, std::string& json) {
    const tuya_crypto::JsonText text(resp.result);
    json = text.str();
    return !json.empty();
}

}  // namespace

void Application::ScheduleCloudBootRetry() {
    // Spawn the one-shot "tuya_cloudboot" task; the latch keeps retries serial.
    bool expected = false;
    if (!cloud_boot_retry_running_.compare_exchange_strong(expected, true)) {
        return;  // a retry is already in flight
    }
    const BaseType_t ok =
        xTaskCreate(CloudBootRetryTask, "tuya_cloudboot", kCloudBootTaskStackBytes, this,
                    kCloudBootTaskPriority, nullptr);
    if (ok != pdPASS) {
        cloud_boot_retry_running_.store(false);
        ESP_LOGW(TAG, "Failed to create the cloud-boot retry task");
    }
}

void Application::CloudBootRetryTask(void* arg) {
    auto* app = static_cast<Application*>(arg);
    app->RunCloudBootRetry();
    app->cloud_boot_retry_running_.store(false);
    vTaskDelete(nullptr);
}

void Application::RunCloudBootRetry() {
    // Dedicated task: StartCloudBoot blocks minutes (time-sync + HTTP
    // retries); registry events marshal back via the listener Schedule.
    StartCloud();
}

void Application::SchedulePresenceRefresh() {
    // The dispatcher owns the cadence: a leftover burst slot polls in 1 s,
    // otherwise the background (backoff-aware) interval applies.
    // The 1 Hz tick and the command hook both schedule here: a load-then-decrement
    // could underflow the counter and poll every second forever.
    uint32_t left = presence_burst_left_.load();
    while (left > 0 && !presence_burst_left_.compare_exchange_weak(left, left - 1)) {
    }
    const bool burst = left > 0;
    const int64_t interval_ms = burst ? kPresenceBurstSpacingMs : presence_interval_ms_.load();
    presence_next_poll_ms_.store(static_cast<int64_t>(sys::SystemInfo::GetUptimeMs()) +
                                 interval_ms);
    // Spawn the one-shot "tuya_presence" task; guards keep it single-flight.
    if (hub_core::DeviceRegistry::GetInstance().Count() == 0) {
        ESP_LOGD(TAG, "Presence refresh skipped — registry is empty");
        return;
    }
    bool expected = false;
    if (!presence_refresh_running_.compare_exchange_strong(expected, true)) {
        return;  // a refresh is already in flight — its slot is spent, not retried
    }
    const BaseType_t ok = xTaskCreate(PresenceRefreshTask, "tuya_presence", kPresenceTaskStackBytes,
                                      this, kPresenceTaskPriority, nullptr);
    if (ok != pdPASS) {
        // The latch is HELD by the re-armed retry: every trigger skips until
        // it fires, so the refresh is not lost.
        ESP_LOGW(TAG, "Failed to create the presence refresh task — retry in 60 s");
        ScheduleSpawnRetry(true);
    }
}

void Application::SchedulePresenceBurst() {
    // A sent command moved the cloud state: confirm with 5 polls one second apart.
    presence_burst_left_.store(kPresenceBurstCount);
    SchedulePresenceRefresh();
}

void Application::PresenceRefreshTask(void* arg) {
    auto* app = static_cast<Application*>(arg);
    app->RunPresenceRefresh();
    app->presence_refresh_running_.store(false);
    vTaskDelete(nullptr);
}

void Application::PresencePollFailed(const char* why) {
    const uint32_t failures = presence_failures_.load() + 1;
    presence_failures_.store(failures);
    const int64_t interval_ms = PresenceBackoffMs(failures);
    presence_interval_ms_.store(interval_ms);
    // Hammering a failing endpoint helps nobody: the backoff owns recovery.
    presence_burst_left_.store(0);
    presence_next_poll_ms_.store(static_cast<int64_t>(sys::SystemInfo::GetUptimeMs()) +
                                 interval_ms);
    ESP_LOGW(TAG, "Presence poll failed (%s) — retry in %u s (failure %u)", why,
             static_cast<unsigned>(interval_ms / 1000), static_cast<unsigned>(failures));
}

void Application::PresencePollRecovered() {
    presence_interval_ms_.store(kPresencePollMs);
    const uint32_t failures = presence_failures_.exchange(0);
    if (failures != 0) {
        // Shrink the backed-off deadline, but never overwrite a concurrently
        // scheduled 1 s burst deadline.
        const int64_t wanted =
            static_cast<int64_t>(sys::SystemInfo::GetUptimeMs()) + kPresencePollMs;
        int64_t next = presence_next_poll_ms_.load();
        while (next > wanted && !presence_next_poll_ms_.compare_exchange_weak(next, wanted)) {
        }
        ESP_LOGI(TAG, "Presence poll recovered after %u failure(s) — interval back to %u s",
                 static_cast<unsigned>(failures), static_cast<unsigned>(kPresencePollMs / 1000));
    }
}

void Application::RunPresenceRefresh() {
    tuya_cloud::CustomerApi& api = tuya_cloud::CustomerApi::GetInstance();
    if (!api.IsConfigured()) {
        ESP_LOGW(TAG, "Presence refresh skipped — CustomerApi not configured");
        return;
    }
    hub_core::DeviceRegistry& registry = hub_core::DeviceRegistry::GetInstance();
    const std::vector<hub_core::Home> homes = registry.GetHomes();
    if (homes.empty()) {
        return;
    }

    size_t matched = 0;    // registry devices seen in the cloud lists
    size_t online = 0;     // of those, currently online per the API
    size_t applied = 0;    // status codes whose value actually moved
    size_t unchanged = 0;  // codes the cloud repeated (or that are disabled)
    for (const hub_core::Home& home : homes) {
        // One attempt; the backoff retry is armed by PresencePollFailed.
        tuya_cloud::ApiResponse resp =
            api.Request(tuya_cloud::HttpMethod::kGet, tuya_cloud::kPathDevicesByHome,
                        tuya_cloud::HomeIdParams(home.id), "");
        if (!resp.ok) {
            ESP_LOGW(TAG, "Presence poll: home %s device list failed (%s)", home.id.c_str(),
                     resp.msg.c_str());
            PresencePollFailed("device list unreachable");
            return;
        }
        if (!cJSON_IsArray(resp.result)) {
            ESP_LOGW(TAG, "Presence poll: home %s result is not a JSON array", home.id.c_str());
            PresencePollFailed("unexpected result shape");
            return;
        }
        for (const cJSON* item = resp.result->child; item != nullptr; item = item->next) {
            // Reuse discovery's parse; an unusable row is skipped.
            const std::string json = tuya_crypto::JsonText(item).str();
            hub_core::Device device;
            if (json.empty() || !hub_core::ParseDeviceJson(json, device)) {
                continue;
            }
            // SetOnline fires only on flips — no steady-state event spam.
            if (registry.SetOnline(device.id, device.online)) {
                ++matched;
                if (device.online) {
                    ++online;
                }
            }
            const hub_core::PresenceApplyResult merged =
                hub_core::ApplyPresenceStatus(registry, device);
            applied += merged.applied;
            unchanged += merged.unchanged;
        }
    }
    PresencePollRecovered();
    ESP_LOGI(TAG, "presence poll: %u code(s) applied, %u unchanged, interval=%u s",
             static_cast<unsigned>(applied), static_cast<unsigned>(unchanged),
             static_cast<unsigned>(presence_interval_ms_.load() / 1000));
    ESP_LOGI(TAG, "presence refresh: %u/%u device(s) online", static_cast<unsigned>(online),
             static_cast<unsigned>(matched));
    // Rooms ride along at most every 5 min: the presence poll is 10 s and a
    // room sweep costs 1 + N OpenAPI calls. On-demand refresh bypasses this.
    const int64_t now_ms = static_cast<int64_t>(sys::SystemInfo::GetUptimeMs());
    if (now_ms >= room_next_poll_ms_.load()) {
        room_next_poll_ms_.store(now_ms + kRoomPollMs);
        ScheduleRoomRefresh();  // fire-and-forget, this task never waits for it
    }
}

void Application::ScheduleSceneFetch() {
    // Spawn the one-shot "tuya_scenes" task; the latch keeps it single-flight.
    bool expected = false;
    if (!scene_fetch_running_.compare_exchange_strong(expected, true)) {
        return;  // a fetch is already in flight
    }
    const BaseType_t ok = xTaskCreate(SceneFetchTask, "tuya_scenes", kSceneFetchTaskStackBytes,
                                      this, kSceneFetchTaskPriority, nullptr);
    if (ok != pdPASS) {
        // Latch held by the retry (see SchedulePresenceRefresh); the 60 s
        // dispatch runs as a RETRY so a partial fetch keeps polling.
        ESP_LOGW(TAG, "Failed to create the scene fetch task — retry in 60 s");
        ScheduleSpawnRetry(false);
    }
}

void Application::FetchScenesIfEmpty() {
    // Trigger rule: fetch only when homes exist and the cache is still empty —
    // the latch above keeps concurrent triggers from stacking.
    if (hub_core::DeviceRegistry::GetInstance().GetHomes().empty() ||
        !hub_core::CommandService::GetInstance().GetScenes().empty()) {
        return;
    }
    ScheduleSceneFetch();
}

void Application::SceneFetchTask(void* arg) {
    auto* app = static_cast<Application*>(arg);
    app->RunSceneFetch();
    app->scene_fetch_running_.store(false);
    vTaskDelete(nullptr);
}

// Rooms and their devices come from the Tuya IoT Platform (the sharing API has
// none).

bool Application::ScheduleRoomRefresh() {
    // Spawn the one-shot "tuya_rooms" task; the latch keeps it single-flight.
    bool expected = false;
    if (!room_refresh_running_.compare_exchange_strong(expected, true)) {
        ESP_LOGI(TAG, "Room refresh skipped — one is already in flight");
        return false;
    }
    const BaseType_t ok = xTaskCreate(RoomRefreshTask, "tuya_rooms", kRoomTaskStackBytes, this,
                                      kRoomTaskPriority, nullptr);
    if (ok != pdPASS) {
        room_refresh_running_.store(false);
        ESP_LOGW(TAG, "Failed to create the room refresh task");
        return false;
    }
    return true;
}

void Application::RoomRefreshTask(void* arg) {
    auto* app = static_cast<Application*>(arg);
    app->RunRoomRefresh();
    app->room_refresh_running_.store(false);
    vTaskDelete(nullptr);
}

void Application::RunRoomRefresh() {
    tuya_cloud::OpenApiClient& api = tuya_cloud::OpenApiClient::GetInstance();
    if (!api.IsConfigured() || !api.HasToken()) {
        ESP_LOGI(TAG, "Room refresh skipped — Tuya IoT Platform not connected");
        return;
    }
    hub_core::DeviceRegistry& registry = hub_core::DeviceRegistry::GetInstance();
    const std::vector<hub_core::Home> homes = registry.GetHomes();
    if (homes.empty()) {
        return;
    }

    for (const hub_core::Home& home : homes) {
        // One attempt per trigger; a later link or the manual refresh retries.
        std::string body;
        tuya_cloud::OpenApiResponse resp =
            api.Get(std::string(tuya_cloud::kOpenApiHomeRooms), tuya_cloud::OpenApiParams{},
                    std::string(home.id));
        if (!resp.ok && resp.code == kOpenApiCodePermissionDeny) {
            // 1106: the room service is off for the account's IoT project.
            ESP_LOGW(TAG, "Room refresh: home %s denied (code %lld) — room service not enabled",
                     home.id.c_str(), static_cast<long long>(resp.code));
            return;
        }
        if (!resp.ok) {
            ESP_LOGW(TAG, "Room refresh: home %s rooms failed (code=%lld %s) — home skipped",
                     home.id.c_str(), static_cast<long long>(resp.code), resp.msg.c_str());
            continue;
        }
        if (!RoomResultText(resp, body)) {
            ESP_LOGW(TAG, "Room refresh: home %s rooms response unusable — home skipped",
                     home.id.c_str());
            continue;
        }
        std::vector<hub_core::Room> rooms;
        if (!hub_core::ParseRoomsJson(body, rooms)) {
            ESP_LOGW(TAG, "Room refresh: home %s rooms shape unsupported — home skipped",
                     home.id.c_str());
            continue;
        }

        // Room devices: one call per room; a failed room aborts the home (a
        // partial sweep would wrongly unassign the room's devices).
        std::vector<std::pair<std::string, std::string>> pairs;
        bool complete = true;
        for (const hub_core::Room& room : rooms) {
            resp = api.Get(std::string(tuya_cloud::kOpenApiRoomDevices),
                           tuya_cloud::OpenApiParams{}, std::string(home.id), std::string(room.id));
            if (!resp.ok) {
                ESP_LOGW(TAG, "Room refresh: home %s room %s devices failed (%s) — home skipped",
                         home.id.c_str(), room.id.c_str(), resp.msg.c_str());
                complete = false;
                break;
            }
            if (!RoomResultText(resp, body)) {
                complete = false;
                break;
            }
            std::vector<std::string> ids;
            if (!hub_core::ParseRoomDevicesJson(body, ids)) {
                complete = false;
                break;
            }
            for (std::string& id : ids) {
                pairs.emplace_back(std::string(room.id), std::move(id));
            }
        }
        if (!complete) {
            continue;  // keep the previous assignments
        }
        registry.SetRooms(home.id, rooms);
        registry.AssignDeviceRooms(home.id, pairs);
        ESP_LOGI(TAG, "Room refresh: home %s has %u room(s), %u device assignment(s)",
                 home.id.c_str(), static_cast<unsigned>(rooms.size()),
                 static_cast<unsigned>(pairs.size()));
    }
}

void Application::ScheduleSpawnRetry(bool presence) {
    if (spawn_retry_timer_ == nullptr) {
        const esp_timer_create_args_t timer_args = {
            .callback =
                [](void* arg) {
                    auto* app = static_cast<Application*>(arg);
                    // Re-spawn the task family whose spawn failed; on the scene
                    // side the dispatch is a RETRY (keeps the poll budget).
                    if (app->spawn_retry_presence_.load()) {
                        app->SchedulePresenceRefresh();
                    } else {
                        app->scene_fetch_is_retry_.store(true);
                        app->ScheduleSceneFetch();
                    }
                },
            .arg = this,
            .dispatch_method = ESP_TIMER_TASK,
            .name = "spawn_retry",
            .skip_unhandled_events = false,
        };
        if (esp_timer_create(&timer_args, &spawn_retry_timer_) != ESP_OK) {
            // No retry possible: release the latch the caller still holds.
            spawn_retry_timer_ = nullptr;
            ESP_LOGW(TAG, "Failed to create the spawn retry timer — latch released");
            if (presence) {
                presence_refresh_running_.store(false);
            } else {
                scene_fetch_running_.store(false);
            }
            return;
        }
    }
    // The armed retry re-spawns whatever family failed last.
    spawn_retry_presence_.store(presence);
    // start_once fails with ESP_ERR_INVALID_STATE while the timer is armed.
    esp_timer_stop(spawn_retry_timer_);
    if (esp_timer_start_once(spawn_retry_timer_, kSpawnRetryDelayUs) != ESP_OK) {
        ESP_LOGW(TAG, "spawn retry timer re-arm failed");
    }
}

void Application::RunSceneFetch() {
    // Consume the retry marker; the timer dispatch keeps the fetch budget.
    const bool is_retry = scene_fetch_is_retry_.exchange(false);
    const hub_core::SceneFetchResult result = hub_core::CommandService::GetInstance().FetchScenes();
    // Timer dispatch = retry (keeps the budget); any other trigger resets it.
    if (!is_retry || result.failed_homes == 0) {
        scene_fetch_retries_ = 0;
    }
    const unsigned cached =
        static_cast<unsigned>(hub_core::CommandService::GetInstance().GetScenes().size());
    if (result.failed_homes == 0) {
        if (scene_retry_timer_ != nullptr) {
            esp_timer_stop(scene_retry_timer_);
        }
        ESP_LOGI(TAG, "Scene fetch complete: %u scene(s)", cached);
        return;
    }
    if (!result.ok) {
        ESP_LOGI(TAG, "Scene fetch failed (every home): %u scene(s)", cached);
        return;
    }
    ESP_LOGW(TAG, "Scene fetch partial: %u scene(s), %u/%u home(s) failed", cached,
             static_cast<unsigned>(result.failed_homes), static_cast<unsigned>(result.total_homes));
    if (scene_fetch_retries_ >= kSceneFetchRetryCap) {
        ESP_LOGW(TAG, "scene fetch gave up after %u retries — reboot/re-link recovers",
                 static_cast<unsigned>(scene_fetch_retries_));
        return;
    }
    ++scene_fetch_retries_;
    if (scene_retry_timer_ == nullptr) {
        const esp_timer_create_args_t timer_args = {
            .callback =
                [](void* arg) {
                    auto* app = static_cast<Application*>(arg);
                    app->scene_fetch_is_retry_.store(true);
                    app->ScheduleSceneFetch();
                },
            .arg = this,
            .dispatch_method = ESP_TIMER_TASK,
            .name = "scene_retry",
            .skip_unhandled_events = false,
        };
        if (esp_timer_create(&timer_args, &scene_retry_timer_) != ESP_OK) {
            scene_retry_timer_ = nullptr;
            ESP_LOGW(TAG, "Failed to create the scene retry timer");
            return;
        }
    }
    // start_once fails with ESP_ERR_INVALID_STATE while the timer is armed.
    esp_timer_stop(scene_retry_timer_);
    if (esp_timer_start_once(scene_retry_timer_, kSceneFetchRetryDelayUs) != ESP_OK) {
        ESP_LOGW(TAG, "scene retry timer re-arm failed");
    }
}
