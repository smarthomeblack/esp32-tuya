#include "hub_services/openapi_enrichment_test.h"

#include <cJSON.h>
#include <esp_log.h>

#include <cstdio>
#include <string>
#include <string_view>

#include "hub_core/device.h"
#include "hub_core/dp_value.h"
#include "hub_core/strategies.h"
#include "openapi_enrichment_internal.h"

static const char* TAG = "EnrichSelfTest";

namespace hub_core {

namespace {

struct TestResult {
    int total = 0;
    int passed = 0;
};

void CheckTrue(TestResult& r, const char* name, bool condition) {
    ++r.total;
    if (condition) {
        ++r.passed;
        return;
    }
    ESP_LOGE(TAG, "FAIL %s", name);
}

void CheckEqStr(TestResult& r, const char* name, std::string_view expected, std::string_view got) {
    ++r.total;
    if (expected == got) {
        ++r.passed;
        return;
    }
    ESP_LOGE(TAG, "FAIL %s: expected=\"%.*s\" got=\"%.*s\"", name,
             static_cast<int>(expected.size()), expected.data(), static_cast<int>(got.size()),
             got.data());
}

void CheckEqInt(TestResult& r, const char* name, long long expected, long long got) {
    ++r.total;
    if (expected == got) {
        ++r.passed;
        return;
    }
    ESP_LOGE(TAG, "FAIL %s: expected=%lld got=%lld", name, expected, got);
}

/// RAII cJSON tree (guarded free, no exceptions).
class JsonPtr {
public:
    explicit JsonPtr(const char* text) : root_(cJSON_Parse(text)) {}
    ~JsonPtr() { cJSON_Delete(root_); }
    JsonPtr(const JsonPtr&) = delete;
    JsonPtr& operator=(const JsonPtr&) = delete;

