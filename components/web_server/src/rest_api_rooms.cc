// Room endpoints: the room list + a manual room refresh trigger.

#include "rest_api_internal.h"

#include <esp_log.h>

#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "hub_core/device_registry.h"
#include "web_server/api_auth.h"

static const char* TAG = "RestApi";

namespace web_server {
namespace rest_api {

// GET /api/rooms — [{id, name, sort, home_id, device_count}], chunked.
// Empty = rooms not fetched yet (unknown), not "the account has no rooms".
esp_err_t HandleRoomsApi(httpd_req_t* req) {
    if (RequireAuth(req) != ESP_OK)
        return ESP_FAIL;  // 401 already sent
    hub_core::DeviceRegistry& registry = hub_core::DeviceRegistry::GetInstance();
    const std::vector<hub_core::Home> homes = registry.GetHomes();
    // Devices are already room-tagged by the last complete sweep.
    std::map<std::string, size_t> counts;  // home_id + '\n' + room_id -> devices
    registry.ForEachDevice([&counts](const hub_core::Device& device) {
        if (!device.room_id.empty()) {
            ++counts[std::string(device.home_id) + '\n' + std::string(device.room_id)];
        }
    });

    std::vector<std::string> items;
    for (const hub_core::Home& home : homes) {
        for (const hub_core::Room& room : home.rooms) {
            cJSON* item = cJSON_CreateObject();
            if (item == nullptr) {
                continue;  // OOM — skip this room
            }
            cJSON_AddStringToObject(item, "id", room.id.c_str());
            cJSON_AddStringToObject(item, "name", room.name.c_str());
            cJSON_AddNumberToObject(item, "sort", room.sort);
            cJSON_AddStringToObject(item, "home_id", home.id.c_str());
            const auto found = counts.find(std::string(home.id) + '\n' + std::string(room.id));
            cJSON_AddNumberToObject(item, "device_count",
                                    found != counts.end() ? static_cast<double>(found->second)
                                                          : 0.0);
            char* item_str = cJSON_PrintUnformatted(item);
            cJSON_Delete(item);
            if (item_str == nullptr) {
                continue;
            }
            items.emplace_back(item_str);
            cJSON_free(item_str);
        }
    }
    return StreamItemArray(req, "[", items);
}

// POST /api/rooms/refresh — spawns the "tuya_rooms" task (202, nothing
// blocks); 200 when one was already in flight; 500 when the hook is unwired.
esp_err_t HandleRoomsRefreshApi(httpd_req_t* req) {
    if (RequireAuth(req) != ESP_OK)
        return ESP_FAIL;  // 401 already sent
    auto started = std::make_shared<bool>(false);
    auto wired = std::make_shared<bool>(false);
    // The trigger itself is lock-free (a task spawn + a latch CAS).
    const MainLoopResult result = RunOnMainLoop([started, wired]() {
        RoomRefreshTrigger trigger;
        {
            std::lock_guard<std::mutex> lock(g_room_trigger_mutex);
            trigger = g_room_refresh_trigger;
        }
        if (trigger == nullptr) {
            return;
        }
        *wired = true;
        *started = trigger();
    });
    if (result != MainLoopResult::kCompleted) {
        return RespondMainLoopError(req, result, "Room refresh start timed out");
    }
    if (!*wired) {
        return RespondJsonError(req, "500 Internal Server Error", "internal",
                                "Room refresh is unavailable");
    }
    if (!*started) {
        return RespondJson(req, "{\"success\":true,\"already_running\":true}", "200 OK");
    }
    return RespondJson(req, "{\"success\":true}", "202 Accepted");
}

}  // namespace rest_api
}  // namespace web_server
