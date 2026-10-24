#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <vector>

#include "hub_core/dp_value.h"

namespace hub_core {

/// One device command: standard dp code + value (no raw-dpId conversion).
struct DeviceCommand {
    std::string code;
    DpValue value;

    /// Full-content equality for the dedup filter; element order matters.
    bool operator==(const DeviceCommand& other) const {
        return code == other.code && value == other.value;
    }
};

/// One scene row; `actions` not carried (unused for triggering).
struct Scene {
    std::string scene_id;
    std::string name;
    bool enabled = false;
    std::string home_id;
};

/// Outcome of CommandService::FetchScenes (partial failures included).
struct SceneFetchResult {
    /// False only when every home failed; zero homes is vacuous success.
    bool ok = false;
    /// Homes whose scene GET or parse failed.
    size_t failed_homes = 0;
    /// Homes queried.
    size_t total_homes = 0;
};

/// Outcome of CommandService::SendDeviceCommands.
enum class CommandResult {
    /// HTTP call completed; the response `ok` flag is checked — only the
    /// result payload content is unused.
    kSent,
    /// Identical commands within 10 s — suppressed by the dedup filter.
    kDeduplicated,
    /// Invalid input or a pipeline/HTTP error (already logged).
    kError,
};

class DedupFilter;  // internal dedup state — src/dedup_filter.h (opaque here)
struct Device;      // hub_core/device.h — routing input (used by reference)

/// OpenAPI quota snapshot (GET /api/openapi/quota): 10 controllable
/// devices/month; warning at >= 8.
struct OpenApiQuotaInfo {
    int month = 0;
    size_t devices = 0;
    size_t limit = 10;
    bool warning = false;
};

/// Device-command + scene service. Cloud methods are blocking HTTP
/// (60 s, no retries) — never call from an httpd handler.
class CommandService {
public:
    /// Process-wide singleton (one hub, one account).
    static CommandService& GetInstance() {
        static CommandService instance;
        return instance;
    }
    CommandService(const CommandService&) = delete;
    CommandService& operator=(const CommandService&) = delete;

    /// Dedup gates the whole list, then sharing/OpenAPI per DP; all groups
    /// must succeed. The whole chain is capped at kCloudCommandDeadlineMs —
    /// legs are skipped once it passes.
    CommandResult SendDeviceCommands(const std::string& dev_id,
                                     const std::vector<DeviceCommand>& commands);

    /// Fetches scenes for all homes, REPLACING the cache; ok is false only
    /// when every home failed. A ClearScenes() during the fetch discards the result.
    SceneFetchResult FetchScenes();

    /// Empties the cache (a different-account relink must not reuse old scenes).
    void ClearScenes();

    /// Snapshot copy of the cached scenes (a reference would dangle).
    std::vector<Scene> GetScenes() const;

    /// Triggers one scene (blocking POST; the result body is unused).
    bool TriggerScene(const std::string& home_id, const std::string& scene_id);

    /// Quota snapshot; lazily rolls the month. Thread-safe, no network.
    OpenApiQuotaInfo GetOpenApiQuota();

private:
    CommandService();
    ~CommandService();  // out-of-line: DedupFilter is a src-internal type

    /// Dedup state (dev_id -> last commands + time), guarded by mutex_.
    std::unique_ptr<DedupFilter> dedup_;

    /// Drops the dedup entry after a FAILED send so a retry is allowed.
    void ForgetDedupEntry(const std::string& dev_id);

    mutable std::mutex mutex_;
    std::vector<Scene> scenes_;
    /// Bumped by ClearScenes(); a fetch started under an older value is discarded.
    std::atomic<uint32_t> scenes_generation_{0};

    // ---- OpenAPI command path ----

    /// Sends one group; retries the whole group on the other endpoint.
    /// `deadline_ms`: absolute esp_timer-ms chain cap (0 = unbounded); legs skip past it.
    bool SendOpenApiGroup(const std::string& dev_id, const std::vector<DeviceCommand>& commands,
                          bool prefer_issue, int64_t deadline_ms);

    /// The OpenAPI leg. False when the client is unusable (no sharing
    /// fallback) or a group fails on both endpoints.
    bool SendViaOpenApi(const std::string& dev_id, const Device& device,
                        const std::vector<DeviceCommand>& commands, int64_t deadline_ms);

    // ---- OpenAPI command quota ----

    /// Quota set, NVS-persisted ("hub"/"openapi_quota"), reset monthly;
    /// guarded by mutex_.
    std::set<std::string> openapi_quota_devices_;
    int openapi_quota_month_ = 0;  // YYYYMM

    /// Records dev_id, persists on change, logs WARN >= 8 / ERROR >= 10.
    void TrackOpenApiQuota(const std::string& dev_id);

    /// Loads the quota state from NVS (constructor). Caller holds mutex_.
    void LoadOpenApiQuotaLocked();

    /// Persists the quota state to NVS. Caller holds mutex_.
    void SaveOpenApiQuotaLocked();
};

}  // namespace hub_core
