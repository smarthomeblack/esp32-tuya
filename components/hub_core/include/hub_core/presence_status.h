#pragma once

#include <cstddef>

#include "hub_core/device.h"

namespace hub_core {

class DeviceRegistry;

/// Outcome of merging one cloud-polled device into the registry.
struct PresenceApplyResult {
    size_t applied = 0;    ///< codes written (each fired one kDeviceUpdated)
    size_t unchanged = 0;  ///< codes left alone (value already equal, or disabled)
};

/// Merges one cloud-polled device's status into the registry, writing ONLY the
/// codes whose value differs — a steady-state poll fires no event. An unknown
/// device id merges nothing (both counters stay 0).
PresenceApplyResult ApplyPresenceStatus(DeviceRegistry& registry, const Device& device);

}  // namespace hub_core
