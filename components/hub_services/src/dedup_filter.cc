// Pure command request-build/parse helpers for the sharing path (dedup_filter.h).

#include <cJSON.h>

#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "tuya_crypto/json_writer.h"

#include "dedup_filter.h"

namespace hub_core {

bool BuildCommandsBody(const std::vector<DeviceCommand>& commands, std::string& body) {
    // Compact {"commands":[{code,value}]}; double via %.17g (round-trip exact).
    if (commands.empty()) {
        return false;
    }
    tuya_crypto::JsonWriter writer;
    writer.BeginObject();
    writer.Key("commands");
    writer.BeginArray();
    for (const DeviceCommand& command : commands) {
        if (command.code.empty() || command.value.IsEmpty()) {
            return false;
        }
        writer.BeginObject();
        writer.Key("code");
        writer.String(command.code);
        writer.Key("value");
        switch (command.value.type) {
            case DpValue::Type::kBool: writer.Bool(command.value.b); break;
            case DpValue::Type::kInt: writer.Int(command.value.i); break;
            case DpValue::Type::kDouble: writer.Double(command.value.d); break;
            case DpValue::Type::kString: writer.String(command.value.s); break;
        }
        writer.EndObject();
    }
    writer.EndArray();
    writer.EndObject();
    if (writer.HasError()) {
        return false;
    }
    body = writer.Str();
    return true;
}

std::string BuildSceneTriggerBody(const std::string& home_id, const std::string& scene_id) {
    tuya_crypto::JsonWriter writer;
    writer.BeginObject();
    writer.Key("homeId");
    writer.String(home_id);
    writer.Key("sceneId");
    writer.String(scene_id);
    writer.EndObject();
    return writer.Str();
}

size_t ParseScenes(const cJSON* result, std::string_view home_id, std::vector<Scene>& out) {
    if (!cJSON_IsArray(result)) {
        // A non-array result is a parse failure for this home.
        return 0;
    }
    size_t added = 0;
    for (const cJSON* item = result->child; item != nullptr; item = item->next) {
        Scene scene;
        scene.home_id = home_id;

        const cJSON* scene_id = cJSON_GetObjectItemCaseSensitive(item, "scene_id");
        if (!cJSON_IsString(scene_id) || scene_id->valuestring == nullptr ||
            scene_id->valuestring[0] == '\0') {
            // No usable scene_id — skip the row.
            continue;
        }
        scene.scene_id = scene_id->valuestring;

        const cJSON* name = cJSON_GetObjectItemCaseSensitive(item, "name");
        if (cJSON_IsString(name) && name->valuestring != nullptr) {
            scene.name = name->valuestring;
        }

        const cJSON* enabled = cJSON_GetObjectItemCaseSensitive(item, "enabled");
        if (cJSON_IsBool(enabled)) {
            scene.enabled = cJSON_IsTrue(enabled);
        }

        // "actions" is deliberately not parsed (nothing consumes it).

        out.push_back(std::move(scene));
        ++added;
    }
    return added;
}

}  // namespace hub_core
