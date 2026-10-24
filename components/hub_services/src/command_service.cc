#include "hub_services/command_service.h"

#include <cJSON.h>
#include <esp_log.h>

#include <set>
#include <string>
#include <utility>
#include <vector>

#include "hub_core/device.h"
#include "hub_core/device_registry.h"
#include "sys/settings.h"
#include "tuya_cloud/constants.h"
#include "tuya_cloud/customer_api.h"
#include "tuya_cloud/http_method.h"
#include "tuya_cloud/openapi_client.h"
#include "tuya_cloud/openapi_constants.h"
#include "tuya_cloud/paths.h"
#include "tuya_cloud/time_sync.h"

#include "dedup_filter.h"
#include "openapi_command_internal.h"

static const char* TAG = "CommandService";

namespace hub_core {

using tuya_cloud::ApiResponse;
using tuya_cloud::CustomerApi;
using tuya_cloud::DevicePath;
using tuya_cloud::HomeIdParams;
using tuya_cloud::HttpMethod;

CommandService::CommandService() : dedup_(std::make_unique<DedupFilter>()) {
    // Restore quota from NVS; the month rolls lazily on first track/query.
    std::lock_guard<std::mutex> lock(mutex_);
    LoadOpenApiQuotaLocked();
}

CommandService::~CommandService() = default;

CommandResult CommandService::SendDeviceCommands(const std::string& dev_id,
                                                 const std::vector<DeviceCommand>& commands) {
    // Chain budget starts HERE (entry): covers sharing + Tuya IoT legs.
    const int64_t deadline_ms = tuya_cloud::StartCommandDeadline();
    // (1) Validate the whole list up front — before dedup or network.
    if (dev_id.empty()) {
        ESP_LOGW(TAG, "rejected command send with empty dev_id");
        return CommandResult::kError;
    }
    {
        std::string validation_body;
        if (!BuildCommandsBody(commands, validation_body)) {
            ESP_LOGW(TAG, "rejected invalid command payload for %s (empty list, code or value)",
                     dev_id.c_str());
            return CommandResult::kError;
        }
    }

    // (2) Device lookup; an unknown id goes entirely to sharing.
    Device device;
    const bool known = DeviceRegistry::GetInstance().GetDevice(dev_id, device);

    // (2b) Disabled-entity guard before dedup writes — a dedup entry
    // must not suppress a later re-enabled retry.
    if (known) {
        for (const DeviceCommand& command : commands) {
            if (device.disabled_entities.count(command.code) > 0) {
                ESP_LOGW(TAG, "command to disabled entity %s on %s — rejected",
                         command.code.c_str(), dev_id.c_str());
                return CommandResult::kError;
            }
        }
    }

    // (3) Dedup gates the send; a FAILED send below Forget()s the entry
    // so an immediate retry is allowed (success keeps the 10 s window).
    const int64_t now_ms = tuya_cloud::TimeSync::NowMs();
    bool allow = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        allow = dedup_->Allows(dev_id, commands, now_ms);
    }
    if (!allow) {
        ESP_LOGW(TAG, "duplicate command suppressed (dedup): dev=%s count=%u", dev_id.c_str(),
                 static_cast<unsigned>(commands.size()));
        return CommandResult::kDeduplicated;
    }

    // (4) Route split per command code by local_strategy.
    std::vector<DeviceCommand> openapi_cmds;
    std::vector<DeviceCommand> sharing_cmds;
    if (known) {
        RouteCommands(device, commands, openapi_cmds, sharing_cmds);
    } else {
        sharing_cmds = commands;
    }

    bool ok = true;

