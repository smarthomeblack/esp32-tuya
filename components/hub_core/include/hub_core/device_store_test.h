#pragma once

namespace hub_core {

/// DeviceStore serialization self-test: byte-exact golden JSON, round-trip,
/// forward-compat, corruption; pure CPU, true only when all pass.
bool RunDeviceStoreSelfTest();

}  // namespace hub_core
