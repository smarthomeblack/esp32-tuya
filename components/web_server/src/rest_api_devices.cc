// Device list + per-device command/entity/remove endpoints.

#include "rest_api_internal.h"

#include <esp_log.h>

#include <cstdio>
#include <cstring>
#include <memory>
#include <string_view>
#include <utility>

#include "hub_core/device_registry.h"
#include "hub_services/command_service.h"
#include "web_server/api_auth.h"

static const char* TAG = "RestApi";

namespace web_server {
namespace rest_api {

namespace {

/// Marshalled command job; the result travels back via the shared_ptr.
struct CommandJob {
    std::string dev_id;
    std::vector<hub_core::DeviceCommand> commands;
    hub_core::CommandResult result = hub_core::CommandResult::kError;
};

/// Marshalled entity-selection job; results + fresh row strings via shared_ptr.
struct EntitySelectionJob {
    std::string dev_id;
    std::vector<std::pair<std::string, bool>> changes;  // code -> enabled
    bool ok = false;
    std::vector<std::string> entity_items;  // fresh rows (printed JSON strings)
};

// POST /api/devices/{id}/commands — body {"commands":[{"code":..,"value":..}]}.
// Auth is checked ONCE in the wildcard dispatcher (wrapper-only handler).
esp_err_t HandleDeviceCommandsApi(httpd_req_t* req) {
    std::string dev_id;
    if (!ParseIdPath(req, "/api/devices/", "/commands", dev_id)) {
        return RespondJsonError(req, "404 Not Found", "not_found", "Unknown device path");
    }
    // 404 on an unknown device BEFORE any cloud round-trip (RAM lookup only).
    hub_core::Device device;
    if (!hub_core::DeviceRegistry::GetInstance().GetDevice(dev_id, device)) {
        return RespondJsonError(req, "404 Not Found", "not_found", "Unknown device id");
    }

    std::string body;
    if (!ReadBody(req, body, kMaxCommandsBody, false)) {
        return ESP_FAIL;  // response already sent
    }
    cJSON* root = BodyNestingOk(body) ? cJSON_Parse(body.c_str()) : nullptr;
    if (root == nullptr) {
        return RespondJsonError(req, "400 Bad Request", "invalid_param", "Invalid JSON");
    }
    const cJSON* commands_item = cJSON_GetObjectItemCaseSensitive(root, "commands");
    if (!cJSON_IsArray(commands_item) || cJSON_GetArraySize(commands_item) == 0) {
        cJSON_Delete(root);
        return RespondJsonError(req, "400 Bad Request", "invalid_param",
                                "'commands' must be a non-empty array");
    }
    auto job = std::make_shared<CommandJob>();
    job->dev_id = dev_id;
    bool valid = true;
    for (const cJSON* item = commands_item->child; item != nullptr; item = item->next) {
        const cJSON* code_item = cJSON_GetObjectItemCaseSensitive(item, "code");
        if (!cJSON_IsString(code_item) || code_item->valuestring == nullptr ||
            code_item->valuestring[0] == '\0') {
            valid = false;
            break;
        }
        const cJSON* value_item = cJSON_GetObjectItemCaseSensitive(item, "value");
        hub_core::DpValue value;
        if (!DpValueFromJson(value_item, value)) {
            valid = false;
            break;
        }
        job->commands.push_back(hub_core::DeviceCommand{code_item->valuestring, value});
    }
    cJSON_Delete(root);
    if (!valid || job->commands.empty()) {
        return RespondJsonError(req, "400 Bad Request", "invalid_param",
                                "Each command needs a non-empty 'code' and a bool/number/"
                                "string 'value'");
    }

    // Disabled entities rejected before marshalling (service guard = layer 2).
    for (const hub_core::DeviceCommand& command : job->commands) {
        if (device.disabled_entities.count(command.code) > 0) {
            char message[128];
            snprintf(message, sizeof(message), "Entity '%.80s' is disabled on this device",
                     command.code.c_str());
            return RespondJsonError(req, "400 Bad Request", "entity_disabled", message);
        }
    }

    // Cloud I/O runs on the worker task (the main loop stays free); ≤ 25 s.
    const WorkerResult result = RunOnWorkerAndWait([job]() {
        job->result =
            hub_core::CommandService::GetInstance().SendDeviceCommands(job->dev_id, job->commands);
    });
    if (result != WorkerResult::kCompleted) {
        return RespondWorkerError(req, result, "Device command timed out");
    }
    if (job->result == hub_core::CommandResult::kSent ||
        job->result == hub_core::CommandResult::kDeduplicated) {
        // Deduplicated (10 s dup filter) still counts as success.
        return RespondJson(req, "{\"success\":true}", "200 OK");
    }
    return RespondJsonError(req, "502 Bad Gateway", "command_failed",
                            "Device command failed (cloud error)");
}

// POST /api/devices/{id}/entities — empty body: current list; body
// {"entities":{code:bool}}: mutation. Auth: wildcard dispatcher only.
esp_err_t HandleDeviceEntitiesApi(httpd_req_t* req) {
    std::string dev_id;
    if (!ParseIdPath(req, "/api/devices/", "/entities", dev_id)) {
        return RespondJsonError(req, "404 Not Found", "not_found", "Unknown device path");
    }
    // 404 on an unknown device BEFORE any registry mutation.
    hub_core::Device device;
    if (!hub_core::DeviceRegistry::GetInstance().GetDevice(dev_id, device)) {
        return RespondJsonError(req, "404 Not Found", "not_found", "Unknown device id");
    }

    std::string body;
    if (!ReadBody(req, body, kMaxCommandsBody, true)) {
        return ESP_FAIL;  // response already sent
    }
    if (body.empty()) {
        // Snapshot only: {id, entities:[...]} (one device's rows).
        cJSON* root = cJSON_CreateObject();
        if (root == nullptr) {
            return RespondJsonError(req, "500 Internal Server Error", "internal", "Out of memory");
        }
        cJSON_AddStringToObject(root, "id", device.id.c_str());
        AttachOwned(root, "entities", BuildEntityRows(device));
        return SendJson(req, root);
    }

    cJSON* root = BodyNestingOk(body) ? cJSON_Parse(body.c_str()) : nullptr;
    if (root == nullptr) {
        return RespondJsonError(req, "400 Bad Request", "invalid_param", "Invalid JSON");
    }
    const cJSON* entities_item = cJSON_GetObjectItemCaseSensitive(root, "entities");
    if (!cJSON_IsObject(entities_item)) {
        cJSON_Delete(root);
        return RespondJsonError(req, "400 Bad Request", "invalid_param",
                                "'entities' must be an object of {code: bool}");
    }
    auto job = std::make_shared<EntitySelectionJob>();
    job->dev_id = dev_id;
    bool valid = true;
    for (const cJSON* entry = entities_item->child; entry != nullptr; entry = entry->next) {
        // Object keys are the entity codes; values must be plain bools.
        if (entry->string == nullptr || entry->string[0] == '\0' || !cJSON_IsBool(entry)) {
            valid = false;
            break;
        }
        job->changes.emplace_back(entry->string, cJSON_IsTrue(entry));
    }
    cJSON_Delete(root);
    if (!valid) {
        return RespondJsonError(req, "400 Bad Request", "invalid_param",
                                "Each entity needs a non-empty code and a bool value");
    }

    const MainLoopResult result = RunOnMainLoop([job]() {
        hub_core::DeviceRegistry& registry = hub_core::DeviceRegistry::GetInstance();
        job->ok = true;
        for (const auto& change : job->changes) {
            if (!registry.SetEntityEnabled(job->dev_id, change.first, change.second)) {
                // SetEntityEnabled fails only on an unknown device (a raced remove).
                job->ok = false;
            }
        }
        // Fresh rows printed inside the job from the re-read device.
        hub_core::Device fresh;
        if (registry.GetDevice(job->dev_id, fresh)) {
            CollectEntityRowStrings(fresh, job->entity_items);
        }
    });
    if (result != MainLoopResult::kCompleted) {
        return RespondMainLoopError(req, result, "Entity selection timed out");
    }
    if (!job->ok) {
        return RespondJsonError(req, "500 Internal Server Error", "internal",
                                "Entity selection failed (device vanished)");
    }
    return StreamItemArray(req, "{\"success\":true,\"entities\":[", job->entity_items);
}

// POST /api/devices/{id}/remove — 404 unknown; marshalled; raced double-delete -> 500.
// Auth is checked ONCE in the wildcard dispatcher (wrapper-only handler).
esp_err_t HandleDeviceRemoveApi(httpd_req_t* req) {
    std::string dev_id;
    if (!ParseIdPath(req, "/api/devices/", "/remove", dev_id)) {
        return RespondJsonError(req, "404 Not Found", "not_found", "Unknown device path");
    }
    hub_core::Device device;
    if (!hub_core::DeviceRegistry::GetInstance().GetDevice(dev_id, device)) {
        return RespondJsonError(req, "404 Not Found", "not_found", "Unknown device id");
    }

    auto ok = std::make_shared<bool>(false);
    const MainLoopResult result = RunOnMainLoop([dev_id, ok]() {
        *ok = hub_core::DeviceRegistry::GetInstance().RemoveDevice(dev_id);
        if (!*ok) {
            // Only via a raced delete between the 404 check and this job.
            ESP_LOGW(TAG, "remove: device %s vanished before the job ran", dev_id.c_str());
        }
    });
    if (result != MainLoopResult::kCompleted) {
        return RespondMainLoopError(req, result, "Device removal timed out");
    }
    if (!*ok) {
        return RespondJsonError(req, "500 Internal Server Error", "internal",
                                "Device removal failed (raced delete)");
    }
    return RespondJson(req, "{\"success\":true}", "200 OK");
}

}  // namespace

// GET /api/devices[?home_id=] — chunked; one device cJSON tree alive at a time.
esp_err_t HandleDevicesApi(httpd_req_t* req) {
    if (RequireAuth(req) != ESP_OK)
        return ESP_FAIL;  // 401 already sent
    const std::string home_filter = HomeIdFilter(req);

    // Snapshot under the registry mutex, then build/print the JSON outside it —
    // a slow cJSON + string path must not stall the MQTT/UPSERT tasks.
    std::vector<hub_core::Device> devices;
    hub_core::DeviceRegistry::GetInstance().ForEachDevice([&](const hub_core::Device& device) {
        if (!home_filter.empty() && std::string_view(device.home_id) != home_filter) {
            return;
        }
        devices.push_back(device);
    });

    const RoomNameResolver room_names = MakeRoomNameResolver();
    std::vector<std::string> items;
    bool capped = false;
    size_t budget = 0;
    for (const hub_core::Device& device : devices) {
        if (budget > kMaxDevicesResponse) {
            capped = true;  // stop adding; the list may be truncated
            break;
        }
        cJSON* item = BuildDeviceObject(device, room_names);
        if (item == nullptr) {
            continue;  // OOM — skip this device
        }
        char* item_str = cJSON_PrintUnformatted(item);
        cJSON_Delete(item);
        if (item_str == nullptr) {
            continue;
        }
        budget += strlen(item_str) + 1;
        items.emplace_back(item_str);
        cJSON_free(item_str);
    }
    if (capped) {
        ESP_LOGE(TAG, "/api/devices exceeded the %u byte budget — response truncated",
                 static_cast<unsigned>(kMaxDevicesResponse));
    }

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    esp_err_t ret = httpd_resp_send_chunk(req, "[", 1);
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
        ret = httpd_resp_send_chunk(req, nullptr, 0);  // terminating chunk
    }
    return ret;
}

// POST /api/devices/* dispatcher (wildcards match only a trailing '*'):
// suffix-routes /entities, /remove, /commands; auth is checked HERE only.
esp_err_t HandleDeviceWildcardApi(httpd_req_t* req) {
    if (RequireAuth(req) != ESP_OK)
        return ESP_FAIL;  // 401 already sent
    const std::string path = UrlPath(req);
    const size_t len = path.size();
    const auto ends_with = [&path, len](const char* suffix) {
        const size_t suffix_len = strlen(suffix);
        return len >= suffix_len && path.compare(len - suffix_len, suffix_len, suffix) == 0;
    };
    if (ends_with("/entities")) {
        return HandleDeviceEntitiesApi(req);
    }
    if (ends_with("/remove")) {
        return HandleDeviceRemoveApi(req);
    }
    return HandleDeviceCommandsApi(req);
}

}  // namespace rest_api
}  // namespace web_server
