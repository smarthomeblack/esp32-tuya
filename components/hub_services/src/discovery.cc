#include "hub_services/discovery.h"
#include "hub_core/device_registry.h"
#include "hub_services/openapi_enrichment.h"

#include <cJSON.h>
#include <esp_log.h>

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "tuya_cloud/constants.h"
#include "tuya_cloud/customer_api.h"
#include "tuya_cloud/http_method.h"
#include "tuya_cloud/paths.h"

#include "discovery_internal.h"
#include "parse_helpers.h"

static const char* TAG = "Discovery";

namespace hub_core {

using tuya_cloud::ApiResponse;
using tuya_cloud::CustomerApi;
using tuya_cloud::DevicePath;
using tuya_cloud::HomeIdParams;
using tuya_cloud::HttpMethod;

namespace {

constexpr uint32_t kDiscoveryTaskStackBytes = 12288;
constexpr UBaseType_t kDiscoveryTaskPriority = 3;

constexpr int kHttpRetries = 2;
constexpr uint32_t kRetryDelayMs = 2000;

bool IsAuthDead(const ApiResponse& resp) {
    // Dead credentials: every further request would fail identically.
    return !resp.ok && resp.msg.find("sign invalid") != std::string::npos;
}

/// One CustomerApi call with retries; aborts retrying on success or dead auth.
/// Deadline-free by design: background chains keep their own retry policy.
ApiResponse RequestWithRetry(HttpMethod method, const std::string& path,
                             const std::string& params_json, const char* what) {
    ApiResponse resp;
    for (int attempt = 0; attempt <= kHttpRetries; ++attempt) {
        if (attempt > 0) {
            ESP_LOGW(TAG, "%s failed (%s) — retry %d/%d", what, resp.msg.c_str(), attempt,
                     kHttpRetries);
            vTaskDelay(pdMS_TO_TICKS(kRetryDelayMs));
        }
        resp = CustomerApi::GetInstance().Request(method, path, params_json, "");
        if (resp.ok || IsAuthDead(resp)) {
            return resp;
        }
    }
    return resp;
}

/// Union of the entity-code map keys (status, status_range, function).
DeviceCodeSet EntityCodeUnion(const Device& device) {
    DeviceCodeSet codes;
    for (const auto& item : device.status) {
        codes.insert(item.first);
    }
    for (const auto& item : device.status_range) {
        codes.insert(item.first);
    }
    for (const auto& item : device.function) {
        codes.insert(item.first);
    }
    return codes;
}

}  // namespace

DetailFetchStatus PublishDetailResult(DiscoveryStaging& staging, Device& device,
                                      unsigned expected_generation, int failed_calls,
                                      bool auth_dead) {
    if (!staging.done || staging.active || staging.generation != expected_generation) {
        return DetailFetchStatus::kScanActive;
    }
    // Captured before `device` is moved into the staging row.
    const std::string id(device.id);
    for (Device& c : staging.candidates) {
        if (std::string_view(c.id) == id) {
            c = std::move(device);
            break;
        }
    }
    // Degraded results stay staged but uncached — the next fetch retries.
    if (failed_calls == 0 && !auth_dead) {
        staging.detail_loaded.insert(id);
        return DetailFetchStatus::kLoaded;
    }
    return DetailFetchStatus::kDegraded;
}

CommitApplyResult ApplyCommitEntry(DeviceRegistry& registry, const Device& candidate,
                                   const DiscoveryCommitEntry& entry, bool detail_loaded) {
    Device existing;
    if (registry.GetDevice(entry.id, existing)) {
        const DeviceCodeSet existing_codes = EntityCodeUnion(existing);
        const DeviceCodeSet staged_codes = EntityCodeUnion(candidate);
        // Metadata can grow while the code union stays the same (a device
        // may carry every code in `status` but no range/function/strategy).
        const bool richer = staged_codes.size() > existing_codes.size() ||
                            candidate.status_range.size() > existing.status_range.size() ||
                            candidate.function.size() > existing.function.size() ||
                            (candidate.support_local &&
                             candidate.local_strategy.size() > existing.local_strategy.size());
        // A wholesale replace may never shrink a map: a smaller candidate is
        // incomplete and would drop live status values / local routing.
        // Sizes only: a larger one may still drop old codes; self-tests pin this; reports re-add.
        const bool no_shrink = candidate.status.size() >= existing.status.size() &&
                               candidate.status_range.size() >= existing.status_range.size() &&
                               candidate.function.size() >= existing.function.size() &&
                               (!candidate.support_local ||
                                candidate.local_strategy.size() >= existing.local_strategy.size());
        // A degraded (incomplete detail) candidate never replaces a good model.
        if (!detail_loaded || !richer || !no_shrink) {
            // Idempotent: the existing device wins over the staged model.
            ESP_LOGI(TAG, "commit: device %s already in the registry — skipped", entry.id.c_str());
            return CommitApplyResult::kSkippedExisting;
        }
        // Model maps replaced from the staged candidate; strategy entries
        // merge by dpId — new dpIds gain entries, existing ones always win.
        Device refreshed = existing;
        refreshed.status = candidate.status;
        refreshed.status_range = candidate.status_range;
        refreshed.function = candidate.function;
        // A candidate that does not know the local strategy must not erase
        // or overwrite known entries; absent dpIds are unioned in.
        if (candidate.support_local) {
            refreshed.support_local = true;
            refreshed.local_strategy = candidate.local_strategy;
        } else {
            for (const auto& item : candidate.local_strategy) {
                refreshed.local_strategy.emplace(item.first, item.second);
            }
        }
        // Back-fill a missing home_id only; a known one is identity.
        if (refreshed.home_id.empty() && !candidate.home_id.empty()) {
            refreshed.home_id = candidate.home_id;
        }
        refreshed.disabled_entities.clear();
        for (const DeviceString& code : staged_codes) {
            // Existing choices win for codes that still exist; the rest
            // follow the entry's enabled_entities like a fresh commit.
            if (existing.disabled_entities.count(code) > 0 ||
                entry.enabled_entities.count(code) == 0) {
                refreshed.disabled_entities.insert(code);
            }
        }
        registry.UpsertDevice(refreshed);
        ESP_LOGI(TAG,
                 "commit: device %s model refreshed (codes %u→%u ranges %u→%u functions %u→%u "
                 "strategies %u→%u)",
                 entry.id.c_str(), static_cast<unsigned>(existing_codes.size()),
                 static_cast<unsigned>(staged_codes.size()),
                 static_cast<unsigned>(existing.status_range.size()),
                 static_cast<unsigned>(candidate.status_range.size()),
                 static_cast<unsigned>(existing.function.size()),
                 static_cast<unsigned>(candidate.function.size()),
                 static_cast<unsigned>(existing.local_strategy.size()),
                 static_cast<unsigned>(candidate.local_strategy.size()));
        return CommitApplyResult::kRefreshed;
    }

    Device device = candidate;
    // Committed devices must be transport-tracked immediately; the one-shot
    // MarkAllSetUp in StartMqttPush may have fired before this commit.
    device.set_up = true;
    // disabled_entities = (the candidate's entity code keys) - enabled_entities.
    const DeviceCodeSet all_codes = EntityCodeUnion(device);
    for (const DeviceString& code : all_codes) {
        if (entry.enabled_entities.count(code) == 0) {
            device.disabled_entities.insert(code);
        }
    }
    registry.UpsertDevice(device);
    ESP_LOGI(TAG, "commit: device %s added (%u entity code(s), %u disabled)", device.id.c_str(),
             static_cast<unsigned>(all_codes.size()),
             static_cast<unsigned>(device.disabled_entities.size()));
    return CommitApplyResult::kAdded;
}

bool DiscoveryService::Start() {
    bool expected = false;
    if (!running_.compare_exchange_strong(expected, true)) {
        ESP_LOGW(TAG, "Discovery already running — Start() ignored");
        return false;
    }
    // Clear staging HERE — task-spawn latency would serve stale candidates.
    {
        std::lock_guard<std::mutex> lock(staging_mutex_);
        staging_ = DiscoveryStaging{};
        staging_.generation = ++scan_generation_;
    }
    const BaseType_t ok = xTaskCreate(DiscoveryTask, "tuya_disco", kDiscoveryTaskStackBytes, this,
                                      kDiscoveryTaskPriority, nullptr);
    if (ok != pdPASS) {
        running_.store(false);
        ESP_LOGE(TAG, "Failed to create the discovery task");
        return false;
    }
    ESP_LOGI(TAG, "Discovery started (task tuya_disco)");
    return true;
}

void DiscoveryService::SetProgressListener(ProgressListener listener) {
    std::lock_guard<std::mutex> lock(mutex_);
    progress_listener_ = std::move(listener);
}

DiscoveryProgress DiscoveryService::GetProgress() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return progress_;
}

