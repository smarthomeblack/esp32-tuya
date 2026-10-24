#include "hub_services/openapi_enrichment.h"

#include <cJSON.h>
#include <esp_log.h>

#include <climits>
#include <cmath>
#include <cstring>
#include <string>
#include <string_view>
#include <utility>

#include "openapi_enrichment_internal.h"
#include "parse_helpers.h"  // ParseDpValue (shadow status typing)

#include "tuya_cloud/openapi_client.h"
#include "tuya_cloud/openapi_constants.h"
#include "tuya_crypto/json_fields.h"
#include "tuya_crypto/json_guard.h"

static const char* TAG = "TuyaIoTEnrich";

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

/// Thing Model "type" -> DP value type; unknown -> "Raw" (kept routable
/// via the default strategy).
std::string MapValueType(const std::string& thing_model_type) {
    if (thing_model_type == "bool") {
        return "Boolean";
    }
    if (thing_model_type == "value") {
        return "Integer";
    }
    if (thing_model_type == "enum") {
        return "Enum";
    }
    if (thing_model_type == "string") {
        return "String";
    }
    if (thing_model_type == "json") {
        return "Json";
    }
    if (thing_model_type == "raw") {
        return "Raw";
    }
    if (thing_model_type == "bitmap") {
        return "Bitmap";
    }
    return "Raw";
}

/// typeSpec minus its "type" key, compact JSON — the valueDesc form.
std::string TypeSpecDescJson(const cJSON* type_spec) {
    if (!cJSON_IsObject(type_spec)) {
        return "{}";
    }
    cJSON* dup = cJSON_Duplicate(type_spec, /*recurse=*/1);
    if (dup == nullptr) {
        return "{}";
    }
    cJSON_DeleteItemFromObjectCaseSensitive(dup, "type");
    char* printed = cJSON_PrintUnformatted(dup);
    cJSON_Delete(dup);
    if (printed == nullptr) {
        return "{}";
    }
    std::string out(printed);
    cJSON_free(printed);
    return out;
}

/// The single-key statusFormat {"<code>":""} the strategy engine pops.
std::string StatusFormatForCode(std::string_view code) {
    std::string out;
    out.reserve(code.size() + 8);
    out += "{\"";
    out += code;
    out += "\":\"\"}";
    return out;
}

/// Substitutes "{id}" (the client resolves only "{uid}"); no URL escaping.
std::string DeviceScopedPath(std::string_view path_template, std::string_view dev_id) {
    std::string path(path_template);
    constexpr std::string_view kId = "{id}";
    const size_t pos = path.find(kId);
    if (pos != std::string::npos) {
        path.replace(pos, kId.size(), dev_id);
    }
    return path;
}

/// True for the writable access modes ("rw" | "wr").
bool AccessModeWritable(const std::string& access_mode) {
    return access_mode.find('w') != std::string::npos;
}

}  // namespace

// Internal parse helpers (openapi_enrichment_internal.h)

int ParseThingModel(const cJSON* result, Device& device) {
    if (!cJSON_IsObject(result)) {
        return 0;
    }
    // result.model is a DOUBLE-ENCODED JSON string.
    const cJSON* model = cJSON_GetObjectItemCaseSensitive(result, "model");
    if (!cJSON_IsString(model) || model->valuestring == nullptr) {
        return 0;
    }
    if (!tuya_crypto::JsonNestingWithinLimit(model->valuestring, strlen(model->valuestring),
                                             tuya_crypto::kMaxJsonDepth)) {
        ESP_LOGW(TAG, "thing model nesting over %d — ignored", tuya_crypto::kMaxJsonDepth);
        return 0;
    }
    cJSON* doc = cJSON_Parse(model->valuestring);
    if (doc == nullptr) {
        return 0;
    }

    int added = 0;
    const cJSON* services = cJSON_GetObjectItemCaseSensitive(doc, "services");
    if (cJSON_IsArray(services)) {
        for (const cJSON* service = services->child; service != nullptr; service = service->next) {
            const cJSON* properties = cJSON_IsObject(service)
                                          ? cJSON_GetObjectItemCaseSensitive(service, "properties")
                                          : nullptr;
            if (!cJSON_IsArray(properties)) {
                continue;
            }
            for (const cJSON* prop = properties->child; prop != nullptr; prop = prop->next) {
                if (!cJSON_IsObject(prop)) {
                    continue;
                }
                const cJSON* ability_id = cJSON_GetObjectItemCaseSensitive(prop, "abilityId");
                const cJSON* code_node = cJSON_GetObjectItemCaseSensitive(prop, "code");
                const cJSON* access_node = cJSON_GetObjectItemCaseSensitive(prop, "accessMode");
                const cJSON* type_spec = cJSON_GetObjectItemCaseSensitive(prop, "typeSpec");
                if (!cJSON_IsNumber(ability_id) || !cJSON_IsString(code_node) ||
                    code_node->valuestring == nullptr || code_node->valuestring[0] == '\0' ||
                    !cJSON_IsString(access_node) || !cJSON_IsObject(type_spec)) {
                    continue;
                }
                const std::string spec_type = tuya_crypto::GetStringField(type_spec, "type");
                if (spec_type.empty()) {
                    continue;
                }
                int dp_id = 0;
                if (!JsonIntValue(ability_id, dp_id)) {
                    continue;
                }
                const std::string code = code_node->valuestring;
                const std::string value_type = MapValueType(spec_type);
                const std::string value_desc = TypeSpecDescJson(type_spec);

                // Sharing-wins: an existing dpId keeps its config.
                if (device.local_strategy.count(dp_id) == 0) {
                    StrategyConfig cfg;
                    cfg.dp_id = dp_id;
                    cfg.support_local = false;      // cloud DPs, not local DPs
                    cfg.value_convert = "default";  // only the default strategy is built
                    cfg.status_code = code;
                    cfg.status_format_json = StatusFormatForCode(code);
                    cfg.value_type = value_type;
                    cfg.value_desc_json = value_desc;
                    // enum_mapping stays empty — "default" never consults it.
                    cfg.use_open_api = true;
                    // property_update always true here (issue-first routing).
                    cfg.property_update = true;
                    device.local_strategy.emplace(dp_id, std::move(cfg));
                    ++added;
                }

                // Fill only when in NEITHER map; "rw"/"wr" -> function, else range.
                if (device.function.count(code) == 0 && device.status_range.count(code) == 0) {
                    const char* access_mode =
                        access_node->valuestring != nullptr ? access_node->valuestring : "";
                    if (AccessModeWritable(access_mode)) {
                        DeviceFunction fn;
                        fn.code = code;
                        // Display name; the code is the fallback label.
                        DeviceString name{tuya_crypto::GetStringField(prop, "name")};
                        fn.name = name.empty() ? DeviceString{code} : name;
                        fn.desc = "";
                        fn.type = value_type;
                        fn.values_json = value_desc;
                        device.function.emplace(code, std::move(fn));
                    } else {
                        DeviceStatusRange range;
                        range.code = code;
                        // Display name ("" when absent).
                        range.name = tuya_crypto::GetStringField(prop, "name");
                        range.type = value_type;
                        range.values_json = value_desc;
                        range.report_type = "";  // dp-report-types never ran for these
                        device.status_range.emplace(code, std::move(range));
                    }
                }
            }
        }
    }
    cJSON_Delete(doc);
    return added;
}