    cJSON* root() const { return root_; }

private:
    cJSON* root_;
};

// Golden fragments — shaped like the real Tuya OpenAPI responses.

/// Thing Model result: result.model is a double-encoded JSON string.
constexpr const char* kThingModelResultJson =
    "{\"model\":\"{\\\"services\\\":[{\\\"properties\\\":["
    "{\\\"abilityId\\\":1,\\\"code\\\":\\\"switch_1\\\",\\\"accessMode\\\":\\\"rw\\\","
    "\\\"typeSpec\\\":{\\\"type\\\":\\\"bool\\\"}},"
    "{\\\"abilityId\\\":17,\\\"code\\\":\\\"cur_current\\\",\\\"accessMode\\\":\\\"ro\\\","
    "\\\"typeSpec\\\":{\\\"type\\\":\\\"value\\\",\\\"min\\\":-30000,\\\"max\\\":30000,"
    "\\\"scale\\\":3,\\\"step\\\":1,\\\"unit\\\":\\\"mA\\\"}},"
    "{\\\"abilityId\\\":19,\\\"code\\\":\\\"add_ele\\\",\\\"accessMode\\\":\\\"ro\\\","
    "\\\"typeSpec\\\":{\\\"type\\\":\\\"value\\\",\\\"min\\\":0,\\\"max\\\":1000000,"
    "\\\"scale\\\":3,\\\"step\\\":100,\\\"unit\\\":\\\"kWh\\\"}},"
    "{\\\"abilityId\\\":6,\\\"code\\\":\\\"fault\\\",\\\"accessMode\\\":\\\"ro\\\","
    "\\\"typeSpec\\\":{\\\"type\\\":\\\"bitmap\\\",\\\"label\\\":[\\\"ov_cr\\\"]}}"
    "]}]}\"}";

/// Shadow Properties result: current dp values (dp_id/code/type/value).
constexpr const char* kShadowResultJson =
    "{\"properties\":["
    "{\"dp_id\":1,\"code\":\"switch_1\",\"type\":\"bool\",\"value\":true},"
    "{\"dp_id\":17,\"code\":\"cur_current\",\"type\":\"value\",\"value\":-1896}"
    "]}";

/// Thing Model result #2: type variants, malformed rows (skipped),
/// a second service, one fractional abilityId (skipped).
constexpr const char* kThingModelResult2Json =
    "{\"model\":\"{\\\"services\\\":["
    "{\\\"properties\\\":["
    "{\\\"abilityId\\\":20,\\\"code\\\":\\\"countdown\\\",\\\"accessMode\\\":\\\"wr\\\","
    "\\\"name\\\":\\\"Countdown Timer\\\","
    "\\\"typeSpec\\\":{\\\"type\\\":\\\"value\\\",\\\"min\\\":0,\\\"max\\\":86400,"
    "\\\"scale\\\":0,\\\"step\\\":1,\\\"propertyUpdate\\\":true}},"
    "{\\\"abilityId\\\":21,\\\"code\\\":\\\"mode\\\",\\\"accessMode\\\":\\\"rw\\\","
    "\\\"typeSpec\\\":{\\\"type\\\":\\\"enum\\\",\\\"range\\\":[\\\"1\\\",\\\"2\\\"]}},"
    "{\\\"abilityId\\\":22,\\\"code\\\":\\\"fault_str\\\",\\\"accessMode\\\":\\\"ro\\\","
    "\\\"typeSpec\\\":{\\\"type\\\":\\\"string\\\",\\\"maxlen\\\":255}},"
    "{\\\"abilityId\\\":23,\\\"code\\\":\\\"schedule\\\",\\\"accessMode\\\":\\\"ro\\\","
    "\\\"typeSpec\\\":{\\\"type\\\":\\\"date\\\"}},"
    "{\\\"abilityId\\\":24,\\\"code\\\":\\\"broken_no_spec\\\",\\\"accessMode\\\":\\\"rw\\\"},"
    "{\\\"abilityId\\\":\\\"25\\\",\\\"code\\\":\\\"broken_string_id\\\","
    "\\\"accessMode\\\":\\\"rw\\\",\\\"typeSpec\\\":{\\\"type\\\":\\\"bool\\\"}},"
    "{\\\"abilityId\\\":25.5,\\\"code\\\":\\\"fractional_id\\\",\\\"accessMode\\\":\\\"rw\\\","
    "\\\"typeSpec\\\":{\\\"type\\\":\\\"bool\\\"}}"
    "]},"
    "{\\\"properties\\\":["
    "{\\\"abilityId\\\":30,\\\"code\\\":\\\"second_service_dp\\\","
    "\\\"accessMode\\\":\\\"rw\\\",\\\"typeSpec\\\":{\\\"type\\\":\\\"json\\\"}}"
    "]}"
    "]}\"}";

/// Shadow result #2: model-less dpIds; rows without dp_id/code skipped.
constexpr const char* kShadowResult2Json =
    "{\"properties\":["
    "{\"dp_id\":9,\"code\":\"relay_status\",\"type\":\"bool\",\"value\":false},"
    "{\"dp_id\":10,\"code\":\"no_type\",\"value\":42},"
    "{\"code\":\"no_dp_id\",\"type\":\"bool\",\"value\":true},"
    "{\"dp_id\":11,\"type\":\"bool\",\"value\":1}"
    "]}";

// Test sections

/// The primary golden flow: model + shadow on a spec-poor device.
void TestGoldenFlow(TestResult& r) {
    Device device;
    device.id = "bep_meter_double";
    int model_added = 0;
    int shadow_added = 0;
    {
        JsonPtr model(kThingModelResultJson);
        model_added = ParseThingModel(model.root(), device);
    }
    {
        JsonPtr shadow(kShadowResultJson);
        shadow_added = ParseShadowProperties(shadow.root(), device);
    }

    // 4 strategies; the shadow found both dpIds known (1, 17).
    CheckEqInt(r, "golden.model_added", 4, model_added);
    CheckEqInt(r, "golden.shadow_added", 0, shadow_added);
    CheckEqInt(r, "golden.local_strategy.count", 4,
               static_cast<long long>(device.local_strategy.size()));

    // dp 1 — switch_1 (rw -> function).
    CheckTrue(r, "golden.dp1.fields",
              device.local_strategy.count(1) == 1 && device.local_strategy[1].dp_id == 1 &&
                  device.local_strategy[1].value_convert == "default" &&
                  device.local_strategy[1].status_code == "switch_1" &&
                  device.local_strategy[1].status_format_json == "{\"switch_1\":\"\"}" &&
                  device.local_strategy[1].value_type == "Boolean" &&
                  device.local_strategy[1].value_desc_json == "{}" &&
                  device.local_strategy[1].enum_mapping.empty() &&
                  !device.local_strategy[1].support_local &&
                  device.local_strategy[1].use_open_api &&
                  device.local_strategy[1].property_update);
    CheckEqStr(r, "golden.dp1.value_desc", "{}", device.local_strategy[1].value_desc_json);
    CheckTrue(r, "golden.function.switch_1",
              device.function.count("switch_1") == 1 &&
                  device.function["switch_1"].code == "switch_1" &&
                  device.function["switch_1"].name == "switch_1" &&
                  device.function["switch_1"].desc.empty() &&
                  device.function["switch_1"].type == "Boolean" &&
                  device.function["switch_1"].values_json == "{}");

    // dp 17 — cur_current (ro -> status_range, Integer valueDesc verbatim).
    CheckTrue(r, "golden.dp17.fields",
              device.local_strategy.count(17) == 1 &&
                  device.local_strategy[17].value_type == "Integer" &&
                  device.local_strategy[17].use_open_api);
    CheckEqStr(r, "golden.dp17.value_desc",
               "{\"min\":-30000,\"max\":30000,\"scale\":3,\"step\":1,\"unit\":\"mA\"}",
               device.local_strategy[17].value_desc_json);
    CheckTrue(r, "golden.status_range.cur_current",
              device.status_range.count("cur_current") == 1 &&
                  device.status_range["cur_current"].type == "Integer" &&
                  device.status_range["cur_current"].values_json ==
                      "{\"min\":-30000,\"max\":30000,\"scale\":3,\"step\":1,\"unit\":\"mA\"}" &&
                  device.status_range["cur_current"].report_type.empty());

    // dp 19 — add_ele (ro -> status_range).
    CheckTrue(r, "golden.dp19.fields",
              device.local_strategy.count(19) == 1 &&
                  device.local_strategy[19].value_type == "Integer" &&
                  device.local_strategy[19].value_desc_json ==
                      "{\"min\":0,\"max\":1000000,\"scale\":3,\"step\":100,\"unit\":\"kWh\"}");
    CheckTrue(r, "golden.status_range.add_ele",
              device.status_range.count("add_ele") == 1 &&
                  device.status_range["add_ele"].type == "Integer");

    // dp 6 — fault (ro -> status_range, Bitmap label list).
    CheckTrue(r, "golden.dp6.fields",
              device.local_strategy.count(6) == 1 &&
                  device.local_strategy[6].value_type == "Bitmap" &&
                  device.local_strategy[6].use_open_api);
    CheckEqStr(r, "golden.dp6.value_desc", "{\"label\":[\"ov_cr\"]}",
               device.local_strategy[6].value_desc_json);
    CheckTrue(r, "golden.status_range.fault",
              device.status_range.count("fault") == 1 &&
                  device.status_range["fault"].type == "Bitmap" &&
                  device.status_range["fault"].values_json == "{\"label\":[\"ov_cr\"]}");

    // Map sizes: 1 function (rw), 3 status ranges (ro).
    CheckEqInt(r, "golden.function.count", 1, static_cast<long long>(device.function.size()));
    CheckEqInt(r, "golden.status_range.count", 3,
               static_cast<long long>(device.status_range.size()));

    // Shadow status fill: switch_1=true (bool), cur_current=-1896 (int).
    CheckEqInt(r, "golden.status.count", 2, static_cast<long long>(device.status.size()));
    CheckTrue(r, "golden.status.switch_1",
              device.status.count("switch_1") == 1 &&
                  device.status["switch_1"].type == DpValue::Type::kBool &&
                  device.status["switch_1"].b);
    CheckTrue(r, "golden.status.cur_current",
              device.status.count("cur_current") == 1 &&
                  device.status["cur_current"].type == DpValue::Type::kInt &&
                  device.status["cur_current"].i == -1896);

    // Enriched entries route through the strategy engine.
    std::string out_code;
    DpValue out_value;
    CheckTrue(r, "golden.convert.dp17",
              Strategies::Convert(device.local_strategy[17], 17, DpValue::FromInt(-2000), out_code,
                                  out_value, device.status_range) &&
                  out_code == "cur_current" && out_value.type == DpValue::Type::kInt &&
                  out_value.i == -2000);
    CheckTrue(r, "golden.convert.dp1",
              Strategies::Convert(device.local_strategy[1], 1, DpValue::FromBool(false), out_code,
                                  out_value, device.status_range) &&
                  out_code == "switch_1" && out_value.type == DpValue::Type::kBool && !out_value.b);
}

/// The mapping table + malformed rows + multi-service walk.
void TestThingModelMapping(TestResult& r) {
    Device device;
    device.id = "mapping_probe";
    int added = 0;
    {
        JsonPtr model(kThingModelResult2Json);
        added = ParseThingModel(model.root(), device);
    }

    // 5 usable properties (24, "25" and 25.5 are skipped).
    CheckEqInt(r, "mapping.added", 5, added);
    CheckEqInt(r, "mapping.local_strategy.count", 5,
               static_cast<long long>(device.local_strategy.size()));

    // dp 20 — "wr" is writable, propertyUpdate true, Integer desc kept.
    CheckTrue(r, "mapping.dp20",
              device.local_strategy.count(20) == 1 && device.local_strategy[20].use_open_api &&
                  device.local_strategy[20].property_update &&
                  device.local_strategy[20].value_type == "Integer");
    CheckEqStr(r, "mapping.dp20.value_desc",
               "{\"min\":0,\"max\":86400,\"scale\":0,\"step\":1,\"propertyUpdate\":true}",
               device.local_strategy[20].value_desc_json);
    CheckTrue(r, "mapping.function.countdown",
              device.function.count("countdown") == 1 &&
                  device.function["countdown"].type == "Integer");
    // Writable function: the Thing Model "name" beats the code fallback.
    CheckEqStr(r, "mapping.function.countdown.name", "Countdown Timer",
               device.function["countdown"].name);

    // dp 21 — Enum.
    CheckTrue(r, "mapping.dp21_enum",
              device.local_strategy.count(21) == 1 &&
                  device.local_strategy[21].value_type == "Enum" &&
                  device.local_strategy[21].value_desc_json == "{\"range\":[\"1\",\"2\"]}");
    CheckTrue(r, "mapping.function.mode",
              device.function.count("mode") == 1 && device.function["mode"].type == "Enum");

    // dp 22 — String (ro).
    CheckTrue(r, "mapping.dp22_string",
              device.local_strategy.count(22) == 1 &&
                  device.local_strategy[22].value_type == "String" &&
                  device.local_strategy[22].value_desc_json == "{\"maxlen\":255}");
    CheckTrue(r, "mapping.status_range.fault_str",
              device.status_range.count("fault_str") == 1 &&
                  device.status_range["fault_str"].type == "String");

    // dp 23 — unknown "date" type -> Raw fallback.
    CheckTrue(r, "mapping.dp23_raw_fallback",
              device.local_strategy.count(23) == 1 &&
                  device.local_strategy[23].value_type == "Raw" &&
                  device.local_strategy[23].value_desc_json == "{}");
    CheckTrue(r, "mapping.status_range.schedule",
              device.status_range.count("schedule") == 1 &&
                  device.status_range["schedule"].type == "Raw");

    // dp 30 — second service processed, Json type.
    CheckTrue(r, "mapping.dp30_second_service",
              device.local_strategy.count(30) == 1 &&
                  device.local_strategy[30].value_type == "Json" &&
                  device.local_strategy[30].value_desc_json == "{}");
    CheckTrue(r, "mapping.function.second_service_dp",
              device.function.count("second_service_dp") == 1 &&
                  device.function["second_service_dp"].type == "Json");

    // The malformed rows never land anywhere.
    CheckTrue(r, "mapping.broken_no_spec_skipped",
              device.local_strategy.count(24) == 0 && device.function.count("broken_no_spec") == 0);
    CheckTrue(r, "mapping.broken_string_id_skipped",
              device.local_strategy.count(25) == 0 &&
                  device.function.count("broken_string_id") == 0);
    // A fractional abilityId never lands (25.5 must not become dp 25).
    CheckTrue(r, "mapping.fractional_ability_id_skipped",
              device.local_strategy.count(25) == 0 && device.function.count("fractional_id") == 0);

    // Map sizes: 3 function, 2 status_range.
    CheckEqInt(r, "mapping.function.count", 3, static_cast<long long>(device.function.size()));
    CheckEqInt(r, "mapping.status_range.count", 2,
               static_cast<long long>(device.status_range.size()));
}

/// Sharing-wins: pre-populated entries are NEVER overwritten.
void TestSharingWins(TestResult& r) {
    Device device;
    device.id = "partial_sharing";
    // Pre-populated: dp 1 strategy, cur_current range, switch_1 status.
    StrategyConfig sharing_cfg;
    sharing_cfg.dp_id = 1;
    sharing_cfg.value_convert = "enum";
    sharing_cfg.status_code = "switch_1";
    sharing_cfg.enum_mapping["0"] = "off";
    device.local_strategy[1] = sharing_cfg;
    DeviceStatusRange sharing_range;
    sharing_range.code = "cur_current";
    sharing_range.type = "SHARING_TYPE";
    sharing_range.values_json = "SHARING_WINS";
    device.status_range["cur_current"] = sharing_range;
    device.status["switch_1"] = DpValue::FromBool(false);

    int model_added = 0;
    int shadow_added = 0;
    {
        JsonPtr model(kThingModelResultJson);
        model_added = ParseThingModel(model.root(), device);
    }
    {
        JsonPtr shadow(kShadowResultJson);
        shadow_added = ParseShadowProperties(shadow.root(), device);
    }

    // Model: only 3 strategies added (dp 1 stays the sharing enum one).
    CheckEqInt(r, "sharing.model_added", 3, model_added);
    CheckEqInt(r, "sharing.local_strategy.count", 4,
               static_cast<long long>(device.local_strategy.size()));
    CheckTrue(r, "sharing.dp1_strategy_kept",
              device.local_strategy.count(1) == 1 &&
                  device.local_strategy[1].value_convert == "enum" &&
                  device.local_strategy[1].enum_mapping["0"] == "off" &&
                  !device.local_strategy[1].use_open_api);

    // status_range: cur_current keeps the sharing row; add_ele/fault filled.
    CheckTrue(r, "sharing.cur_current_range_kept",
              device.status_range.count("cur_current") == 1 &&
                  device.status_range["cur_current"].type == "SHARING_TYPE" &&
                  device.status_range["cur_current"].values_json == "SHARING_WINS");
    CheckTrue(r, "sharing.add_ele_range_filled",
              device.status_range.count("add_ele") == 1 &&
                  device.status_range["add_ele"].type == "Integer");

    // rw switch_1 fills the EMPTY function map (per-map sharing-wins).
    CheckTrue(r, "sharing.function.switch_1_filled",
              device.function.count("switch_1") == 1 &&
                  device.function["switch_1"].type == "Boolean");

    // Shadow: switch_1 stays false (the device-list value); cur_current filled.
    CheckEqInt(r, "sharing.shadow_added", 0, shadow_added);
    CheckTrue(r, "sharing.status.switch_1_kept",
              device.status.count("switch_1") == 1 &&
                  device.status["switch_1"].type == DpValue::Type::kBool &&
                  !device.status["switch_1"].b);
    CheckTrue(r, "sharing.status.cur_current_filled",
              device.status.count("cur_current") == 1 && device.status["cur_current"].i == -1896);
}

/// The minimal shadow strategy + the malformed-row policy.
void TestShadowMinimalStrategy(TestResult& r) {
    Device device;
    device.id = "shadow_probe";
    int added = 0;
    {
        JsonPtr shadow(kShadowResult2Json);
        added = ParseShadowProperties(shadow.root(), device);
    }

    // dp 9 (with type) gets a minimal strategy; dp 10 (no type) does not.
    CheckEqInt(r, "shadow2.added", 1, added);
    CheckTrue(r, "shadow2.dp9_minimal",
              device.local_strategy.count(9) == 1 && device.local_strategy[9].dp_id == 9 &&
                  device.local_strategy[9].value_convert == "default" &&
                  device.local_strategy[9].status_code == "relay_status" &&
                  device.local_strategy[9].status_format_json == "{\"relay_status\":\"\"}" &&
                  device.local_strategy[9].value_type == "Boolean" &&
                  device.local_strategy[9].value_desc_json == "{}" &&
                  device.local_strategy[9].use_open_api &&
                  device.local_strategy[9].property_update &&
                  !device.local_strategy[9].support_local);
    CheckTrue(r, "shadow2.dp10_no_strategy", device.local_strategy.count(10) == 0);

    // Status: relay_status=false, no_type=42 (filled even without a type).
    CheckEqInt(r, "shadow2.status.count", 2, static_cast<long long>(device.status.size()));
    CheckTrue(r, "shadow2.status.relay_status",
              device.status.count("relay_status") == 1 &&
                  device.status["relay_status"].type == DpValue::Type::kBool &&
                  !device.status["relay_status"].b);
    CheckTrue(r, "shadow2.status.no_type",
              device.status.count("no_type") == 1 &&
                  device.status["no_type"].type == DpValue::Type::kInt &&
                  device.status["no_type"].i == 42);
    CheckTrue(r, "shadow2.no_dp_id_skipped",
              device.status.count("no_dp_id") == 0 && device.status.count("") == 0);

    // The minimal strategy routes: dp 9 converts relay_status.
    std::string out_code;
    DpValue out_value;
    CheckTrue(r, "shadow2.convert.dp9",
              Strategies::Convert(device.local_strategy[9], 9, DpValue::FromBool(true), out_code,
                                  out_value, device.status_range) &&
                  out_code == "relay_status" && out_value.type == DpValue::Type::kBool &&
                  out_value.b);
}

/// Defensive shapes: all yield 0 without touching the device.
void TestMalformedResults(TestResult& r) {
    const char* kCases[] = {
        "null",
        "[]",
        "{}",
        "{\"other\": 1}",
        "{\"model\": \"not json at all\"}",
        "{\"model\": \"{}\"}",
        "{\"model\": 42}",
        "{\"model\": \"[]\"}",
        "{\"properties\": null}",
        "{\"properties\": {}}",
    };
    for (const char* kase : kCases) {
        Device device;
        device.id = "probe";
        device.status["preexisting"] = DpValue::FromInt(7);
        int model_added = 0;
        int shadow_added = 0;
        {
            JsonPtr json(kase);
            model_added = ParseThingModel(json.root(), device);
            shadow_added = ParseShadowProperties(json.root(), device);
        }
        char name[96];
        std::snprintf(name, sizeof(name), "malformed.untouched(%.40s)", kase);
        const bool untouched = device.local_strategy.empty() && device.function.empty() &&
                               device.status_range.empty() &&
                               device.status.count("preexisting") == 1 &&
                               device.status["preexisting"].i == 7;
        CheckTrue(r, name, untouched);
        std::snprintf(name, sizeof(name), "malformed.model_zero(%.40s)", kase);
        CheckEqInt(r, name, 0, model_added);
        std::snprintf(name, sizeof(name), "malformed.shadow_zero(%.40s)", kase);
        CheckEqInt(r, name, 0, shadow_added);
    }
}

}  // namespace

bool RunOpenApiEnrichmentSelfTest() {
    TestResult r;
    TestGoldenFlow(r);
    TestThingModelMapping(r);
    TestSharingWins(r);
    TestShadowMinimalStrategy(r);
    TestMalformedResults(r);

    if (r.passed == r.total) {
        ESP_LOGI(TAG, "TUYA IOT ENRICH SELFTEST PASS (%d checks)", r.total);
        return true;
    }
    ESP_LOGE(TAG, "TUYA IOT ENRICH SELFTEST FAIL (%d/%d)", r.passed, r.total);
    return false;
}

}  // namespace hub_core
