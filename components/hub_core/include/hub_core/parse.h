#pragma once

#include <string>
#include <vector>

#include "hub_core/device.h"

namespace hub_core {

/// cJSON-free public wrappers over internal cJSON helpers: JSON text in,
/// helper out, tree freed; malformed input skips, bad root -> false.

/// One device item JSON (splat fields + status list) -> out; home_id is
/// assigned by the caller.
bool ParseDeviceJson(const std::string& json, Device& out);

/// The devices/detail result: parses the FIRST array entry into `out`.
/// False when the array is empty or unusable.
bool ParseDeviceDetailResultJson(const std::string& json, Device& out);

/// The specifications result; replaces function/status_range wholesale
/// (a non-object root leaves them unchanged).
void ParseSpecificationsJson(const std::string& json, Device& device);

/// The status strategy result — only call on a SUCCESSFUL response.
void ParseStatusStrategyJson(const std::string& json, Device& device);

/// The dp-report-types result; unknown codes ignored, values verbatim.
void ParseDpReportTypesJson(const std::string& json, Device& device);

/// The Tuya IoT room list (one home); `rooms` is appended to.
bool ParseRoomsJson(const std::string& json, std::vector<Room>& rooms);

/// The Tuya IoT devices of one room; `device_ids` is appended to.
bool ParseRoomDevicesJson(const std::string& json, std::vector<std::string>& device_ids);

}  // namespace hub_core
