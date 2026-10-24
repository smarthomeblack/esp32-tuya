#pragma once

namespace sys {

/// Sys self-test: LogBuffer reassembly/eviction, ParseLogLine, CPU usage
/// vectors; true only when all checks pass.
bool RunSelfTest();

}  // namespace sys
