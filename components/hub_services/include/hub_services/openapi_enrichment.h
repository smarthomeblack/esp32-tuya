#pragma once

#include "hub_core/device.h"

namespace hub_core {

/// Fills Thing Model / Shadow gaps via the OpenAPI; sharing-wins (existing
/// entries never overwritten). Returns DPs added (0 = no-op or failure).
int EnrichDeviceViaOpenApi(Device& device);

}  // namespace hub_core
