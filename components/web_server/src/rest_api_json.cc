// JSON object builders shared by the /api handlers and the WS events.

#include "rest_api_internal.h"

#include <cmath>
#include <cstdint>
#include <cstring>
#include <functional>
#include <map>
#include <string>
#include <string_view>
#include <vector>

#include "hub_core/device_registry.h"
#include "hub_services/command_service.h"
#include "hub_services/discovery.h"
#include "sys/cpu_monitor.h"
#include "sys/settings.h"
#include "sys/system_info.h"
#include "sys/wifi_station.h"
#include "tuya_cloud/cloud_auth.h"
#include "tuya_mqtt/tuya_mqtt_client.h"
#include "web_server/api_auth.h"

namespace web_server {
namespace rest_api {

namespace {

const char* DeviceStateName(DeviceState state) {
    switch (state) {
        case kDeviceStateStarting: return "starting";
        case kDeviceStateWifiConfig: return "wifi_config";
        case kDeviceStateNeedsSetup: return "needs_setup";
        case kDeviceStateIdle: return "idle";
        case kDeviceStateCloudConnecting: return "cloud_connecting";
        case kDeviceStateLinked: return "linked";
        case kDeviceStateFatalError: return "fatal_error";
        default: return "unknown";
    }
}

const char* AuthStateName(tuya_cloud::AuthState state) {
    switch (state) {
        case tuya_cloud::AuthState::kIdle: return "idle";
        case tuya_cloud::AuthState::kWaitingScan: return "waiting_scan";
        case tuya_cloud::AuthState::kLinked: return "linked";
        case tuya_cloud::AuthState::kFailed: return "failed";
        default: return "unknown";
    }
}

/// DpValue -> cJSON; int64 via double (DP values stay well below 2^53).
cJSON* DpValueToJson(const hub_core::DpValue& value) {
    switch (value.type) {
        case hub_core::DpValue::Type::kBool: return cJSON_CreateBool(value.b);
        case hub_core::DpValue::Type::kInt: return cJSON_CreateNumber(static_cast<double>(value.i));
        case hub_core::DpValue::Type::kDouble: return cJSON_CreateNumber(value.d);
        case hub_core::DpValue::Type::kString: return cJSON_CreateString(value.s.c_str());
    }
    return nullptr;
}

}  // namespace

const char* DefaultLanguage() {
#if defined(CONFIG_HUB_LANG_EN)
    return "en";
#else
    return "vi";
#endif
}

void AttachOwned(cJSON* parent, const char* key, cJSON* child) {
    if (child == nullptr) {
        return;
    }
    if (cJSON_AddItemToObject(parent, key, child) == 0) {
        cJSON_Delete(child);
    }
}

void AddOwnedToArray(cJSON* array, cJSON* child) {
    if (child == nullptr) {
        return;
    }
    if (cJSON_AddItemToArray(array, child) == 0) {
        cJSON_Delete(child);
    }
}

bool DpValueFromJson(const cJSON* node, hub_core::DpValue& out) {
    if (cJSON_IsBool(node)) {
        out = hub_core::DpValue::FromBool(cJSON_IsTrue(node));
        return true;
    }
    if (cJSON_IsNumber(node)) {
        const double value = node->valuedouble;
        // cJSON turns 1e400 into inf; the writer would then emit invalid JSON.
        if (!std::isfinite(value)) {
            return false;
        }
        if (value == std::floor(value) && value >= static_cast<double>(INT64_MIN) &&
            value < static_cast<double>(INT64_MAX)) {
            out = hub_core::DpValue::FromInt(static_cast<int64_t>(value));
        } else {
            out = hub_core::DpValue::FromDouble(value);
        }
        return true;
    }
    if (cJSON_IsString(node) && node->valuestring != nullptr) {
        out = hub_core::DpValue::FromString(node->valuestring);
        return true;
    }
    return false;  // null / object / array / absent
}

// ---- Object builders (rest_api.h contract) ----

cJSON* BuildWifiObject() {
    sys::WifiStation& station = sys::WifiStation::GetInstance();
    cJSON* wifi = cJSON_CreateObject();
    if (wifi == nullptr) {
        return nullptr;
    }
    cJSON_AddBoolToObject(wifi, "connected", station.IsConnected());
    cJSON_AddStringToObject(wifi, "ssid", station.GetSsid().c_str());
    cJSON_AddStringToObject(wifi, "ip", station.GetIp().c_str());
    cJSON_AddNumberToObject(wifi, "rssi", static_cast<double>(station.GetRssi()));
    return wifi;
}

cJSON* BuildHeapObject() {
    cJSON* heap = cJSON_CreateObject();
    if (heap == nullptr) {
        return nullptr;
    }
    cJSON_AddNumberToObject(heap, "internal_free",
                            static_cast<double>(sys::SystemInfo::GetFreeHeapInternal()));
    cJSON_AddNumberToObject(heap, "psram_free",
                            static_cast<double>(sys::SystemInfo::GetFreeHeapPsram()));
    cJSON_AddNumberToObject(heap, "internal_min_free",
                            static_cast<double>(sys::SystemInfo::GetMinFreeHeapInternal()));
    // Totals/largest blocks for the RAM/PSRAM meters.
    cJSON_AddNumberToObject(heap, "internal_total",
                            static_cast<double>(sys::SystemInfo::GetTotalHeapInternal()));
    cJSON_AddNumberToObject(heap, "internal_largest",
                            static_cast<double>(sys::SystemInfo::GetLargestFreeHeapInternal()));
    cJSON_AddNumberToObject(heap, "psram_total",
                            static_cast<double>(sys::SystemInfo::GetTotalHeapPsram()));
    cJSON_AddNumberToObject(heap, "psram_largest",
                            static_cast<double>(sys::SystemInfo::GetLargestFreeHeapPsram()));
    return heap;
}

cJSON* BuildCpuObject() {
    const sys::CpuMonitor::Snapshot snapshot = sys::CpuMonitor::Get();
    if (!snapshot.valid) {
        return nullptr;
    }
    cJSON* cpu = cJSON_CreateObject();
    if (cpu == nullptr) {
        return nullptr;
    }
    cJSON_AddNumberToObject(cpu, "usage", snapshot.usage_pct);
    cJSON_AddNumberToObject(cpu, "core0", snapshot.core0_pct);
    cJSON_AddNumberToObject(cpu, "core1", snapshot.core1_pct);
    return cpu;
}

cJSON* BuildAuthSummaryObject() {
    const tuya_cloud::AuthStatus status = tuya_cloud::CloudAuthManager::GetInstance().GetStatus();
    cJSON* auth = cJSON_CreateObject();
    if (auth == nullptr) {
        return nullptr;
    }
    cJSON_AddStringToObject(auth, "state", AuthStateName(status.state));
    cJSON_AddStringToObject(auth, "username", status.username.c_str());
    cJSON_AddStringToObject(auth, "qr_payload", status.qr_payload.c_str());
    // True while the un-rotated initial key is the effective key — the
    // login gate forces a key change until it flips.
    cJSON_AddBoolToObject(auth, "initial", ApiKeyIsInitial());
    return auth;
}

cJSON* BuildDiscoverySummaryObject() {
    hub_core::DiscoveryService& service = hub_core::DiscoveryService::GetInstance();
    const hub_core::DiscoveryProgress progress = service.GetProgress();
    cJSON* discovery = cJSON_CreateObject();
    if (discovery == nullptr) {
        return nullptr;
    }
    cJSON_AddBoolToObject(discovery, "active", service.IsRunning());
    cJSON_AddNumberToObject(discovery, "percent", progress.percent);
    cJSON_AddBoolToObject(discovery, "done", progress.done);
    cJSON_AddBoolToObject(discovery, "failed", progress.failed);
    // Staged candidate count (0 while a scan runs — staging is cleared at Start).
    cJSON_AddNumberToObject(discovery, "candidates",
                            static_cast<double>(service.GetStagingCandidateCount()));
    return discovery;
}

namespace {

/// Room id -> name for one home ("" when the home has no room data yet).
std::string RoomNameFor(const std::vector<hub_core::Home>& homes,
                        const hub_core::DeviceString& home_id,
                        const hub_core::DeviceString& room_id) {
    if (room_id.empty()) {
        return "";
    }
    for (const hub_core::Home& home : homes) {
        if (std::string_view(home.id) != std::string_view(home_id)) {
            continue;
        }
        for (const hub_core::Room& room : home.rooms) {
            if (std::string_view(room.id) == std::string_view(room_id)) {
                return std::string(room.name);
            }
        }
    }
    return "";
}

}  // namespace

cJSON* BuildDeviceObject(const hub_core::Device& device, const RoomNameResolver& room_names) {
    cJSON* item = cJSON_CreateObject();
    if (item == nullptr) {
        return nullptr;
    }
    cJSON_AddStringToObject(item, "id", device.id.c_str());
    cJSON_AddStringToObject(item, "name", device.name.c_str());
    cJSON_AddStringToObject(item, "category", device.category.c_str());
    cJSON_AddBoolToObject(item, "online", device.online);
    cJSON_AddBoolToObject(item, "support_local", device.support_local);
    cJSON_AddStringToObject(item, "home_id", device.home_id.c_str());
    // Top-level keys (never DP codes): the room name is resolved, never stored.
    cJSON_AddStringToObject(item, "room_id", device.room_id.c_str());
    const std::string room_name =
        room_names ? room_names(device.home_id, device.room_id) : std::string();
    cJSON_AddStringToObject(item, "room_name", room_name.c_str());

    cJSON* status = cJSON_AddObjectToObject(item, "status");
    if (status != nullptr) {
        for (const auto& entry : device.status) {
            if (device.disabled_entities.count(entry.first) > 0) {
                continue;  // disabled entities are invisible to /api/devices
            }
            AttachOwned(status, entry.first.c_str(), DpValueToJson(entry.second));
        }
    }
    cJSON* ranges = cJSON_AddObjectToObject(item, "status_range");
    if (ranges != nullptr) {
        auto add_range = [&ranges](const char* code, const char* type, const char* values,
                                   const char* name, bool writable) {
            cJSON* range = cJSON_CreateObject();
            if (range == nullptr) {
                return;  // OOM — skip this entity
            }
            cJSON_AddStringToObject(range, "type", type);
            // The RAW server JSON string ({"range":[...]} / {"min":...});
            // the UI parses it.
            cJSON_AddStringToObject(range, "values", values);
            // Model names may be empty; the code is the display fallback.
            cJSON_AddStringToObject(range, "name", name[0] != '\0' ? name : code);
            cJSON_AddBoolToObject(range, "writable", writable);
            AttachOwned(ranges, code, range);
        };
        for (const auto& entry : device.status_range) {
            if (device.disabled_entities.count(entry.first) > 0) {
                continue;
            }
            const auto fn = device.function.find(entry.first);
            const char* name = entry.second.name.c_str();
            if (name[0] == '\0' && fn != device.function.end()) {
                name = fn->second.name.c_str();
            }
            add_range(entry.first.c_str(), entry.second.type.c_str(),
                      entry.second.values_json.c_str(), name, fn != device.function.end());
        }
        // Function-only DPs (no status_range row) still get an entry.
        for (const auto& entry : device.function) {
            if (device.status_range.count(entry.first) > 0 ||
                device.disabled_entities.count(entry.first) > 0) {
                continue;
            }
            add_range(entry.first.c_str(), entry.second.type.c_str(),
                      entry.second.values_json.c_str(), entry.second.name.c_str(), true);
        }
    }
    return item;
}

namespace {

/// Entity rows with an explicit enable set (registry-backed candidate rows).
cJSON* BuildEntityRowsWithDisabled(const hub_core::Device& device,
                                   const hub_core::DeviceCodeSet& disabled) {
    // Merge scratch: one row per code; pointers reference stable map nodes.
    struct RowInfo {
        const hub_core::DeviceString* name = nullptr;  // first non-empty wins
        const hub_core::DeviceString* type = nullptr;  // first non-empty wins
        const hub_core::DpValue* value = nullptr;      // only when status has the code
        bool writable = false;                         // code exists in function
    };
    std::map<hub_core::DeviceString, RowInfo> rows;
    for (const auto& entry : device.function) {
        RowInfo& row = rows[entry.first];
        row.writable = true;
        if (!entry.second.name.empty() && row.name == nullptr) {
            row.name = &entry.second.name;
        }
        if (!entry.second.type.empty() && row.type == nullptr) {
            row.type = &entry.second.type;
        }
    }
    for (const auto& entry : device.status_range) {
        RowInfo& row = rows[entry.first];
        if (!entry.second.name.empty() && row.name == nullptr) {
            row.name = &entry.second.name;
        }
        if (!entry.second.type.empty() && row.type == nullptr) {
            row.type = &entry.second.type;
        }
    }
    for (const auto& entry : device.status) {
        rows[entry.first].value = &entry.second;
    }

    cJSON* array = cJSON_CreateArray();
    if (array == nullptr) {
        return nullptr;
    }
    for (const auto& entry : rows) {
        const hub_core::DeviceString& code = entry.first;
        const RowInfo& info = entry.second;
        cJSON* row = cJSON_CreateObject();
        if (row == nullptr) {
            continue;  // OOM — skip this entity
        }
        cJSON_AddStringToObject(row, "code", code.c_str());
        cJSON_AddStringToObject(row, "name", info.name != nullptr ? info.name->c_str() : "");
        cJSON_AddStringToObject(row, "type", info.type != nullptr ? info.type->c_str() : "");
        cJSON_AddBoolToObject(row, "writable", info.writable);
        if (info.value != nullptr) {
            AttachOwned(row, "value", DpValueToJson(*info.value));
        }
        cJSON_AddBoolToObject(row, "enabled", disabled.count(code) == 0);
        AddOwnedToArray(array, row);
    }
    return array;
}

}  // namespace

cJSON* BuildEntityRows(const hub_core::Device& device) {
    return BuildEntityRowsWithDisabled(device, device.disabled_entities);
}

hub_core::DeviceCodeSet CollectEntityCodes(const hub_core::Device& device) {
    hub_core::DeviceCodeSet codes;
    for (const auto& entry : device.status) {
        codes.insert(entry.first);
    }
    for (const auto& entry : device.status_range) {
        codes.insert(entry.first);
    }
    for (const auto& entry : device.function) {
        codes.insert(entry.first);
    }
    return codes;
}

void CollectEntityRowStrings(const hub_core::Device& device, std::vector<std::string>& out) {
    cJSON* rows = BuildEntityRows(device);
    if (rows == nullptr) {
        return;
    }
    for (const cJSON* row = rows->child; row != nullptr; row = row->next) {
        char* text = cJSON_PrintUnformatted(row);
        if (text == nullptr) {
            continue;  // OOM — skip this row
        }
        out.emplace_back(text);
        cJSON_free(text);
    }
    cJSON_Delete(rows);
}

esp_err_t StreamItemArray(httpd_req_t* req, const char* prefix,
                          const std::vector<std::string>& items) {
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    esp_err_t ret = httpd_resp_send_chunk(req, prefix, strlen(prefix));
    bool first = true;
    for (const std::string& item : items) {
        if (ret != ESP_OK) {
            break;
        }
        if (!first) {
            ret = httpd_resp_send_chunk(req, ",", 1);
            if (ret != ESP_OK) {
                break;
            }
        }
        first = false;
        ret = httpd_resp_send_chunk(req, item.data(), static_cast<ssize_t>(item.size()));
    }
    if (ret == ESP_OK) {
        ret = httpd_resp_send_chunk(req, "]", 1);
    }
    if (ret == ESP_OK) {
        ret = httpd_resp_send_chunk(req, "}", 1);
    }
    if (ret == ESP_OK) {
        ret = httpd_resp_send_chunk(req, nullptr, 0);  // terminating chunk
    }
    return ret;
}

std::function<std::string(const hub_core::DeviceString&, const hub_core::DeviceString&)>
MakeRoomNameResolver() {
    const std::vector<hub_core::Home> homes = hub_core::DeviceRegistry::GetInstance().GetHomes();
    return [homes](const hub_core::DeviceString& home_id, const hub_core::DeviceString& room_id) {
        return RoomNameFor(homes, home_id, room_id);
    };
}

cJSON* BuildCandidateObject(const hub_core::Device& candidate, bool detail_loaded) {
    cJSON* item = cJSON_CreateObject();
    if (item == nullptr) {
        return nullptr;
    }
    cJSON_AddStringToObject(item, "id", candidate.id.c_str());
    cJSON_AddStringToObject(item, "name", candidate.name.c_str());
    cJSON_AddStringToObject(item, "category", candidate.category.c_str());
    cJSON_AddStringToObject(item, "product_name", candidate.product_name.c_str());
    cJSON_AddBoolToObject(item, "online", candidate.online);
    cJSON_AddStringToObject(item, "home_id", candidate.home_id.c_str());
    cJSON_AddStringToObject(item, "room_id", candidate.room_id.c_str());
    const RoomNameResolver resolve = MakeRoomNameResolver();
    const std::string room_name = resolve(candidate.home_id, candidate.room_id);
    cJSON_AddStringToObject(item, "room_name", room_name.c_str());
    hub_core::Device existing;
    const bool already_added =
        hub_core::DeviceRegistry::GetInstance().GetDevice(candidate.id, existing);
    cJSON_AddBoolToObject(item, "already_added", already_added);
    // False until the per-device model was lazily fetched (expand/commit).
    cJSON_AddBoolToObject(item, "detail_loaded", detail_loaded);
    // An added device reflects the registry's enable choices (staging has none).
    AttachOwned(item, "entities",
                already_added ? BuildEntityRowsWithDisabled(candidate, existing.disabled_entities)
                              : BuildEntityRows(candidate));
    return item;
}

// ---- WS-shared builders (rest_api.h contract) ----

cJSON* BuildStatusObject() {
    cJSON* root = cJSON_CreateObject();
    if (root == nullptr) {
        return nullptr;
    }
    cJSON_AddStringToObject(root, "version", sys::SystemInfo::GetAppVersion().c_str());
    {
        sys::Settings settings("hub");
        cJSON_AddStringToObject(root, "language",
                                settings.GetString("lang", DefaultLanguage()).c_str());
    }
    cJSON_AddStringToObject(
        root, "state",
        DeviceStateName(static_cast<DeviceState>(g_device_state.load(std::memory_order_relaxed))));
    AttachOwned(root, "wifi", BuildWifiObject());
    AttachOwned(root, "heap", BuildHeapObject());
    // CPU snapshot (omitted until the monitor's first valid sample).
    AttachOwned(root, "cpu", BuildCpuObject());
    cJSON_AddNumberToObject(root, "uptime_ms", static_cast<double>(sys::SystemInfo::GetUptimeMs()));
    AttachOwned(root, "auth", BuildAuthSummaryObject());
    AttachOwned(root, "mqtt",
                BuildMqttStatusObject(tuya_mqtt::TuyaMqttClient::GetInstance().GetStatus()));
    AttachOwned(root, "discovery", BuildDiscoverySummaryObject());
    cJSON_AddNumberToObject(
        root, "home_count",
        static_cast<double>(hub_core::DeviceRegistry::GetInstance().GetHomes().size()));
    cJSON_AddNumberToObject(root, "device_count",
                            static_cast<double>(hub_core::DeviceRegistry::GetInstance().Count()));
    cJSON_AddNumberToObject(
        root, "scene_count",
        static_cast<double>(hub_core::CommandService::GetInstance().GetScenes().size()));
    return root;
}

cJSON* BuildAuthStatusObject(const tuya_cloud::AuthStatus& status) {
    cJSON* root = cJSON_CreateObject();
    if (root == nullptr) {
        return nullptr;
    }
    cJSON_AddStringToObject(root, "state", AuthStateName(status.state));
    cJSON_AddStringToObject(root, "user_code", status.user_code.c_str());
    cJSON_AddStringToObject(root, "qr_payload", status.qr_payload.c_str());
    cJSON_AddStringToObject(root, "username", status.username.c_str());
    cJSON_AddStringToObject(root, "error", status.error.c_str());
    return root;
}

cJSON* BuildDiscoveryProgressObject(const hub_core::DiscoveryProgress& progress) {
    cJSON* root = cJSON_CreateObject();
    if (root == nullptr) {
        return nullptr;
    }
    // "active" derives from done/failed here; /api/status uses IsRunning().
    cJSON_AddBoolToObject(root, "active", !progress.done && !progress.failed);
    cJSON_AddNumberToObject(root, "percent", progress.percent);
    cJSON_AddBoolToObject(root, "done", progress.done);
    cJSON_AddBoolToObject(root, "failed", progress.failed);
    cJSON_AddNumberToObject(root, "homes_done", progress.homes_done);
    cJSON_AddNumberToObject(root, "homes_total", progress.homes_total);
    cJSON_AddNumberToObject(root, "devices_done", progress.devices_done);
    cJSON_AddNumberToObject(root, "devices_total", progress.devices_total);
    cJSON_AddStringToObject(root, "error", progress.error.c_str());
    return root;
}

cJSON* BuildMqttStatusObject(const tuya_mqtt::MqttStatus& status) {
    cJSON* root = cJSON_CreateObject();
    if (root == nullptr) {
        return nullptr;
    }
    cJSON_AddBoolToObject(root, "connected", status.connected);
    cJSON_AddStringToObject(root, "error", status.error.c_str());
    return root;
}

cJSON* BuildRegistryEventData(const hub_core::RegistryEventPayload& event) {
    cJSON* root = cJSON_CreateObject();
    if (root == nullptr) {
        return nullptr;
    }
    if (!event.dev_id.empty()) {
        cJSON_AddStringToObject(root, "devId", event.dev_id.c_str());
    }
    if (!event.home_id.empty()) {
        cJSON_AddStringToObject(root, "home_id", event.home_id.c_str());
    }
    switch (event.event) {
        case hub_core::RegistryEvent::kDeviceAdded:
        case hub_core::RegistryEvent::kDeviceRemoved: break;  // {devId} is the whole payload
        case hub_core::RegistryEvent::kDeviceOnlineChanged: {
            hub_core::Device device;
            if (hub_core::DeviceRegistry::GetInstance().GetDevice(event.dev_id, device)) {
                cJSON_AddBoolToObject(root, "online", device.online);
            }
            break;
        }
        case hub_core::RegistryEvent::kDeviceUpdated: {
            if (event.changed_codes.empty()) {
                // Model/rename refresh: {devId} only, byte-identical to device_added
                // (the UI refetches /api/devices on that event).
                break;
            }
            hub_core::Device device;
            if (hub_core::DeviceRegistry::GetInstance().GetDevice(event.dev_id, device)) {
                cJSON_AddStringToObject(root, "name", device.name.c_str());
                // props: the CURRENT values of the changed codes with
                // their report timestamps (t = 0 when never reported).
                cJSON* props = cJSON_AddArrayToObject(root, "props");
                if (props != nullptr) {
                    for (const std::string& code : event.changed_codes) {
                        const auto value_it = device.status.find(code);
                        if (value_it == device.status.end()) {
                            continue;
                        }
                        cJSON* prop = cJSON_CreateObject();
                        if (prop == nullptr) {
                            continue;
                        }
                        cJSON_AddStringToObject(prop, "code", code.c_str());
                        AttachOwned(prop, "value", DpValueToJson(value_it->second));
                        int64_t t = 0;
                        const auto ts_it = device.dp_timestamps.find(code);
                        if (ts_it != device.dp_timestamps.end()) {
                            t = ts_it->second;
                        }
                        cJSON_AddNumberToObject(prop, "t", static_cast<double>(t));
                        AddOwnedToArray(props, prop);
                    }
                }
            }
            break;
        }
        case hub_core::RegistryEvent::kHomesUpdated:
        case hub_core::RegistryEvent::kRoomsUpdated:
        case hub_core::RegistryEvent::kDevicesCleared:
            break;  // not part of the WS contract here (see RegistryEventName)
    }
    return root;
}

const char* RegistryEventName(hub_core::RegistryEvent event) {
    switch (event) {
        case hub_core::RegistryEvent::kDeviceAdded: return "device_added";
        case hub_core::RegistryEvent::kDeviceRemoved: return "device_removed";
        case hub_core::RegistryEvent::kDeviceUpdated: return "device_update";
        case hub_core::RegistryEvent::kDeviceOnlineChanged: return "device_online";
        case hub_core::RegistryEvent::kRoomsUpdated: return "rooms_updated";
        case hub_core::RegistryEvent::kHomesUpdated: return nullptr;
        case hub_core::RegistryEvent::kDevicesCleared: return nullptr;
    }
    return nullptr;
}

}  // namespace rest_api
}  // namespace web_server
