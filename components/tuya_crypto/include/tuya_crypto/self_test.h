#pragma once

namespace tuya_crypto {

/// Runs the tuya_crypto self-test against golden vectors in src/test_vectors.h.
/// Returns true only if every check passes.
bool RunSelfTest();

}  // namespace tuya_crypto