    // (5) Sharing leg: response ok flag checked (result payload unused;
    // status changes only via MQTT); sharing goes first on a mixed list.
    if (!sharing_cmds.empty()) {
        std::string sharing_body;
        if (!BuildCommandsBody(sharing_cmds, sharing_body)) {
            // Cannot happen — the full list already validated; kept defensive.
            ESP_LOGW(TAG, "sharing body build failed for %s", dev_id.c_str());
            ForgetDedupEntry(dev_id);
            return CommandResult::kError;
        }
        ApiResponse resp = CustomerApi::GetInstance().Request(
            HttpMethod::kPost, DevicePath(tuya_cloud::kPathCommands, dev_id), "", sharing_body,
            deadline_ms);
        if (!resp.ok) {
            // CustomerApi already handled the auth failure — just report.
            ESP_LOGW(TAG, "command send failed: dev=%s err=%s", dev_id.c_str(), resp.msg.c_str());
            ok = false;
        } else {
            ESP_LOGI(TAG, "commands sent (sharing): dev=%s count=%u", dev_id.c_str(),
                     static_cast<unsigned>(sharing_cmds.size()));
        }
    }

    // (6) OpenAPI leg: issue/commands endpoint per property_update.
    if (!openapi_cmds.empty() && !SendViaOpenApi(dev_id, device, openapi_cmds, deadline_ms)) {
        ok = false;
    }

    if (!ok) {
        ForgetDedupEntry(dev_id);
        return CommandResult::kError;
    }
    ESP_LOGI(TAG, "commands sent: dev=%s count=%u (sharing=%u tuya_iot=%u)", dev_id.c_str(),
             static_cast<unsigned>(commands.size()), static_cast<unsigned>(sharing_cmds.size()),
             static_cast<unsigned>(openapi_cmds.size()));
    return CommandResult::kSent;
}

void CommandService::ForgetDedupEntry(const std::string& dev_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    dedup_->Forget(dev_id);
}

bool CommandService::SendViaOpenApi(const std::string& dev_id, const Device& device,
                                    const std::vector<DeviceCommand>& commands,
                                    int64_t deadline_ms) {
    // No OpenAPI account -> failure (no sharing fallback for OpenAPI DPs).
    tuya_cloud::OpenApiClient& api = tuya_cloud::OpenApiClient::GetInstance();
    if (!api.IsConfigured() || !api.HasToken()) {
        ESP_LOGW(TAG,
                 "tuya_iot route unavailable for %s (client not configured or no token) — "
                 "no sharing fallback for Tuya IoT DPs",
                 dev_id.c_str());
        return false;
    }

    // Per-DP property_update flag; each group is one batched call.
    std::vector<DeviceCommand> issue_cmds;
    std::vector<DeviceCommand> regular_cmds;
    SplitByPropertyUpdate(device, commands, issue_cmds, regular_cmds);

    // Quota tracks any success (the slot is consumed already); the return
    // requires every group to succeed (header contract).
    const bool issue_ok = issue_cmds.empty() ||
                          SendOpenApiGroup(dev_id, issue_cmds, /*prefer_issue=*/true, deadline_ms);
    const bool regular_ok =
        regular_cmds.empty() ||
        SendOpenApiGroup(dev_id, regular_cmds, /*prefer_issue=*/false, deadline_ms);
    if (issue_ok || regular_ok) {
        TrackOpenApiQuota(dev_id);
    }
    return issue_ok && regular_ok;
}

