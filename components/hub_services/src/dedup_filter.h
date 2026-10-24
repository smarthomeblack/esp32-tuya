#pragma once

/// INTERNAL (src-only) — do not include from public headers.
/// Clock-free command internals shared with the self-test.

#include <cJSON.h>

#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <vector>

#include "hub_services/command_service.h"

namespace hub_core {

/// 10-second command dedup filter: entries are written on allowed sends
/// only. Clock-free (now_ms injected); not thread-safe by itself.
class DedupFilter {
public:
    static constexpr int64_t kTimeLimitMs = 10000;
    /// The sweep cadence/entry-age threshold, separate from kTimeLimitMs.
    static constexpr int64_t kSweepWindowMs = 10000;

    /// Sweep, then decide + update state; true when the send is allowed.
    bool Allows(const std::string& dev_id, const std::vector<DeviceCommand>& commands,
                int64_t now_ms) {
        Sweep(now_ms);

        auto it = entries_.find(dev_id);
        if (it == entries_.end()) {
            entries_.emplace(dev_id, Entry{commands, now_ms});
            return true;
        }
        if (commands != it->second.commands || now_ms - it->second.last_ms >= kTimeLimitMs) {
            it->second = Entry{commands, now_ms};
            return true;
        }
        // Identical commands within the window: blocked, state untouched.
        return false;
    }

    /// Erases one entry: a FAILED send must not consume the dedup window.
    void Forget(const std::string& dev_id) { entries_.erase(dev_id); }

    /// Drops entries older than the sweep window, at most once per window.
    void Sweep(int64_t now_ms) {
        if (now_ms - last_sweep_ms_ < kSweepWindowMs) {
            return;
        }
        for (auto it = entries_.begin(); it != entries_.end();) {
            if (now_ms - it->second.last_ms >= kSweepWindowMs) {
                it = entries_.erase(it);
            } else {
                ++it;
            }
        }
        last_sweep_ms_ = now_ms;
    }

    size_t Size() const { return entries_.size(); }

private:
    struct Entry {
        std::vector<DeviceCommand> commands;
        int64_t last_ms = 0;
    };

    std::map<std::string, Entry> entries_;
    /// Last sweep time; starts at 0, so the first call sweeps.
    int64_t last_sweep_ms_ = 0;
};

// Pure request-build/parse helpers. Compact insertion-ordered JSON;
// kDouble serializes as %.17g (round-trip exact).

/// Builds the compact commands body. False, `body` untouched, on an
/// empty list/code or null-ish value.
bool BuildCommandsBody(const std::vector<DeviceCommand>& commands, std::string& body);

/// Scene-trigger body {"homeId":"...","sceneId":"..."}.
std::string BuildSceneTriggerBody(const std::string& home_id, const std::string& scene_id);

/// Appends one home's scenes (home_id assigned locally) to `out`. Rows
/// without a usable scene_id are skipped; non-array appends nothing.
size_t ParseScenes(const cJSON* result, std::string_view home_id, std::vector<Scene>& out);

}  // namespace hub_core