DiscoveryStaging DiscoveryService::GetStaging() const {
    std::lock_guard<std::mutex> lock(staging_mutex_);
    return staging_;
}

size_t DiscoveryService::GetStagingCandidateCount() const {
    std::lock_guard<std::mutex> lock(staging_mutex_);
    return staging_.candidates.size();
}

CommitSelectionResult
DiscoveryService::CommitSelection(const std::vector<DiscoveryCommitEntry>& entries) {
    CommitSelectionResult result;
    if (running_.load()) {
        ESP_LOGW(TAG, "CommitSelection rejected — a discovery scan is active");
        result.scan_active = true;
        return result;
    }
    // Work on a copy taken under the lock; no HTTP in this function.
    DiscoveryStaging staging;
    {
        std::lock_guard<std::mutex> lock(staging_mutex_);
        staging = staging_;
    }
    if (entries.empty() || !staging.done) {
        // A stray call must not wipe the homes with an empty snapshot.
        return result;
    }

    DeviceRegistry& registry = DeviceRegistry::GetInstance();
    registry.SetHomes(staging.homes);

    for (const DiscoveryCommitEntry& entry : entries) {
        const Device* candidate = nullptr;
        for (const Device& c : staging.candidates) {
            if (std::string_view(c.id) == entry.id) {
                candidate = &c;
                break;
            }
        }
        if (candidate == nullptr) {
            ESP_LOGW(TAG, "commit: candidate %s not in staging — skipped", entry.id.c_str());
            ++result.skipped;
            continue;
        }
        const bool detail_loaded = staging.detail_loaded.count(entry.id) > 0;
        switch (ApplyCommitEntry(registry, *candidate, entry, detail_loaded)) {
            case CommitApplyResult::kAdded: ++result.added; break;
            case CommitApplyResult::kRefreshed: ++result.refreshed; break;
            case CommitApplyResult::kSkippedExisting: ++result.skipped; break;
        }
    }
    ESP_LOGI(TAG, "commit: %d added, %d refreshed, %d skipped from %u entr(y/ies)", result.added,
             result.refreshed, result.skipped, static_cast<unsigned>(entries.size()));
    return result;
}

