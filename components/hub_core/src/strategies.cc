#include "hub_core/strategies.h"

#include <cJSON.h>
#include <esp_log.h>

#include <cctype>
#include <cmath>
#include <cstdint>
#include <string>
#include <string_view>

#include "hub_core/dp_value.h"

#include "tuya_crypto/json_guard.h"

static const char* TAG = "Strategies";

namespace hub_core {

namespace {

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

/// ASCII str.capitalize().
std::string CapitalizeAscii(std::string_view value) {
    if (value.empty()) {
        return "";
    }
    std::string out(value);
    out[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(out[0])));
    for (size_t i = 1; i < out.size(); ++i) {
        out[i] = static_cast<char>(std::tolower(static_cast<unsigned char>(out[i])));
    }
    return out;
}

/// RAII guard for parsed cJSON trees (no exceptions; guarded frees).
/// Depth gate first: statusFormat/valueDesc/values strings are cloud-sourced.
class JsonTree {
public:
    explicit JsonTree(const DeviceString& text)
        : root_(tuya_crypto::JsonNestingWithinLimit(text.c_str(), text.size(),
                                                    tuya_crypto::kMaxJsonDepth)
                    ? cJSON_Parse(text.c_str())
                    : nullptr) {}
    ~JsonTree() { cJSON_Delete(root_); }
    JsonTree(const JsonTree&) = delete;
    JsonTree& operator=(const JsonTree&) = delete;

    cJSON* root() const { return root_; }
    bool valid() const { return root_ != nullptr; }

private:
    cJSON* root_;
};

}  // namespace

bool ConvertDpReport(std::string_view dev_id, const DeviceStrategyMap& local_strategy,
                     const DeviceStatusRangeMap& status_range, int dp_id, const DpValue& raw,
                     std::string& out_code, DpValue& out_value) {
    const auto it = local_strategy.find(dp_id);
    if (it == local_strategy.end()) {
        ESP_LOGD(TAG, "report %.*s: dpId %d has no strategy — dropped",
                 static_cast<int>(dev_id.size()), dev_id.data(), dp_id);
        return false;
    }
    if (!Strategies::Convert(it->second, dp_id, raw, out_code, out_value, status_range)) {
        ESP_LOGD(TAG, "report %.*s: dpId %d dropped by strategy conversion",
                 static_cast<int>(dev_id.size()), dev_id.data(), dp_id);
        return false;
    }
    return true;
}

bool Strategies::Convert(const StrategyConfig& cfg, int dp_id, const DpValue& raw,
                         std::string& out_code, DpValue& out_value,
                         const DeviceStatusRangeMap& status_range) {
    const DeviceString& name = cfg.value_convert;
    bool converted = false;
    if (name == "default") {
        converted = ConvertDefault(cfg, raw, out_code, out_value);
    } else if (name == "enum") {
        converted = ConvertEnum(cfg, raw, out_code, out_value);
    } else {
        ESP_LOGW(TAG, "dp %d: unknown strategy '%s' — dropping report", dp_id, name.c_str());
        return false;
    }
    if (!converted) {
        return false;
    }
    // Enum range gate, applied to every converted value.
    if (!PassesEnumRangeGate(out_code, out_value, status_range)) {
        ESP_LOGD(TAG, "dp %d: value %s not in enum range of '%s' — dropping", dp_id,
                 out_value.AsKeyString().c_str(), out_code.c_str());
        return false;
    }
    return true;
}

bool Strategies::ConvertDefault(const StrategyConfig& cfg, const DpValue& raw,
                                std::string& out_code, DpValue& out_value) {
    if (!CodeFromStatusFormat(cfg.status_format_json, out_code)) {
        return false;
    }
    if (!raw.IsNullOrEmpty()) {
        out_value = raw;
        return true;
    }
    return ConvertDefaultValue(cfg, out_value);
}

