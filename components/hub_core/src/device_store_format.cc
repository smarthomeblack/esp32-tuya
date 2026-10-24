// registry.json serialization/deserialization helpers (device_store_internal.h).

#include <cJSON.h>
#include <esp_log.h>

#include <cerrno>
#include <charconv>
#include <climits>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

#include "tuya_crypto/json_fields.h"
#include "tuya_crypto/json_writer.h"

#include "device_store_internal.h"

static const char* TAG = "DeviceStore";

namespace hub_core {

namespace {

/// Mirrors kRegistryPath in device_store.cc.
constexpr const char* kRegistryPath = "/littlefs/registry.json";

/// Moves the registry to the single .bad slot (LittleFS rename replaces, so
/// the previous .bad goes first; errno when it failed).
int MoveAsideBadSlot() {
    std::remove(kBadRegistryPath);
    if (std::rename(kRegistryPath, kBadRegistryPath) == 0) {
        return 0;
    }
    return errno;
}

// ---- validated int64 field reader (parse side) ----

/// Unlike the shared readers: rejects non-finite / out-of-int64 doubles
/// (the naive cast would be UB) and logs the rejection.
int64_t GetValidatedInt64(const cJSON* object, const char* key) {
    const cJSON* node = cJSON_GetObjectItemCaseSensitive(object, key);
    if (cJSON_IsNumber(node)) {
        // Validate before the cast: an out-of-range double -> int64 would be UB.
        const double v = node->valuedouble;
        if (std::isfinite(v) && v == std::floor(v) && v >= static_cast<double>(INT64_MIN) &&
            v < static_cast<double>(INT64_MAX)) {
            return static_cast<int64_t>(v);
        }
        ESP_LOGW(TAG, "Store: field '%s' is not an int64 (value=%f) — using 0", key, v);
        return 0;
    }
    return 0;
}

/// Strict parse of the type-tagged array; else WARN + false (absent).
bool DpValueFromJsonArray(const cJSON* node, DpValue& out) {
    if (!cJSON_IsArray(node) || cJSON_GetArraySize(node) != 2) {
        ESP_LOGW(TAG, "Store: DpValue entry is not a 2-element array — dropped");
        return false;
    }
    const cJSON* tag_node = node->child;
    const cJSON* val_node = tag_node != nullptr ? tag_node->next : nullptr;
    if (!cJSON_IsString(tag_node) || tag_node->valuestring == nullptr) {
        ESP_LOGW(TAG, "Store: DpValue tag is not a string — dropped");
        return false;
    }
    const char* tag = tag_node->valuestring;
    if (std::strcmp(tag, "b") == 0) {
        if (!cJSON_IsBool(val_node)) {
            ESP_LOGW(TAG, "Store: DpValue [\"b\",...] value not a bool — dropped");
            return false;
        }
        out = DpValue::FromBool(cJSON_IsTrue(val_node));
        return true;
    }
    if (std::strcmp(tag, "i") == 0) {
        if (!cJSON_IsNumber(val_node)) {
            ESP_LOGW(TAG, "Store: DpValue [\"i\",...] value not a number — dropped");
            return false;
        }
        const double v = val_node->valuedouble;
        if (!(std::isfinite(v) && v == std::floor(v) && v >= static_cast<double>(INT64_MIN) &&
              v < static_cast<double>(INT64_MAX))) {
            ESP_LOGW(TAG, "Store: DpValue [\"i\",...] value not an int64 — dropped");
            return false;
        }
        out = DpValue::FromInt(static_cast<int64_t>(v));
        return true;
    }
    if (std::strcmp(tag, "d") == 0) {
        if (!cJSON_IsNumber(val_node)) {
            ESP_LOGW(TAG, "Store: DpValue [\"d\",...] value not a number — dropped");
            return false;
        }
        // 1e400 parses as an infinity: it must never re-enter the registry.
        if (!std::isfinite(val_node->valuedouble)) {
            ESP_LOGW(TAG, "Store: DpValue [\"d\",...] value not finite — dropped");
            return false;
        }
        out = DpValue::FromDouble(val_node->valuedouble);
        return true;
    }
    if (std::strcmp(tag, "s") == 0) {
        if (!cJSON_IsString(val_node) || val_node->valuestring == nullptr) {
            ESP_LOGW(TAG, "Store: DpValue [\"s\",...] value not a string — dropped");
            return false;
        }
        out = DpValue::FromString(val_node->valuestring);
        return true;
    }
    ESP_LOGW(TAG, "Store: DpValue tag \"%s\" unknown — dropped", tag);
    return false;
}

/// DpValue -> type-tagged 2-element array; the tag stops "123" decaying to
/// int. Ints pass through cJSON's double semantics (DoubleCjson).
void WriteDpValue(tuya_crypto::JsonWriter& writer, const DpValue& value) {
    const char* tag = "s";
    switch (value.type) {
        case DpValue::Type::kBool: tag = "b"; break;
        case DpValue::Type::kInt: tag = "i"; break;
        case DpValue::Type::kDouble: tag = "d"; break;
        case DpValue::Type::kString: tag = "s"; break;
    }
    writer.BeginArray();
    writer.StringCjson(tag);
    switch (value.type) {
        case DpValue::Type::kBool: writer.Bool(value.b); break;
        case DpValue::Type::kInt:
            // int64 through cJSON's double: DP values are far below 2^53.
            writer.DoubleCjson(static_cast<double>(value.i));
            break;
        case DpValue::Type::kDouble: writer.DoubleCjson(value.d); break;
        case DpValue::Type::kString: writer.StringCjson(value.s); break;
    }
    writer.EndArray();
}

/// Streams one device object (field order pinned by the version-1 format).
void WriteDeviceJson(tuya_crypto::JsonWriter& writer, const Device& device) {
    writer.BeginObject();
    writer.KeyCjson("id");
    writer.StringCjson(device.id);
    writer.KeyCjson("name");
    writer.StringCjson(device.name);
    writer.KeyCjson("category");
    writer.StringCjson(device.category);
    writer.KeyCjson("product_id");
    writer.StringCjson(device.product_id);
    writer.KeyCjson("product_name");
    writer.StringCjson(device.product_name);
    writer.KeyCjson("model");
    writer.StringCjson(device.model);
    writer.KeyCjson("uuid");
    writer.StringCjson(device.uuid);
    writer.KeyCjson("asset_id");
    writer.StringCjson(device.asset_id);
    writer.KeyCjson("icon");
    writer.StringCjson(device.icon);
    writer.KeyCjson("time_zone");
    writer.StringCjson(device.time_zone);
    writer.KeyCjson("sub");
    writer.Bool(device.sub);
    writer.KeyCjson("online");
    writer.Bool(device.online);
    writer.KeyCjson("active_time");
    writer.DoubleCjson(static_cast<double>(device.active_time));
    writer.KeyCjson("create_time");
    writer.DoubleCjson(static_cast<double>(device.create_time));
    writer.KeyCjson("update_time");
    writer.DoubleCjson(static_cast<double>(device.update_time));
    writer.KeyCjson("home_id");
    writer.StringCjson(device.home_id);
    // Optional key of the version-1 format (a legacy snapshot has none).
    writer.KeyCjson("room_id");
    writer.StringCjson(device.room_id);
    writer.KeyCjson("support_local");
    writer.Bool(device.support_local);

    // status: {code: type-tagged arrays} — std::map key order (deterministic).
    writer.KeyCjson("status");
    writer.BeginObject();
    for (const auto& entry : device.status) {
        writer.KeyCjson(entry.first);
        WriteDpValue(writer, entry.second);
    }
    writer.EndObject();

    // function: {code: {name, desc, type, values}} — the map key IS the code.
    writer.KeyCjson("function");
    writer.BeginObject();
    for (const auto& entry : device.function) {
        const DeviceFunction& fn = entry.second;
        writer.KeyCjson(entry.first);
        writer.BeginObject();
        writer.KeyCjson("name");
        writer.StringCjson(fn.name);
        writer.KeyCjson("desc");
        writer.StringCjson(fn.desc);
        writer.KeyCjson("type");
        writer.StringCjson(fn.type);
        writer.KeyCjson("values");
        writer.StringCjson(fn.values_json);
        writer.EndObject();
    }
    writer.EndObject();

    // status_range: {code: {name, type, values, report_type}}.
    writer.KeyCjson("status_range");
    writer.BeginObject();
    for (const auto& entry : device.status_range) {
        const DeviceStatusRange& range = entry.second;
        writer.KeyCjson(entry.first);
        writer.BeginObject();
        writer.KeyCjson("name");
        writer.StringCjson(range.name);
        writer.KeyCjson("type");
        writer.StringCjson(range.type);
        writer.KeyCjson("values");
        writer.StringCjson(range.values_json);
        writer.KeyCjson("report_type");
        writer.StringCjson(range.report_type);
        writer.EndObject();
    }
    writer.EndObject();

    // local_strategy: {"<dpId>": {...}} — dpId as decimal STRING key.
    writer.KeyCjson("local_strategy");
    writer.BeginObject();
    for (const auto& entry : device.local_strategy) {
        const StrategyConfig& cfg = entry.second;
        writer.KeyCjson(std::to_string(entry.first));
        writer.BeginObject();
        writer.KeyCjson("support_local");
        writer.Bool(cfg.support_local);
        writer.KeyCjson("value_convert");
        writer.StringCjson(cfg.value_convert);
        writer.KeyCjson("status_code");
        writer.StringCjson(cfg.status_code);
        writer.KeyCjson("status_format_json");
        writer.StringCjson(cfg.status_format_json);
        writer.KeyCjson("value_type");
        writer.StringCjson(cfg.value_type);
        writer.KeyCjson("value_desc_json");
        writer.StringCjson(cfg.value_desc_json);
        writer.KeyCjson("enum_mapping");
        writer.BeginObject();
        for (const auto& mapping : cfg.enum_mapping) {
            writer.KeyCjson(mapping.first);
            writer.StringCjson(mapping.second);
        }
        writer.EndObject();
        writer.KeyCjson("use_open_api");
        writer.Bool(cfg.use_open_api);
        writer.KeyCjson("property_update");
        writer.Bool(cfg.property_update);
        writer.EndObject();
    }
    writer.EndObject();

    // disabled_entities: [code, ...] — std::set order (sorted).
    writer.KeyCjson("disabled_entities");
    writer.BeginArray();
    for (const auto& code : device.disabled_entities) {
        writer.StringCjson(code);
    }
    writer.EndArray();

    // dp_timestamps / set_up are deliberately NOT emitted (see device_store.h).
    writer.EndObject();
}

}  // namespace

bool DeviceFromJson(const cJSON* node, Device& out) {
    if (!cJSON_IsObject(node)) {
        return false;
    }
    Device device;
    device.id = tuya_crypto::GetStringField(node, "id");
    if (device.id.empty()) {
        return false;
    }
    device.name = tuya_crypto::GetStringField(node, "name");
    device.category = tuya_crypto::GetStringField(node, "category");
    device.product_id = tuya_crypto::GetStringField(node, "product_id");
    device.product_name = tuya_crypto::GetStringField(node, "product_name");
    device.model = tuya_crypto::GetStringField(node, "model");
    device.uuid = tuya_crypto::GetStringField(node, "uuid");
    device.asset_id = tuya_crypto::GetStringField(node, "asset_id");
    device.icon = tuya_crypto::GetStringField(node, "icon");
    device.time_zone = tuya_crypto::GetStringField(node, "time_zone");
    device.sub = tuya_crypto::GetBoolField(node, "sub");
    device.online = tuya_crypto::GetBoolField(node, "online");
    device.active_time = GetValidatedInt64(node, "active_time");
    device.create_time = GetValidatedInt64(node, "create_time");
    device.update_time = GetValidatedInt64(node, "update_time");
    device.home_id = tuya_crypto::GetStringField(node, "home_id");
    device.room_id = tuya_crypto::GetStringField(node, "room_id");  // "" when absent
    device.support_local = tuya_crypto::GetBoolField(node, "support_local");

    // status (strict: type-tagged 2-element arrays, anything else dropped)
    const cJSON* status = cJSON_GetObjectItemCaseSensitive(node, "status");
    if (cJSON_IsObject(status)) {
        for (const cJSON* item = status->child; item != nullptr; item = item->next) {
            if (item->string == nullptr || item->string[0] == '\0') {
                continue;
            }
            DpValue value;
            if (!DpValueFromJsonArray(item, value)) {
                continue;  // tolerated anomaly — field stays absent
            }
            device.status[item->string] = std::move(value);
        }
    }

    // function {code: {name, desc, type, values}}
    const cJSON* functions = cJSON_GetObjectItemCaseSensitive(node, "function");
    if (cJSON_IsObject(functions)) {
        for (const cJSON* item = functions->child; item != nullptr; item = item->next) {
            if (item->string == nullptr || item->string[0] == '\0') {
                continue;
            }
            if (!cJSON_IsObject(item)) {
                ESP_LOGW(TAG, "Store: function entry \"%s\" is not an object — dropped",
                         item->string);
                continue;
            }
            DeviceFunction fn;
            fn.code = item->string;  // the map key IS the code
            fn.name = tuya_crypto::GetStringField(item, "name");
            fn.desc = tuya_crypto::GetStringField(item, "desc");
            fn.type = tuya_crypto::GetStringField(item, "type");
            fn.values_json = tuya_crypto::GetStringField(item, "values");
            device.function[item->string] = std::move(fn);
        }
    }

    // status_range {code: {name, type, values, report_type}}
    const cJSON* ranges = cJSON_GetObjectItemCaseSensitive(node, "status_range");
    if (cJSON_IsObject(ranges)) {
        for (const cJSON* item = ranges->child; item != nullptr; item = item->next) {
            if (item->string == nullptr || item->string[0] == '\0') {
                continue;
            }
            if (!cJSON_IsObject(item)) {
                ESP_LOGW(TAG, "Store: status_range entry \"%s\" is not an object — dropped",
                         item->string);
                continue;
            }
            DeviceStatusRange range;
            range.code = item->string;  // the map key IS the code
            range.name = tuya_crypto::GetStringField(item, "name");
            range.type = tuya_crypto::GetStringField(item, "type");
            range.values_json = tuya_crypto::GetStringField(item, "values");
            range.report_type = tuya_crypto::GetStringField(item, "report_type");
            device.status_range[item->string] = std::move(range);
        }
    }

    // local_strategy {"<dpId>": {...}} — the key must be a decimal int.
    const cJSON* strategy = cJSON_GetObjectItemCaseSensitive(node, "local_strategy");
    if (cJSON_IsObject(strategy)) {
        for (const cJSON* item = strategy->child; item != nullptr; item = item->next) {
            if (item->string == nullptr || item->string[0] == '\0') {
                continue;
            }
            int dp_id = 0;
            const char* end = item->string + std::strlen(item->string);
            const auto result = std::from_chars(item->string, end, dp_id, /*base=*/10);
            if (result.ec != std::errc() || result.ptr != end) {
                ESP_LOGW(TAG, "Store: local_strategy key \"%s\" is not a decimal dpId — dropped",
                         item->string);
                continue;
            }
            if (!cJSON_IsObject(item)) {
                ESP_LOGW(TAG, "Store: local_strategy entry \"%s\" is not an object — dropped",
                         item->string);
                continue;
            }
            StrategyConfig cfg;
            cfg.dp_id = dp_id;  // the map key IS the dp_id
            cfg.support_local = tuya_crypto::GetBoolField(item, "support_local");
            cfg.value_convert = tuya_crypto::GetStringField(item, "value_convert");
            cfg.status_code = tuya_crypto::GetStringField(item, "status_code");
            cfg.status_format_json = tuya_crypto::GetStringField(item, "status_format_json");
            cfg.value_type = tuya_crypto::GetStringField(item, "value_type");
            cfg.value_desc_json = tuya_crypto::GetStringField(item, "value_desc_json");
            const cJSON* enum_map = cJSON_GetObjectItemCaseSensitive(item, "enum_mapping");
            if (cJSON_IsObject(enum_map)) {
                for (const cJSON* entry = enum_map->child; entry != nullptr; entry = entry->next) {
                    if (entry->string == nullptr || entry->string[0] == '\0') {
                        continue;
                    }
                    if (!cJSON_IsString(entry) || entry->valuestring == nullptr) {
                        ESP_LOGW(TAG, "Store: enum_mapping entry \"%s\" is not a string — dropped",
                                 entry->string);
                        continue;
                    }
                    cfg.enum_mapping[entry->string] = entry->valuestring;
                }
            }
            cfg.use_open_api = tuya_crypto::GetBoolField(item, "use_open_api");
            cfg.property_update = tuya_crypto::GetBoolField(item, "property_update");
            device.local_strategy[dp_id] = std::move(cfg);
        }
    }

    // disabled_entities [code, ...]
    const cJSON* disabled = cJSON_GetObjectItemCaseSensitive(node, "disabled_entities");
    if (cJSON_IsArray(disabled)) {
        for (const cJSON* item = disabled->child; item != nullptr; item = item->next) {
            if (!cJSON_IsString(item) || item->valuestring == nullptr ||
                item->valuestring[0] == '\0') {
                ESP_LOGW(TAG, "Store: disabled_entities entry is not a non-empty string — dropped");
                continue;
            }
            device.disabled_entities.insert(item->valuestring);
        }
    }

    out = std::move(device);
    return true;
}

std::string BuildStoreJson(const std::vector<Home>& homes, const std::vector<Device>& devices,
                           const std::string& uid, int64_t saved_at_unix) {
    return BuildStoreJsonStreamed(
        homes, uid, saved_at_unix,
        [&devices](const DeviceEmit& emit) {
            for (const Device& device : devices) {
                emit(device);
            }
        },
        nullptr);
}

std::string BuildStoreJsonStreamed(const std::vector<Home>& homes, const std::string& uid,
                                   int64_t saved_at_unix, const DeviceSource& device_source,
                                   size_t* device_count) {
    // The writer's string grows in PSRAM (allocations >= 500 bytes prefer it).
    tuya_crypto::JsonWriter writer;
    size_t emitted = 0;
    writer.BeginObject();
    writer.KeyCjson("version");
    writer.DoubleCjson(1);
    writer.KeyCjson("uid");
    writer.StringCjson(uid);
    writer.KeyCjson("saved_at");
    writer.DoubleCjson(static_cast<double>(saved_at_unix));

    writer.KeyCjson("homes");
    writer.BeginArray();
    for (const Home& home : homes) {
        writer.BeginObject();
        writer.KeyCjson("id");
        writer.StringCjson(home.id);
        writer.KeyCjson("name");
        writer.StringCjson(home.name);
        // Optional key: emitted only when the rooms were fetched this boot.
        if (!home.rooms.empty()) {
            writer.KeyCjson("rooms");
            writer.BeginArray();
            for (const Room& room : home.rooms) {
                writer.BeginObject();
                writer.KeyCjson("id");
                writer.StringCjson(room.id);
                writer.KeyCjson("name");
                writer.StringCjson(room.name);
                writer.KeyCjson("sort");
                writer.DoubleCjson(static_cast<double>(room.sort));
                writer.EndObject();
            }
            writer.EndArray();
        }
        writer.EndObject();
    }
    writer.EndArray();

    writer.KeyCjson("devices");
    writer.BeginArray();
    if (device_source) {
        device_source([&writer, &emitted](const Device& device) {
            ++emitted;
            WriteDeviceJson(writer, device);
        });
    }
    writer.EndArray();
    writer.EndObject();

    if (device_count != nullptr) {
        *device_count = emitted;
    }
    if (writer.HasError()) {
        return "";
    }
    return writer.TakeStr();
}

bool ParseStoreJson(const std::string& text, std::vector<Home>& homes, std::vector<Device>& devices,
                    std::string& stored_uid) {
    homes.clear();
    devices.clear();
    stored_uid.clear();

    cJSON* root = cJSON_ParseWithLength(text.c_str(), text.size());
    if (root == nullptr) {
        ESP_LOGW(TAG, "Store: registry.json is not valid JSON — ignored");
        return false;
    }
    if (!cJSON_IsObject(root)) {
        cJSON_Delete(root);
        ESP_LOGW(TAG, "Store: registry.json root is not an object — ignored");
        return false;
    }
    const cJSON* version = cJSON_GetObjectItemCaseSensitive(root, "version");
    if (!cJSON_IsNumber(version) || version->valuedouble != 1.0) {
        cJSON_Delete(root);
        // Keep a newer-format snapshot from being overwritten by the next save
        // (never a second .bad: the partition budget assumes one).
        const bool moved = MoveAsideBadSlot() == 0;
        ESP_LOGW(TAG, "Store: version unsupported or missing (expected 1) — ignored (file %s)",
                 moved ? "moved aside to .bad" : "left in place");
        return false;
    }
    stored_uid = tuya_crypto::GetStringField(root, "uid");
    // "saved_at" is informational only — not parsed (forward compatible).

    const cJSON* homes_node = cJSON_GetObjectItemCaseSensitive(root, "homes");
    if (cJSON_IsArray(homes_node)) {
        for (const cJSON* item = homes_node->child; item != nullptr; item = item->next) {
            if (!cJSON_IsObject(item)) {
                ESP_LOGW(TAG, "Store: home entry is not an object — skipped");
                continue;
            }
            Home home;
            home.id = tuya_crypto::GetStringField(item, "id");
            if (home.id.empty()) {
                ESP_LOGW(TAG, "Store: home entry without an id — skipped");
                continue;
            }
            home.name = tuya_crypto::GetStringField(item, "name");
            // Optional room list (a legacy snapshot has none -> empty = unknown).
            const cJSON* rooms_node = cJSON_GetObjectItemCaseSensitive(item, "rooms");
            if (cJSON_IsArray(rooms_node)) {
                for (const cJSON* entry = rooms_node->child; entry != nullptr;
                     entry = entry->next) {
                    if (!cJSON_IsObject(entry)) {
                        continue;
                    }
                    Room room;
                    room.id = tuya_crypto::GetStringField(entry, "id");
                    if (room.id.empty()) {
                        continue;  // an id-less row cannot be matched
                    }
                    room.name = tuya_crypto::GetStringField(entry, "name");
                    const int64_t sort = GetValidatedInt64(entry, "sort");
                    room.sort = sort >= INT_MIN && sort <= INT_MAX ? static_cast<int>(sort) : 0;
                    home.rooms.push_back(std::move(room));
                }
            }
            homes.push_back(std::move(home));
        }
    }

    const cJSON* devices_node = cJSON_GetObjectItemCaseSensitive(root, "devices");
    if (cJSON_IsArray(devices_node)) {
        for (const cJSON* item = devices_node->child; item != nullptr; item = item->next) {
            Device device;
            if (!DeviceFromJson(item, device)) {
                ESP_LOGW(TAG, "Store: device entry unusable (no id / not an object) — skipped");
                continue;
            }
            devices.push_back(std::move(device));
        }
    }

    cJSON_Delete(root);
    return true;
}

}  // namespace hub_core
