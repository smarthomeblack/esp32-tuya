#pragma once

#include <cstdint>
#include <map>
#include <memory_resource>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include "hub_core/dp_value.h"

namespace hub_core {

// PSRAM pool via the pmr default resource (set by InitHubPsramPmrDefault
// in Application::Start — build no Device before it).
using DeviceString = std::pmr::string;

// Transparent: basic_string lacks a cross-allocator operator<, so any key
// mix compares as string_view; order matches std::less<std::string>.
struct DeviceStringLess {
    using is_transparent = void;
    template <typename L, typename R> bool operator()(const L& l, const R& r) const {
        return std::string_view(l) < std::string_view(r);
    }
};

using DeviceCodeSet = std::pmr::set<DeviceString, DeviceStringLess>;
template <typename T> using DeviceMap = std::pmr::map<DeviceString, T, DeviceStringLess>;
using DeviceEnumMap = DeviceMap<DeviceString>;

/// A device function from the specifications call; `values_json` is the
/// raw JSON string exactly as sent.
struct DeviceFunction {
    DeviceString code;
    DeviceString name;
    DeviceString desc;
    DeviceString type;
    DeviceString values_json;
};

/// A status range entry; `report_type` is filled by the dp-report-types
/// call — "sum" | "minux" (protocol typo) | "un_known", stored verbatim.
struct DeviceStatusRange {
    DeviceString code;
    /// Display name from the OpenAPI Thing Model; "" for purely-sharing devices.
    DeviceString name;
    DeviceString type;
    DeviceString values_json;
    DeviceString report_type;
};

using DeviceFunctionMap = DeviceMap<DeviceFunction>;
using DeviceStatusRangeMap = DeviceMap<DeviceStatusRange>;

/// Raw-dp -> standard-code conversion config for one dpId. The standard
/// code is the single key of `status_format_json`.
struct StrategyConfig {
    int dp_id = 0;
    bool support_local = false;
    DeviceString value_convert;
    DeviceString status_code;
    DeviceString status_format_json;
    DeviceString value_type;
    DeviceString value_desc_json;
    DeviceEnumMap enum_mapping;  // raw -> mapped value

    /// DP discovered via OpenAPI (not Device Sharing); commands route through OpenAPI.
    bool use_open_api = false;
    /// True routes commands via the shadow property-issue path.
    bool property_update = false;
};

using DeviceStrategyMap = std::pmr::map<int, StrategyConfig>;
using DeviceStatusMap = DeviceMap<DpValue>;
using DeviceTimestampMap = DeviceMap<int64_t>;

/// A room from the Tuya IoT room list; both strings are the cloud values.
struct Room {
    DeviceString id;
    DeviceString name;
    int sort = 0;  // cloud display order; 0 = unsorted
};

/// A home; `id` IS the ownerId string (numeric is stringified).
struct Home {
    DeviceString id;
    DeviceString name;
    std::pmr::vector<Room> rooms;  // empty = rooms unknown, not "no rooms"
};

/// CustomerDevice model plus locally-assigned home_id and client flags.
/// support_local is only set on a successful /status call.
struct Device {
    // ---- splat fields from the device-list response ----
    DeviceString id;
    DeviceString name;
    DeviceString category;  // product category code ("dj", "cz", "kg", ...)
    DeviceString product_id;
    DeviceString product_name;
    DeviceString model;
    DeviceString uuid;
    DeviceString asset_id;
    DeviceString icon;
    DeviceString time_zone;
    bool sub = false;  // gateway sub-device
    bool online = false;
    int64_t active_time = 0;  // last pairing time (unix s)
    int64_t create_time = 0;  // first pairing time (unix s)
    int64_t update_time = 0;  // status update time (unix s)

    // ---- locally assigned / client-side ----
    DeviceString home_id;  // home (ownerId string) that listed this device
    DeviceString room_id;  // room in that home; "" = unassigned/unknown
    bool set_up = false;
    bool support_local = false;

    // ---- status (current DP states) and instruction sets ----
    DeviceStatusMap status;
    DeviceFunctionMap function;
    DeviceStatusRangeMap status_range;
    DeviceStrategyMap local_strategy;

    /// DP report timestamps (ms) from MQTT protocol 4; recorded only, never compared.
    DeviceTimestampMap dp_timestamps;

    /// DP codes the user DESELECTED; an absent code is enabled. Commands
    /// and status updates for disabled codes are rejected.
    DeviceCodeSet disabled_entities;
};

}  // namespace hub_core
