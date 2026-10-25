#include "hub_core/presence_status.h"

#include <string_view>

#include "hub_core/device_registry.h"

namespace hub_core {

namespace {

/// DP report time from the poll payload; absent keeps the registry's own stamp.
int64_t PollTimestampMs(const Device& device, std::string_view code) {
    const auto it = device.dp_timestamps.find(code);
    return it == device.dp_timestamps.end() ? 0 : it->second;
}

}  // namespace

PresenceApplyResult ApplyPresenceStatus(DeviceRegistry& registry, const Device& device) {
    PresenceApplyResult result;
    Device stored;
    if (!registry.GetDevice(device.id, stored)) {
        return result;  // not committed locally — nothing to merge into
    }
    for (const auto& [code, value] : device.status) {
        // A disabled entity is invisible to reports; skip before the write.
        if (stored.disabled_entities.count(code) != 0) {
            ++result.unchanged;
            continue;
        }
        const auto current = stored.status.find(code);
        if (current != stored.status.end() && current->second == value) {
            ++result.unchanged;
            continue;
        }
        if (registry.UpdateStatus(device.id, code, value, PollTimestampMs(device, code))) {
            ++result.applied;
        }
    }
    return result;
}

}  // namespace hub_core