bool CommandService::SendOpenApiGroup(const std::string& dev_id,
                                      const std::vector<DeviceCommand>& commands, bool prefer_issue,
                                      int64_t deadline_ms) {
    tuya_cloud::OpenApiClient& api = tuya_cloud::OpenApiClient::GetInstance();
    const std::string issue_path = OpenApiDevicePath(tuya_cloud::kOpenApiThingIssue, dev_id);
    const std::string commands_path = OpenApiDevicePath(tuya_cloud::kOpenApiDeviceCommands, dev_id);

    // Primary endpoint first; the whole group retries on the other endpoint.
    for (int attempt = 0; attempt < 2; ++attempt) {
        const bool use_issue = (attempt == 0) == prefer_issue;
        const char* endpoint = use_issue ? "shadow/properties/issue" : "devices/commands";
        if (tuya_cloud::DeadlineExceeded(deadline_ms)) {
            // Budget gone: neither this endpoint nor the fallback may start.
            ESP_LOGW(TAG, "chain deadline exceeded — skipping tuya_iot %s for %s", endpoint,
                     dev_id.c_str());
            return false;
        }
        std::string body;
        const bool built = use_issue ? BuildShadowIssueBody(commands, body)
                                     : BuildOpenApiCommandsBody(commands, body);
        if (!built) {
            // Cannot happen (the full list already validated above).
            ESP_LOGW(TAG, "tuya_iot body build failed for %s (%s)", dev_id.c_str(), endpoint);
            return false;
        }
        if (attempt > 0) {
            ESP_LOGW(TAG, "tuya_iot %s failed for %s — falling back to %s",
                     prefer_issue ? "shadow/properties/issue" : "devices/commands", dev_id.c_str(),
                     endpoint);
        }
        // Response ok flag checked, result payload unused — as on the sharing leg.
        tuya_cloud::OpenApiResponse resp =
            api.Post(use_issue ? issue_path : commands_path, body, deadline_ms);
        if (resp.ok) {
            ESP_LOGI(TAG, "commands sent (tuya_iot %s): dev=%s count=%u", endpoint, dev_id.c_str(),
                     static_cast<unsigned>(commands.size()));
            return true;
        }
        ESP_LOGW(TAG, "tuya_iot %s failed: dev=%s err=%s", endpoint, dev_id.c_str(),
                 resp.msg.c_str());
    }
    ESP_LOGW(TAG, "tuya_iot send exhausted both endpoints: dev=%s", dev_id.c_str());
    return false;
}

void CommandService::TrackOpenApiQuota(const std::string& dev_id) {
    const int month = CurrentMonth();
    bool changed = false;
    size_t used = 0;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        changed = QuotaRecord(month, dev_id, openapi_quota_month_, openapi_quota_devices_);
        used = openapi_quota_devices_.size();
        if (changed) {
            SaveOpenApiQuotaLocked();
        }
    }
    if (used >= static_cast<size_t>(kQuotaLimit)) {
        // Advisory only — the server enforces the real cap.
        ESP_LOGE(TAG,
                 "Tuya IoT controllable-device quota at the free-tier limit: %u/%d this "
                 "month — the server may reject further commands",
                 static_cast<unsigned>(used), kQuotaLimit);
    } else if (used >= static_cast<size_t>(kQuotaWarnThreshold)) {
        ESP_LOGW(TAG, "approaching Tuya IoT quota: %u/%d controllable devices this month",
                 static_cast<unsigned>(used), kQuotaLimit);
    }
}

OpenApiQuotaInfo CommandService::GetOpenApiQuota() {
    OpenApiQuotaInfo info;
    info.limit = static_cast<size_t>(kQuotaLimit);
    const int month = CurrentMonth();
    std::lock_guard<std::mutex> lock(mutex_);
    if (month > 0 && month != openapi_quota_month_) {
        // Lazy month roll resets the set; persist the cleared state.
        openapi_quota_month_ = month;
        openapi_quota_devices_.clear();
        SaveOpenApiQuotaLocked();
    }
    info.month = openapi_quota_month_;
    info.devices = openapi_quota_devices_.size();
    info.warning = info.devices >= static_cast<size_t>(kQuotaWarnThreshold);
    return info;
}

void CommandService::LoadOpenApiQuotaLocked() {
    sys::Settings settings(kQuotaNvsNamespace, false);
    int month = 0;
    std::set<std::string> devices;
    if (QuotaDecode(settings.GetString(kQuotaNvsKey), month, devices)) {
        openapi_quota_month_ = month;
        openapi_quota_devices_ = std::move(devices);
    }
}

void CommandService::SaveOpenApiQuotaLocked() {
    sys::Settings settings(kQuotaNvsNamespace, true);
    settings.SetString(kQuotaNvsKey, QuotaEncode(openapi_quota_month_, openapi_quota_devices_));
}

