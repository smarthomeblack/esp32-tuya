// cJSON -> model parse helpers shared by discovery and the public wrappers.

#include <cJSON.h>

#include <algorithm>
#include <climits>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>
#include <utility>
#include <vector>

#include "tuya_crypto/json_fields.h"

#include "parse_helpers.h"

namespace hub_core {

namespace {

/// Guarded number -> int (naive casts truncate fractions / saturate overflow).
bool JsonIntValue(const cJSON* node, int& out) {
    if (!cJSON_IsNumber(node)) {
        return false;
    }
    const double value = node->valuedouble;
    if (std::isfinite(value) && value == std::floor(value) &&
        value >= static_cast<double>(INT_MIN) && value <= static_cast<double>(INT_MAX)) {
        out = static_cast<int>(value);
        return true;
    }
    return false;
}

/// Guarded number -> int64 (naive casts truncate fractions / saturate overflow).
bool JsonIntValue(const cJSON* node, int64_t& out) {
    if (!cJSON_IsNumber(node)) {
        return false;
    }
    const double value = node->valuedouble;
    if (std::isfinite(value) && value == std::floor(value) &&
        value >= static_cast<double>(INT64_MIN) && value < static_cast<double>(INT64_MAX)) {
        out = static_cast<int64_t>(value);
        return true;
    }
    return false;
}

/// A node's value as text (containers re-serialized; empty on absence).
std::string JsonNodeAsText(const cJSON* node) {
    if (node == nullptr) {
        return "";
    }
    if (cJSON_IsString(node)) {
        return node->valuestring != nullptr ? node->valuestring : "";
    }
    if (cJSON_IsBool(node)) {
        return cJSON_IsTrue(node) ? "true" : "false";
    }
    if (cJSON_IsNumber(node)) {
        char buf[32];
        const int n = std::snprintf(buf, sizeof(buf), "%.17g", node->valuedouble);
        if (n > 0 && n < static_cast<int>(sizeof(buf))) {
            return std::string(buf, static_cast<size_t>(n));
        }
        return "0";
    }
    if (cJSON_IsObject(node) || cJSON_IsArray(node)) {
        char* printed = cJSON_PrintUnformatted(node);
        if (printed == nullptr) {
            return "";
        }
        std::string out(printed);
        cJSON_free(printed);
        return out;
    }
    return "";
}

/// The `values`-style fields: JSON strings; nested shapes tolerated.
std::string JsonFieldAsText(const cJSON* object, const char* key) {
    return JsonNodeAsText(cJSON_GetObjectItemCaseSensitive(object, key));
}

/// One DP value; false for JSON null/absent/non-finite (nothing to store).
bool TryParseDpValue(const cJSON* node, DpValue& out) {
    if (node == nullptr || cJSON_IsNull(node)) {
        return false;
    }
    if (cJSON_IsBool(node)) {
        out = DpValue::FromBool(cJSON_IsTrue(node));
        return true;
    }
    if (cJSON_IsNumber(node)) {
        const double value = node->valuedouble;
        // cJSON turns 1e400 into inf, which re-serializes as invalid JSON.
        if (!std::isfinite(value)) {
            return false;
        }
        // Integral int64-range numbers -> kInt; everything else -> kDouble.
        if (value == std::floor(value) && value >= static_cast<double>(INT64_MIN) &&
            value < static_cast<double>(INT64_MAX)) {
            out = DpValue::FromInt(static_cast<int64_t>(value));
        } else {
            out = DpValue::FromDouble(value);
        }
        return true;
    }
    if (cJSON_IsString(node)) {
        out = DpValue::FromString(node->valuestring != nullptr ? node->valuestring : "");
        return true;
    }
    // Objects/arrays have no DpValue form — defensive empty string.
    out = DpValue::FromString("");
    return true;
}

/// An id that arrives as a string OR a number (Tuya mixes both); "" otherwise.
std::string JsonStringOrIntId(const cJSON* node) {
    if (cJSON_IsString(node) && node->valuestring != nullptr) {
        return node->valuestring;
    }
    int64_t value = 0;
    if (JsonIntValue(node, value)) {
        return std::to_string(value);
    }
    return "";
}

}  // namespace

DpValue ParseDpValue(const cJSON* node) {
    DpValue out;
    TryParseDpValue(node, out);  // invalid -> the default empty kString form
    return out;
}

bool ParseHomes(const cJSON* result, std::vector<Home>& homes) {
    if (!cJSON_IsArray(result)) {
        return false;
    }
    for (const cJSON* item = result->child; item != nullptr; item = item->next) {
        if (!cJSON_IsObject(item)) {
            continue;
        }
        // The home id IS the ownerId string (numeric is stringified).
        std::string id;
        const cJSON* owner_id = cJSON_GetObjectItemCaseSensitive(item, "ownerId");
        if (cJSON_IsString(owner_id) && owner_id->valuestring != nullptr) {
            id = owner_id->valuestring;
        } else if (cJSON_IsNumber(owner_id)) {
            int64_t owner_value = 0;
            if (JsonIntValue(owner_id, owner_value)) {
                id = std::to_string(owner_value);
            }
        }
        if (id.empty()) {
            continue;  // skip the unusable row
        }
        Home home;
        home.id = id;
        home.name = tuya_crypto::GetStringField(item, "name");
        homes.push_back(std::move(home));
    }
    return true;
}

bool ParseDevice(const cJSON* item, Device& device) {
    if (!cJSON_IsObject(item)) {
        return false;
    }
    device.id = tuya_crypto::GetStringField(item, "id");
    if (device.id.empty()) {
        return false;
    }
    device.name = tuya_crypto::GetStringField(item, "name");
    device.category = tuya_crypto::GetStringField(item, "category");
    device.product_id = tuya_crypto::GetStringField(item, "product_id");
    device.product_name = tuya_crypto::GetStringField(item, "product_name");
    device.model = tuya_crypto::GetStringField(item, "model");
    device.uuid = tuya_crypto::GetStringField(item, "uuid");
    device.asset_id = tuya_crypto::GetStringField(item, "asset_id");
    device.icon = tuya_crypto::GetStringField(item, "icon");
    device.time_zone = tuya_crypto::GetStringField(item, "time_zone");
    device.sub = tuya_crypto::GetBoolField(item, "sub");
    device.online = tuya_crypto::GetBoolField(item, "online");
    device.active_time = tuya_crypto::GetInt64Field(item, "active_time");
    device.create_time = tuya_crypto::GetInt64Field(item, "create_time");
    device.update_time = tuya_crypto::GetInt64Field(item, "update_time");

    // status: LIST of {code, value} -> map; "value" may be JSON null.
    const cJSON* status = cJSON_GetObjectItemCaseSensitive(item, "status");
    if (cJSON_IsArray(status)) {
        for (const cJSON* entry = status->child; entry != nullptr; entry = entry->next) {
            if (!cJSON_IsObject(entry)) {
                continue;
            }
            const cJSON* code = cJSON_GetObjectItemCaseSensitive(entry, "code");
            const cJSON* value = cJSON_GetObjectItemCaseSensitive(entry, "value");
            DpValue parsed;
            if (cJSON_IsString(code) && code->valuestring != nullptr &&
                TryParseDpValue(value, parsed)) {
                device.status[code->valuestring] = std::move(parsed);
            }
        }
    }
    return true;
}

void ParseSpecifications(const cJSON* result, Device& device) {
    if (!cJSON_IsObject(result)) {
        return;
    }
    DeviceFunctionMap functions;
    const cJSON* fns = cJSON_GetObjectItemCaseSensitive(result, "functions");
    if (cJSON_IsArray(fns)) {
        for (const cJSON* fn = fns->child; fn != nullptr; fn = fn->next) {
            if (!cJSON_IsObject(fn)) {
                continue;
            }
            const DeviceString code{tuya_crypto::GetStringField(fn, "code")};
            if (code.empty()) {
                continue;
            }
            DeviceFunction function;
            function.code = code;
            function.name = tuya_crypto::GetStringField(fn, "name");
            function.desc = tuya_crypto::GetStringField(fn, "desc");
            function.type = tuya_crypto::GetStringField(fn, "type");
            function.values_json = JsonFieldAsText(fn, "values");
            // Last-wins on duplicate codes (emplace would be first-wins).
            functions.insert_or_assign(code, std::move(function));
        }
    }
    DeviceStatusRangeMap status_ranges;
    const cJSON* statuses = cJSON_GetObjectItemCaseSensitive(result, "status");
    if (cJSON_IsArray(statuses)) {
        for (const cJSON* st = statuses->child; st != nullptr; st = st->next) {
            if (!cJSON_IsObject(st)) {
                continue;
            }
            const DeviceString code{tuya_crypto::GetStringField(st, "code")};
            if (code.empty()) {
                continue;
            }
            DeviceStatusRange range;
            range.code = code;
            range.type = tuya_crypto::GetStringField(st, "type");
            range.values_json = JsonFieldAsText(st, "values");
            // Last-wins on duplicate codes; report_type filled later.
            status_ranges.insert_or_assign(code, std::move(range));
        }
    }
    device.function = std::move(functions);
    device.status_range = std::move(status_ranges);
}

void ParseStatusStrategy(const cJSON* result, Device& device) {
    // Only on a SUCCESSFUL /status response — else leave defaults.
    // support_local needs at least one usable DTO; absent/empty = unknown -> false.
    bool support_local = false;
    DeviceStrategyMap strategies;
    if (cJSON_IsObject(result)) {
        const cJSON* dtos = cJSON_GetObjectItemCaseSensitive(result, "dpStatusRelationDTOS");
        if (cJSON_IsArray(dtos)) {
            for (const cJSON* dto = dtos->child; dto != nullptr; dto = dto->next) {
                if (!cJSON_IsObject(dto)) {
                    continue;
                }
                if (!tuya_crypto::GetBoolField(dto, "supportLocal")) {
                    // ANY non-local DP drops the whole device to false.
                    support_local = false;
                    break;
                }
                const cJSON* dp_id = cJSON_GetObjectItemCaseSensitive(dto, "dpId");
                int dp_id_value = 0;
                if (!JsonIntValue(dp_id, dp_id_value)) {
                    continue;  // skip the entry
                }
                support_local = true;  // first usable DTO; a later non-local one clears it
                StrategyConfig cfg;
                cfg.dp_id = dp_id_value;
                cfg.support_local = true;
                cfg.value_convert = tuya_crypto::GetStringField(dto, "valueConvert");
                cfg.status_code = tuya_crypto::GetStringField(dto, "statusCode");
                cfg.status_format_json = JsonFieldAsText(dto, "statusFormat");
                cfg.value_type = tuya_crypto::GetStringField(dto, "valueType");
                cfg.value_desc_json = JsonFieldAsText(dto, "valueDesc");
                // enumMappingMap {raw: {"value": v}} -> raw -> v; valueless omitted.
                const cJSON* mapping = cJSON_GetObjectItemCaseSensitive(dto, "enumMappingMap");
                if (cJSON_IsObject(mapping)) {
                    for (const cJSON* e = mapping->child; e != nullptr; e = e->next) {
                        const cJSON* mapped = cJSON_GetObjectItemCaseSensitive(e, "value");
                        if (e->string != nullptr && mapped != nullptr && !cJSON_IsNull(mapped)) {
                            cfg.enum_mapping[e->string] = JsonNodeAsText(mapped);
                        }
                    }
                }
                // Last-wins on duplicate dpIds (emplace would be first-wins).
                strategies.insert_or_assign(cfg.dp_id, std::move(cfg));
            }
        }
    }
    // The strategy map is assigned only in the support_local case.
    device.support_local = support_local;
    if (support_local) {
        device.local_strategy = std::move(strategies);
    }
}

void ParseDpReportTypes(const cJSON* result, Device& device) {
    // dp_code is the STANDARD status code (not a dpId); values verbatim.
    if (!cJSON_IsArray(result)) {
        return;
    }
    for (const cJSON* item = result->child; item != nullptr; item = item->next) {
        if (!cJSON_IsObject(item)) {
            continue;
        }
        const cJSON* dp_code = cJSON_GetObjectItemCaseSensitive(item, "dp_code");
        const cJSON* report_type = cJSON_GetObjectItemCaseSensitive(item, "report_type");
        if (cJSON_IsString(dp_code) && dp_code->valuestring != nullptr &&
            dp_code->valuestring[0] != '\0' && cJSON_IsString(report_type) &&
            report_type->valuestring != nullptr) {
            auto it = device.status_range.find(dp_code->valuestring);
            if (it != device.status_range.end()) {
                it->second.report_type = report_type->valuestring;
            }
            // Unknown dp_code is silently ignored.
        }
    }
}

bool ParseRooms(const cJSON* result, std::vector<Room>& rooms) {
    if (!cJSON_IsObject(result) && !cJSON_IsArray(result)) {
        return false;
    }
    // The live home object nests the list under "rooms"; a bare array is tolerated.
    const cJSON* list =
        cJSON_IsArray(result) ? result : cJSON_GetObjectItemCaseSensitive(result, "rooms");
    if (!cJSON_IsArray(list)) {
        return false;
    }
    for (const cJSON* item = list->child; item != nullptr; item = item->next) {
        if (!cJSON_IsObject(item)) {
            continue;
        }
        Room room;
        // room_id is a JSON NUMBER in the live payload; a string id is tolerated.
        room.id = JsonStringOrIntId(cJSON_GetObjectItemCaseSensitive(item, "room_id"));
        if (room.id.empty()) {
            room.id = JsonStringOrIntId(cJSON_GetObjectItemCaseSensitive(item, "id"));
        }
        if (room.id.empty()) {
            continue;  // skip the unusable row
        }
        room.name = tuya_crypto::GetStringField(item, "name");
        int sort = 0;
        if (JsonIntValue(cJSON_GetObjectItemCaseSensitive(item, "sort"), sort)) {
            room.sort = sort;
        }
        rooms.push_back(std::move(room));
    }
    return true;
}

bool ParseRoomDevices(const cJSON* result, std::vector<std::string>& device_ids) {
    if (!cJSON_IsArray(result)) {
        return false;
    }
    for (const cJSON* item = result->child; item != nullptr; item = item->next) {
        std::string id;
        if (cJSON_IsObject(item)) {
            id = tuya_crypto::GetStringField(item, "id");
        } else if (cJSON_IsString(item) && item->valuestring != nullptr) {
            id = item->valuestring;
        }
        if (id.empty()) {
            continue;
        }
        if (std::find(device_ids.begin(), device_ids.end(), id) == device_ids.end()) {
            device_ids.push_back(std::move(id));
        }
    }
    return true;
}

}  // namespace hub_core
