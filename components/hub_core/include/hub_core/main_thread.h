#pragma once

namespace hub_core {

/// Records the main event loop task (opaque FreeRTOS handle; call once from
/// the loop itself). Public API stays FreeRTOS-header-free.
void SetMainThread(void* handle);

/// Observability only (never aborts): logs an error when `api` runs off the
/// main task. Silent until SetMainThread has registered the handle.
void AssertMainThread(const char* api);

}  // namespace hub_core