bool Strategies::ConvertEnum(const StrategyConfig& cfg, const DpValue& raw, std::string& out_code,
                             DpValue& out_value) {
    if (!CodeFromStatusFormat(cfg.status_format_json, out_code)) {
        return false;
    }
    // Key-string lookup; a miss falls to the default value.
    const std::string key = raw.AsKeyString();
    auto it = cfg.enum_mapping.find(key);
    if (it != cfg.enum_mapping.end()) {
        out_value = DpValue::FromString(it->second);
        return true;
    }
    return ConvertDefaultValue(cfg, out_value);
}

bool Strategies::ConvertDefaultValue(const StrategyConfig& cfg, DpValue& out_value) {
    const std::string value_type = CapitalizeAscii(cfg.value_type);
    if (value_type == "Boolean") {
        out_value = DpValue::FromBool(false);
        return true;
    }
    if (value_type == "Integer") {
        // Missing "min" is unproducible -> drop (no None form in DpValue).
        JsonTree desc(cfg.value_desc_json);
        if (desc.valid() && cJSON_IsObject(desc.root())) {
            const cJSON* min = cJSON_GetObjectItemCaseSensitive(desc.root(), "min");
            int64_t min_value = 0;
            if (JsonIntValue(min, min_value)) {
                out_value = DpValue::FromInt(min_value);
                return true;
            }
        }
        ESP_LOGW(TAG, "Integer default needs an integral valueDesc.min — dropping");
        return false;
    }
    if (value_type == "Enum") {
        // Missing/empty "range" is unproducible -> drop.
        JsonTree desc(cfg.value_desc_json);
        if (desc.valid() && cJSON_IsObject(desc.root())) {
            const cJSON* range = cJSON_GetObjectItemCaseSensitive(desc.root(), "range");
            if (cJSON_IsArray(range) && cJSON_GetArraySize(range) > 0) {
                const cJSON* first = cJSON_GetArrayItem(range, 0);
                if (cJSON_IsString(first) && first->valuestring != nullptr) {
                    out_value = DpValue::FromString(first->valuestring);
                    return true;
                }
            }
        }
        ESP_LOGW(TAG, "Enum default needs a non-empty valueDesc.range — dropping");
        return false;
    }
    out_value = DpValue::FromString("");
    return true;
}

bool Strategies::CodeFromStatusFormat(const DeviceString& status_format_json,
                                      std::string& out_code) {
    // The code is the only key of the statusFormat object.
    JsonTree format(status_format_json);
    if (format.valid() && cJSON_IsObject(format.root())) {
        const cJSON* child = format.root()->child;
        if (child != nullptr && child->string != nullptr) {
            out_code = child->string;
            // Multi-key statusFormat: deterministically takes the first.
            return true;
        }
    }
    ESP_LOGW(TAG, "statusFormat is not a JSON object with a key — dropping");
    return false;
}

bool Strategies::PassesEnumRangeGate(const std::string& code, const DpValue& value,
                                     const DeviceStatusRangeMap& status_range) {
    auto it = status_range.find(code);
    if (it == status_range.end()) {
        return true;
    }
    if (it->second.type != "Enum") {
        return true;
    }
    // Unparsable values -> keep (fail-open); a missing "range" -> drop.
    JsonTree values(it->second.values_json);
    if (!values.valid() || !cJSON_IsObject(values.root())) {
        ESP_LOGW(TAG, "unparsable status_range values for '%s' — keeping value", code.c_str());
        return true;
    }
    const cJSON* range = cJSON_GetObjectItemCaseSensitive(values.root(), "range");
    if (!cJSON_IsArray(range)) {
        return false;
    }
    const std::string needle = value.AsKeyString();
    for (const cJSON* item = range->child; item != nullptr; item = item->next) {
        if (cJSON_IsString(item) && item->valuestring != nullptr && needle == item->valuestring) {
            return true;
        }
        // Numeric equality: 5 == 5.0 (int vs float DP values compare equal).
        if (cJSON_IsNumber(item) && value.type == DpValue::Type::kInt) {
            int64_t item_value = 0;
            if (JsonIntValue(item, item_value) && item_value == value.i) {
                return true;
            }
        }
    }
    return false;
}

}  // namespace hub_core