int ParseShadowProperties(const cJSON* result, Device& device) {
    if (!cJSON_IsObject(result)) {
        return 0;
    }
    const cJSON* properties = cJSON_GetObjectItemCaseSensitive(result, "properties");
    if (!cJSON_IsArray(properties)) {
        return 0;
    }
    int added = 0;
    for (const cJSON* prop = properties->child; prop != nullptr; prop = prop->next) {
        if (!cJSON_IsObject(prop)) {
            continue;
        }
        const cJSON* dp_id = cJSON_GetObjectItemCaseSensitive(prop, "dp_id");
        int id = 0;
        if (!JsonIntValue(dp_id, id)) {
            continue;  // no usable dp_id
        }
        const DeviceString code{tuya_crypto::GetStringField(prop, "code")};
        if (code.empty()) {
            continue;  // no code: nothing to fill
        }

        // Sharing-wins status snapshot; null stores the empty DpValue.
        if (device.status.count(code) == 0) {
            device.status[code] = ParseDpValue(cJSON_GetObjectItemCaseSensitive(prop, "value"));
        }

        // Minimal strategy for model-less dpIds; needs the "type" field.
        if (device.local_strategy.count(id) == 0) {
            const cJSON* type = cJSON_GetObjectItemCaseSensitive(prop, "type");
            if (cJSON_IsString(type) && type->valuestring != nullptr) {
                StrategyConfig cfg;
                cfg.dp_id = id;
                cfg.support_local = false;
                cfg.value_convert = "default";
                cfg.status_code = code;
                cfg.status_format_json = StatusFormatForCode(code);
                cfg.value_type = MapValueType(type->valuestring);
                cfg.value_desc_json = "{}";
                cfg.use_open_api = true;
                // property_update always true (issue-first routing).
                cfg.property_update = true;
                device.local_strategy.emplace(id, std::move(cfg));
                ++added;
            }
        }
    }
    return added;
}

int EnrichDeviceViaOpenApi(Device& device) {
    tuya_cloud::OpenApiClient& api = tuya_cloud::OpenApiClient::GetInstance();
    // No credentials/token -> silent no-op (0 added).
    if (!api.IsConfigured() || !api.HasToken()) {
        return 0;
    }
    if (device.id.empty()) {
        return 0;
    }

    int added = 0;

    // (1) Thing Model — a failure warns and leaves the model untouched.
    tuya_cloud::OpenApiResponse resp =
        api.Get(DeviceScopedPath(tuya_cloud::kOpenApiThingModel, device.id));
    if (resp.ok && resp.result != nullptr) {
        added += ParseThingModel(resp.result, device);
    } else {
        ESP_LOGW(TAG, "Device %s: thing model fetch failed (%s) — specs stay as-is",
                 device.id.c_str(), resp.msg.c_str());
    }

    // (2) Shadow Properties (current dp values + model-less dpIds).
    resp = api.Get(DeviceScopedPath(tuya_cloud::kOpenApiThingShadow, device.id));
    if (resp.ok && resp.result != nullptr) {
        added += ParseShadowProperties(resp.result, device);
    } else {
        ESP_LOGW(TAG, "Device %s: shadow properties fetch failed (%s) — status snapshot skipped",
                 device.id.c_str(), resp.msg.c_str());
    }

    return added;
}

}  // namespace hub_core
