#pragma once

namespace hub_core {

/// Golden-sample self-test for the device model (same parse helpers as
/// discovery). Pure CPU; true only when every check passes.
bool RunModelSelfTest();

}  // namespace hub_core