DetailFetchStatus DiscoveryService::FetchCandidateDetail(const std::string& id) {
    bool expected = false;
    if (!detail_fetch_running_.compare_exchange_strong(expected, true)) {
        return DetailFetchStatus::kBusy;
    }
    // Clear the single-flight flag on every return path.
    struct FlagRelease {
        std::atomic<bool>& flag;
        ~FlagRelease() { flag.store(false); }
    } release{detail_fetch_running_};

    // Validation mirrors the commit handler: scan states first, then the id.
    DiscoveryStaging snapshot;
    {
        std::lock_guard<std::mutex> lock(staging_mutex_);
        snapshot = staging_;
    }
    if (running_.load() || snapshot.active) {
        return DetailFetchStatus::kScanActive;
    }
    if (!snapshot.done) {
        return DetailFetchStatus::kNoScan;
    }
    Device device;
    bool found = false;
    for (const Device& c : snapshot.candidates) {
        if (std::string_view(c.id) == id) {
            device = c;  // work on a copy; the lock is never held over HTTP
            found = true;
            break;
        }
    }
    if (!found) {
        return DetailFetchStatus::kNotFound;
    }
    if (snapshot.detail_loaded.count(id) > 0) {
        return DetailFetchStatus::kAlreadyLoaded;
    }

    // Same call order as the old scan loop; failures degrade the model.
    int failed_calls = 0;
    bool auth_dead = false;

    // (a) specifications; on failure keep empty maps.
    ApiResponse sub =
        RequestWithRetry(HttpMethod::kGet, DevicePath(tuya_cloud::kPathSpecifications, device.id),
                         "", "specifications");
    if (sub.ok) {
        ParseSpecifications(sub.result, device);
    } else if (IsAuthDead(sub)) {
        // Dead credentials: every further request would fail identically.
        auth_dead = true;
    } else {
        ESP_LOGW(TAG, "Device %s: specifications failed (%s) — continuing with defaults",
                 device.id.c_str(), sub.msg.c_str());
        failed_calls++;
    }

    // (b) status strategy; on failure support_local stays false.
    if (!auth_dead) {
        sub = RequestWithRetry(HttpMethod::kGet,
                               DevicePath(tuya_cloud::kPathStatusStrategy, device.id), "",
                               "status strategy");
        if (sub.ok) {
            ParseStatusStrategy(sub.result, device);
        } else if (IsAuthDead(sub)) {
            auth_dead = true;
        } else {
            ESP_LOGW(TAG, "Device %s: status strategy failed (%s) — support_local stays false",
                     device.id.c_str(), sub.msg.c_str());
            failed_calls++;
        }
    }

    // (c) dp report types; depends on (a) having filled status_range.
    if (!auth_dead) {
        sub = RequestWithRetry(HttpMethod::kGet,
                               DevicePath(tuya_cloud::kPathDpReportTypes, device.id), "",
                               "dp report types");
        if (sub.ok) {
            ParseDpReportTypes(sub.result, device);
        } else if (IsAuthDead(sub)) {
            auth_dead = true;
        } else {
            ESP_LOGW(TAG, "Device %s: dp report types failed (%s) — report types stay empty",
                     device.id.c_str(), sub.msg.c_str());
            failed_calls++;
        }
    }

    // (d) OpenAPI enrichment for spec-poor devices; fills only
    // MISSING entries (sharing wins).
    if (!auth_dead && device.local_strategy.empty() && device.function.empty()) {
        const int added = EnrichDeviceViaOpenApi(device);
        if (added > 0) {
            ESP_LOGI(TAG, "Device %s: enriched %d DP(s) via Tuya IoT Thing Model + Shadow",
                     device.id.c_str(), added);
        }
    }

    // Captured before the publish moves `device` into staging.
    const size_t functions = device.function.size();
    const size_t ranges = device.status_range.size();
    const size_t strategies = device.local_strategy.size();

    // Publish under the lock; a newer scan generation discards the result
    // (pre-scan fields must not splat onto the new staging row).
    DetailFetchStatus status;
    {
        std::lock_guard<std::mutex> lock(staging_mutex_);
        status =
            PublishDetailResult(staging_, device, snapshot.generation, failed_calls, auth_dead);
    }
    if (status == DetailFetchStatus::kScanActive) {
        return status;
    }
    if (auth_dead) {
        ESP_LOGE(TAG, "Device %s: detail fetch aborted — auth failed", id.c_str());
    } else if (failed_calls > 0) {
        ESP_LOGW(TAG,
                 "Device %s: detail degraded (%d failed call(s)) — not cached, a later "
                 "fetch retries",
                 id.c_str(), failed_calls);
    } else {
        ESP_LOGI(TAG, "Device %s: detail loaded (functions=%u ranges=%u strategies=%u)", id.c_str(),
                 static_cast<unsigned>(functions), static_cast<unsigned>(ranges),
                 static_cast<unsigned>(strategies));
    }
    return status;
}