SceneFetchResult CommandService::FetchScenes() {
    // A ClearScenes() after this point invalidates the whole result.
    const uint32_t generation = scenes_generation_.load();
    // The home list comes from the registry; one scene call per home.
    const std::vector<Home> homes = DeviceRegistry::GetInstance().GetHomes();

    std::vector<Scene> fetched;  // fresh list
    size_t failed_homes = 0;
    for (const Home& home : homes) {
        ApiResponse resp = CustomerApi::GetInstance().Request(
            HttpMethod::kGet, tuya_cloud::kPathScenesByHome, HomeIdParams(home.id), "");
        if (!resp.ok) {
            // A failing home is skipped, not propagated.
            ESP_LOGW(TAG, "scene fetch failed for home %s: %s", home.id.c_str(), resp.msg.c_str());
            ++failed_homes;
            continue;
        }
        if (!cJSON_IsArray(resp.result)) {
            // A non-array result is a parse failure for this home.
            ESP_LOGW(TAG, "scene fetch: home %s result is not an array", home.id.c_str());
            ++failed_homes;
            continue;
        }
        const size_t added = ParseScenes(resp.result, home.id, fetched);
        ESP_LOGI(TAG, "home %s: %u scene(s)", home.id.c_str(), static_cast<unsigned>(added));
    }

    const size_t total = fetched.size();
    // False only when every home failed; zero homes is vacuous success.
    const bool all_failed = !homes.empty() && failed_homes == homes.size();
    bool committed = false;
    {
        // REPLACE the cache only when no ClearScenes() raced this fetch.
        std::lock_guard<std::mutex> lock(mutex_);
        if (scenes_generation_.load() == generation) {
            scenes_ = std::move(fetched);
            committed = true;
        }
    }
    SceneFetchResult result;
    result.ok = !all_failed;
    result.failed_homes = failed_homes;
    result.total_homes = homes.size();
    if (!committed) {
        // The wipe owns the cache now; a later FetchScenesIfEmpty re-runs.
        ESP_LOGW(TAG, "scene cache cleared during fetch — discarded %u scene(s)",
                 static_cast<unsigned>(total));
    } else if (all_failed) {
        ESP_LOGE(TAG, "scene fetch failed for every home (%u/%u)",
                 static_cast<unsigned>(failed_homes), static_cast<unsigned>(homes.size()));
    } else if (failed_homes > 0) {
        ESP_LOGW(TAG, "scene fetch partial: %u scene(s) from %u/%u home(s)",
                 static_cast<unsigned>(total), static_cast<unsigned>(homes.size() - failed_homes),
                 static_cast<unsigned>(homes.size()));
    } else {
        ESP_LOGI(TAG, "scene fetch complete: %u scene(s) in %u home(s)",
                 static_cast<unsigned>(total), static_cast<unsigned>(homes.size()));
    }
    return result;
}

void CommandService::ClearScenes() {
    std::lock_guard<std::mutex> lock(mutex_);
    scenes_.clear();
    // Invalidate an in-flight fetch: its stale result must not refill the cache.
    scenes_generation_.fetch_add(1);
    ESP_LOGI(TAG, "scene cache cleared");
}

std::vector<Scene> CommandService::GetScenes() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return scenes_;
}

bool CommandService::TriggerScene(const std::string& home_id, const std::string& scene_id) {
    // Chain budget starts HERE (entry): refresh + trigger POST.
    const int64_t deadline_ms = tuya_cloud::StartCommandDeadline();
    if (home_id.empty() || scene_id.empty()) {
        ESP_LOGW(TAG, "rejected scene trigger with empty home/scene id");
        return false;
    }
    // POST trigger, body {"homeId","sceneId"}.
    ApiResponse resp =
        CustomerApi::GetInstance().Request(HttpMethod::kPost, tuya_cloud::kPathSceneTrigger, "",
                                           BuildSceneTriggerBody(home_id, scene_id), deadline_ms);
    if (!resp.ok) {
        ESP_LOGW(TAG, "scene trigger failed (%s/%s): %s", home_id.c_str(), scene_id.c_str(),
                 resp.msg.c_str());
        return false;
    }
    ESP_LOGI(TAG, "scene triggered: home=%s scene=%s", home_id.c_str(), scene_id.c_str());
    return true;
}

}  // namespace hub_core
