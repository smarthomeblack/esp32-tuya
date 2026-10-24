#pragma once

#include <cstddef>

namespace tuya_crypto {

// cJSON allows 1000 nesting; task stacks are 8-12 KB — cap before parse.
constexpr int kMaxJsonDepth = 32;

/// Pre-parse depth check: true when bracket nesting stays <= max_depth and
/// brackets balance. JSON string contents are skipped ('"' toggle, '\' escapes).
bool JsonNestingWithinLimit(const char* body, size_t len, int max_depth);

}  // namespace tuya_crypto
