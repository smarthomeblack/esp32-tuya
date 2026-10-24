#pragma once

/// INTERNAL (src-only) — do not include from public headers.
/// Pure DeviceStore serialization helpers (no flash I/O). Deterministic
/// output: same model -> same bytes.

#include <cJSON.h>

#include <cstddef>
#include <string>
#include <vector>

#include "hub_core/device.h"
#include "hub_core/device_store.h"

namespace hub_core {

/// Where ParseStoreJson renames an unsupported-version snapshot so the next
/// save cannot silently overwrite it; Clear() removes it as stale state.
inline constexpr const char* kBadRegistryPath = "/littlefs/registry.json.bad";

/// Device JSON -> `out`; false without a usable "id". Corrupt fields are
/// dropped, unknown ignored, missing keep defaults.
bool DeviceFromJson(const cJSON* node, Device& out);

/// The full registry.json body (compact, deterministic); saved_at passed in.
std::string BuildStoreJson(const std::vector<Home>& homes, const std::vector<Device>& devices,
                           const std::string& uid, int64_t saved_at_unix);

/// Same body, streamed: `device_source` feeds the devices one by one (no
/// vector materialized); `device_count` (nullable) gets the emitted count.
std::string BuildStoreJsonStreamed(const std::vector<Home>& homes, const std::string& uid,
                                   int64_t saved_at_unix, const DeviceSource& device_source,
                                   size_t* device_count);

/// registry.json text -> `homes`/`devices`/`stored_uid` (replaced). False
/// when unparsable/unsupported version; missing arrays -> empty.
bool ParseStoreJson(const std::string& text, std::vector<Home>& homes, std::vector<Device>& devices,
                    std::string& stored_uid);

}  // namespace hub_core
