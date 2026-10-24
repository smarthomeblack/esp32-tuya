#pragma once

#include <atomic>
#include <cstddef>
#include <functional>
#include <mutex>
#include <set>
#include <string>
#include <vector>

#include "hub_core/device.h"

namespace hub_core {

/// Progress snapshot, delivered per home/device; done=true on success,
/// failed=true (with error) on the terminal event.
struct DiscoveryProgress {
    int homes_done = 0;
    int homes_total = 0;
    int devices_done = 0;
    int devices_total = 0;
    int percent = 0;
    bool done = false;
    bool failed = false;
    std::string error;
};

/// One user selection (POST /api/discovery/commit); disabled_entities =
/// (candidate codes) - enabled_entities. Missing field = all enabled.
struct DiscoveryCommitEntry {
    std::string id;
    DeviceCodeSet enabled_entities;
};

/// Snapshot of the last COMPLETED scan (see GetStaging).
struct DiscoveryStaging {
    bool active = false;  ///< a scan is running right now
    bool done = false;    ///< a scan completed (cleared by the next Start)
    bool failed = false;  ///< the last scan failed (done stays false)
    /// Stamped at every staging reset; a detail fetch from an older
    /// generation must not publish into the current staging.
    unsigned generation = 0;
    std::vector<Device> candidates;  ///< scan rows; detail fetched lazily
    std::vector<Home> homes;         ///< the scan's home list
    /// Candidate ids whose detail fetch fully succeeded; degraded fetches
    /// stay out so a later fetch retries. Reset by the next Start.
    std::set<std::string> detail_loaded;
};

/// Outcome of FetchCandidateDetail (lazy per-candidate detail fetch).
enum class DetailFetchStatus {
    kLoaded,         ///< staging now holds the full model
    kDegraded,       ///< partial model staged, not cached — a later fetch retries
    kAlreadyLoaded,  ///< detail was already fetched — no HTTP was made
    kNoScan,         ///< no completed scan to fetch from
    kNotFound,       ///< the id is not a staged candidate
    kScanActive,     ///< a scan is running (or raced in mid-fetch)
    kBusy,           ///< another detail fetch is in flight
};

/// Outcome of CommitSelection: counts by disposition. `scan_active`
/// means a scan was running and nothing was applied.
struct CommitSelectionResult {
    bool scan_active = false;
    int added = 0;      ///< new devices inserted
    int refreshed = 0;  ///< detail-loaded, richer + non-shrinking models refreshed
    int skipped = 0;    ///< not-in-staging, degraded, equal/poorer or shrinking models
};

/// Cloud discovery staged for user review; CommitSelection is the ONLY
/// registry writer (runs on the "tuya_disco" task; listeners must be fast).
class DiscoveryService {
public:
    static DiscoveryService& GetInstance() {
        static DiscoveryService instance;
        return instance;
    }
    DiscoveryService(const DiscoveryService&) = delete;
    DiscoveryService& operator=(const DiscoveryService&) = delete;

    using ProgressListener = std::function<void(const DiscoveryProgress&)>;

    /// Spawns the task; false when already running or spawn fails.
    bool Start();

    bool IsRunning() const { return running_.load(); }

    /// Mutex-guarded copy of the live progress snapshot — safe from any task.
    DiscoveryProgress GetProgress() const;

    /// Sets (replaces) the progress listener — see the contract above.
    void SetProgressListener(ProgressListener listener);

    /// Snapshot of the last completed scan; a failed run leaves no candidates.
    DiscoveryStaging GetStaging() const;

    /// The staged candidate count only (cheaper than GetStaging()).
    size_t GetStagingCandidateCount() const;

    /// Applies the selection to the registry (idempotent; unknown ids
    /// skipped); returns per-disposition counts, scan_active on a race.
    CommitSelectionResult CommitSelection(const std::vector<DiscoveryCommitEntry>& entries);

    /// Lazily runs the per-device follow-up calls (specifications, status
    /// strategy, dp report types) plus OpenAPI enrichment for ONE staged
    /// candidate and updates the staging entry. Failed calls stage the
    /// partial model and return kDegraded (not cached — retried later).
    /// Blocking HTTP — run on a worker task, never an httpd or main-loop
    /// context. Single-flight: kBusy while another fetch is in flight.
    DetailFetchStatus FetchCandidateDetail(const std::string& id);

private:
    DiscoveryService() = default;

    static void DiscoveryTask(void* arg);

    void RunDiscovery();

    /// Delivers one progress snapshot (listener invoked outside the lock).
    void NotifyProgress();

    /// Delivers the terminal snapshot (done/failed + error).
    void Finish(bool failed, const std::string& error);

    std::atomic<bool> running_{false};
    /// Guards progress_listener_ and progress_.
    mutable std::mutex mutex_;
    ProgressListener progress_listener_;
    DiscoveryProgress progress_;
    /// Guards staging; never held with mutex_ or across an HTTP call.
    mutable std::mutex staging_mutex_;
    DiscoveryStaging staging_;
    /// Single-flight guard for FetchCandidateDetail (one fetch at a time).
    std::atomic<bool> detail_fetch_running_{false};
    /// Bumped at every staging reset; stale detail fetches detect it.
    std::atomic<unsigned> scan_generation_{0};
};

}  // namespace hub_core