void DiscoveryService::DiscoveryTask(void* arg) {
    auto* service = static_cast<DiscoveryService*>(arg);
    service->RunDiscovery();
    service->running_.store(false);
    vTaskDelete(nullptr);
}

void DiscoveryService::NotifyProgress() {
    DiscoveryProgress snapshot;
    ProgressListener listener;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const int total = progress_.homes_total + progress_.devices_total;
        const int done = progress_.homes_done + progress_.devices_done;
        progress_.percent = (total > 0) ? (done * 100 / total) : 0;
        snapshot = progress_;
        listener = progress_listener_;
    }
    if (listener) {
        listener(snapshot);  // outside the lock — see the contract
    }
}

void DiscoveryService::Finish(bool failed, const std::string& error) {
    DiscoveryProgress snapshot;
    ProgressListener listener;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        progress_.done = true;
        progress_.failed = failed;
        progress_.error = error;
        if (!failed) {
            progress_.percent = 100;
        }
        snapshot = progress_;
        listener = progress_listener_;
    }
    // Terminal staging state: a failed run proposes nothing.
    size_t candidate_count = 0;
    {
        std::lock_guard<std::mutex> lock(staging_mutex_);
        staging_.active = false;
        if (failed) {
            staging_.done = false;
            staging_.failed = true;
            staging_.candidates.clear();
            staging_.homes.clear();
        }
        candidate_count = staging_.candidates.size();
    }
    if (failed) {
        ESP_LOGE(TAG, "Discovery FAILED: homes=%d/%d devices=%d/%d — %s", snapshot.homes_done,
                 snapshot.homes_total, snapshot.devices_done, snapshot.devices_total,
                 error.c_str());
    } else {
        ESP_LOGI(TAG, "Discovery complete: homes=%d devices=%d (staging holds %u candidates)",
                 snapshot.homes_total, snapshot.devices_done,
                 static_cast<unsigned>(candidate_count));
    }
    if (listener) {
        listener(snapshot);
    }
}

