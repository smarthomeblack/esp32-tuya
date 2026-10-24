#pragma once

#include <memory_resource>

namespace hub_core {

/// The process PSRAM pmr pool (synchronized: alloc/free from any task).
std::pmr::memory_resource* GetHubPsramResource();

/// Points std::pmr::get_default_resource() at the pool. Call ONCE at the
/// top of Application::Start, BEFORE any pmr container (self-tests build
/// Devices); plain std::string/std::map are unaffected.
void InitHubPsramPmrDefault();

}  // namespace hub_core
