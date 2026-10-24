#pragma once

/// INTERNAL (src-only) — do not include from public headers.
/// cJSON -> model parse helpers (each takes an ApiResponse `result`
/// node). Robustness: skip malformed entries, keep defaults.

#include <cJSON.h>

#include <vector>

#include "hub_core/device.h"
#include "hub_core/dp_value.h"

namespace hub_core {

/// result = array of {ownerId, name}; Home.id IS the ownerId (numeric
/// stringified). False when not an array.
bool ParseHomes(const cJSON* result, std::vector<Home>& homes);

/// One device-list item (splat fields; status list -> map). False without
/// a usable "id"; `home_id` is assigned by the caller.
bool ParseDevice(const cJSON* item, Device& device);

/// result = {functions, status}; `values` kept as the raw JSON string.
/// Replaces function/status_range wholesale.
void ParseSpecifications(const cJSON* result, Device& device);

/// support_local = AND of the DTOs' supportLocal; local_strategy is
/// replaced only when true. Only call on a SUCCESSFUL response.
void ParseStatusStrategy(const cJSON* result, Device& device);

/// result = array of {dp_code, report_type} — dp_code is the STANDARD code
/// (not a dpId), stored verbatim on existing entries only.
void ParseDpReportTypes(const cJSON* result, Device& device);

/// cJSON value -> DpValue (see DpValue for the typing rules); a null or
/// absent node -> the empty kString ("use default") form.
DpValue ParseDpValue(const cJSON* node);

/// result = {rooms: [{room_id, name, sort}]} (the live OpenAPI home object);
/// a bare array of rooms is tolerated. Entries without an id are skipped.
bool ParseRooms(const cJSON* result, std::vector<Room>& rooms);

/// result = room devices: full device objects (the "id" is collected) or bare
/// id strings. Duplicates are dropped; a non-list root is false.
bool ParseRoomDevices(const cJSON* result, std::vector<std::string>& device_ids);

}  // namespace hub_core
