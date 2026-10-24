#pragma once

/// INTERNAL (src-only) — do not include from public headers.
/// OpenAPI enrichment parse helpers (no network); malformed rows skipped.

#include <cJSON.h>

#include "hub_core/device.h"

namespace hub_core {

/// Parses the double-encoded Thing Model into local_strategy +
/// function/status_range (sharing-wins). Returns strategies added.
int ParseThingModel(const cJSON* result, Device& device);

/// Fills status[code] and minimal local_strategy[dp_id] for unknown dpIds
/// (sharing-wins). Returns the strategies added.
int ParseShadowProperties(const cJSON* result, Device& device);

}  // namespace hub_core
