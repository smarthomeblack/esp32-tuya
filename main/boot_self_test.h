#pragma once

namespace boot_self_test {

/// Runs every boot self-test gate, then the post-pass OTA mark-valid.
/// False on the first failure — the caller parks in FatalError.

bool RunAllGates();

}  // namespace boot_self_test