void DiscoveryService::RunDiscovery() {
    CustomerApi& api = CustomerApi::GetInstance();

    {
        std::lock_guard<std::mutex> lock(mutex_);
        progress_ = DiscoveryProgress{};  // fresh snapshot for this run
    }
    NotifyProgress();

    // Staging from a clean slate; the registry is only written by CommitSelection.
    {
        std::lock_guard<std::mutex> lock(staging_mutex_);
        staging_ = DiscoveryStaging{};
        staging_.active = true;
        staging_.generation = ++scan_generation_;
    }

    if (!api.IsConfigured()) {
        Finish(true, "CustomerApi not configured");
        return;
    }

    // ---- Step 1: homes ----
    ESP_LOGI(TAG, "Fetching homes (%.*s)", static_cast<int>(tuya_cloud::kPathHomes.size()),
             tuya_cloud::kPathHomes.data());
    ApiResponse resp =
        RequestWithRetry(HttpMethod::kGet, std::string(tuya_cloud::kPathHomes), "", "homes list");
    if (!resp.ok) {
        Finish(true, "homes request failed: " + resp.msg);
        return;
    }
    std::vector<Home> homes;  // task-local; staged at the end
    if (!ParseHomes(resp.result, homes)) {
        Finish(true, "homes result is not a JSON array");
        return;
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        progress_.homes_total = static_cast<int>(homes.size());
    }
    ESP_LOGI(TAG, "Found %d home(s)", static_cast<int>(homes.size()));
    NotifyProgress();

    // ---- Step 2: per home, the device list ----
    std::vector<Device> candidates;  // task-local; staged at the end
    for (const Home& home : homes) {
        resp = RequestWithRetry(HttpMethod::kGet, std::string(tuya_cloud::kPathDevicesByHome),
                                HomeIdParams(home.id), "device list");
        if (!resp.ok) {
            if (IsAuthDead(resp)) {
                // Dead credentials: abort now.
                Finish(true, "auth failed while fetching devices: " + resp.msg);
                return;
            }
            // One home failing does NOT abort the run — skip it.
            ESP_LOGW(TAG, "Home %s (%s): device list failed (%s) — skipping this home",
                     home.id.c_str(), home.name.c_str(), resp.msg.c_str());
            {
                std::lock_guard<std::mutex> lock(mutex_);
                progress_.homes_done++;
            }
            NotifyProgress();
            continue;
        }
        if (resp.result == nullptr || !cJSON_IsArray(resp.result)) {
            ESP_LOGW(TAG, "Home %s (%s): device list result is not a JSON array — skipping",
                     home.id.c_str(), home.name.c_str());
            {
                std::lock_guard<std::mutex> lock(mutex_);
                progress_.homes_done++;
            }
            NotifyProgress();
            continue;
        }

        // ---- Step 3: stage the device rows; per-device detail is fetched
        // lazily (FetchCandidateDetail) on expand/commit ----
        for (const cJSON* item = resp.result->child; item != nullptr; item = item->next) {
            Device device;  // fresh object: discovery REPLACES, never merges
            if (!ParseDevice(item, device)) {
                ESP_LOGW(TAG, "Home %s: device entry without a usable id — skipped",
                         home.id.c_str());
                continue;
            }
            device.home_id = home.id;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                progress_.devices_total++;
            }

            // The scan row (id/name/status/...) is staged as-is;
            // specifications, strategy and report types come later.
            candidates.push_back(std::move(device));
            {
                std::lock_guard<std::mutex> lock(mutex_);
                progress_.devices_done++;
            }
            // `device` was moved from — use the vector's back().
            const Device& staged = candidates.back();
            ESP_LOGI(TAG, "Device %s (%s, category=%s): online=%d status=%u", staged.id.c_str(),
                     staged.name.c_str(), staged.category.c_str(), staged.online ? 1 : 0,
                     static_cast<unsigned>(staged.status.size()));
            NotifyProgress();
        }

        {
            std::lock_guard<std::mutex> lock(mutex_);
            progress_.homes_done++;
        }
        NotifyProgress();
    }

    // The results become the staging snapshot (the single write point).
    {
        std::lock_guard<std::mutex> lock(staging_mutex_);
        staging_.candidates = std::move(candidates);
        staging_.homes = std::move(homes);
        staging_.done = true;
        staging_.active = false;  // the run is logically complete
    }

    Finish(false, "");
}

}  // namespace hub_core
