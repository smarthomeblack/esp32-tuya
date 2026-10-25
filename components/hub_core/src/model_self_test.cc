#include "hub_core/model_self_test.h"

#include <cJSON.h>
#include <esp_log.h>

#include <string>
#include <string_view>
#include <vector>

#include "hub_core/device.h"
#include "hub_core/device_registry.h"
#include "hub_core/dp_value.h"
#include "hub_core/parse.h"
#include "hub_core/presence_status.h"
#include "hub_core/strategies.h"
#include "parse_helpers.h"

static const char* TAG = "ModelSelfTest";

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

// Golden fragments — shaped like the real SDK responses.

/// Homes list: numeric ownerId, string ownerId, one broken row.
constexpr const char* kHomesJson = "["
                                   "{\"ownerId\": 123456789012, \"name\": \"Home A\"},"
                                   "{\"ownerId\": \"987654321098\", \"name\": \"Home B\"},"
                                   "{\"name\": \"Broken row (no ownerId)\"}"
                                   "]";

/// Device list: one fully populated device + one minimal device.
constexpr const char* kDeviceListJson =
    "["
    "{"
    "\"id\": \"6cd4a1f9c9abc\", \"name\": \"Bedroom Light\", \"local_key\": \"key001\","
    "\"category\": \"dj\", \"product_id\": \"ltkgwrq1\", \"product_name\": \"Smart LED\","
    "\"model\": \"SL-100\", \"sub\": false, \"uuid\": \"u-001\", \"asset_id\": \"a-001\","
    "\"online\": true, \"icon\": \"icon/1.png\", \"ip\": \"192.168.1.7\","
    "\"time_zone\": \"+07:00\", \"active_time\": 1700000000,"
    "\"create_time\": 1690000000, \"update_time\": 1750000000,"
    "\"status\": ["
    "{\"code\": \"switch_led\", \"value\": true},"
    "{\"code\": \"work_mode\", \"value\": \"white\"},"
    "{\"code\": \"bright_value\", \"value\": 128},"
    "{\"code\": \"temp_value\", \"value\": 25.5},"
    "{\"code\": \"broken_no_value\", \"ignored\": 1}"
    "]"
    "},"
    "{\"id\": \"6cd4a1f9d0\", \"name\": \"Socket\"}"
    "]";

/// Specifications result: {functions, status} with `values` as a JSON string.
constexpr const char* kSpecificationsJson =
    "{"
    "\"functions\": ["
    "{\"code\": \"switch_led\", \"name\": \"Switch\", \"desc\": \"Light switch\","
    " \"type\": \"Boolean\", \"values\": \"{\\\"true\\\":\\\"\\\",\\\"false\\\":\\\"\\\"}\"},"
    "{\"code\": \"bright_value\", \"name\": \"Brightness\", \"desc\": \"brightness\","
    " \"type\": \"Integer\", \"values\": "
    "\"{\\\"min\\\":10,\\\"max\\\":1000,\\\"scale\\\":0,\\\"step\\\":1}\"}"
    "],"
    "\"status\": ["
    "{\"code\": \"switch_led\", \"type\": \"Boolean\","
    " \"values\": \"{\\\"true\\\":\\\"\\\",\\\"false\\\":\\\"\\\"}\"},"
    "{\"code\": \"work_mode\", \"type\": \"Enum\","
    " \"values\": \"{\\\"range\\\":[\\\"white\\\",\\\"colour\\\",\\\"scene\\\",\\\"music\\\"]}\"},"
    "{\"code\": \"bright_value\", \"type\": \"Integer\","
    " \"values\": \"{\\\"min\\\":10,\\\"max\\\":1000,\\\"scale\\\":0,\\\"step\\\":1}\"}"
    "]"
    "}";

/// Status strategy: all DTOs supportLocal=true; the last has a fractional
/// dpId (skipped).
constexpr const char* kStatusStrategyJson =
    "{"
    "\"productKey\": \"ltkgwrq1\","
    "\"dpStatusRelationDTOS\": ["
    "{"
    "\"dpId\": 20, \"supportLocal\": true, \"valueConvert\": \"default\","
    "\"statusCode\": \"switch_led\", \"statusFormat\": \"{\\\"switch_led\\\":\\\"\\\"}\","
    "\"valueDesc\": \"{\\\"true\\\":\\\"\\\",\\\"false\\\":\\\"\\\"}\","
    "\"valueType\": \"Boolean\", \"enumMappingMap\": {}"
    "},"
    "{"
    "\"dpId\": 61, \"supportLocal\": true, \"valueConvert\": \"enum\","
    "\"statusCode\": \"work_mode\", \"statusFormat\": \"{\\\"work_mode\\\":\\\"\\\"}\","
    "\"valueDesc\": \"{\\\"range\\\":[\\\"white\\\",\\\"colour\\\",\\\"scene\\\",\\\"music\\\"]}\","
    "\"valueType\": \"Enum\","
    "\"enumMappingMap\": {\"0\": {\"value\": \"white\"}, \"1\": {\"value\": \"colour\"},"
    " \"2\": {\"value\": \"scene\"}, \"9\": {}}"
    "},"
    "{"
    "\"dpId\": 22, \"supportLocal\": true, \"valueConvert\": \"default\","
    "\"statusCode\": \"bright_value\", \"statusFormat\": \"{\\\"bright_value\\\":\\\"\\\"}\","
    "\"valueDesc\": \"{\\\"min\\\":10,\\\"max\\\":1000,\\\"scale\\\":0,\\\"step\\\":1}\","
    "\"valueType\": \"Integer\", \"enumMappingMap\": {}"
    "},"
    "{"
    "\"dpId\": 1.5, \"supportLocal\": true, \"valueConvert\": \"default\","
    "\"statusCode\": \"fractional_dp\", \"statusFormat\": \"{\\\"fractional_dp\\\":\\\"\\\"}\","
    "\"valueDesc\": \"{}\", \"valueType\": \"Boolean\", \"enumMappingMap\": {}"
    "}"
    "]"
    "}";

/// Variant with ONE DTO supportLocal=false: the device flips to
/// support_local=false with an empty local_strategy.
constexpr const char* kStatusStrategyNotLocalJson =
    "{"
    "\"productKey\": \"px\","
    "\"dpStatusRelationDTOS\": ["
    "{"
    "\"dpId\": 1, \"supportLocal\": true, \"valueConvert\": \"default\","
    "\"statusCode\": \"a\", \"statusFormat\": \"{\\\"a\\\":\\\"\\\"}\","
    "\"valueDesc\": \"{}\", \"valueType\": \"Boolean\", \"enumMappingMap\": {}"
    "},"
    "{"
    "\"dpId\": 2, \"supportLocal\": false, \"valueConvert\": \"default\","
    "\"statusCode\": \"b\", \"statusFormat\": \"{\\\"b\\\":\\\"\\\"}\","
    "\"valueDesc\": \"{}\", \"valueType\": \"Boolean\", \"enumMappingMap\": {}"
    "},"
    "{"
    "\"dpId\": 3, \"supportLocal\": true, \"valueConvert\": \"default\","
    "\"statusCode\": \"c\", \"statusFormat\": \"{\\\"c\\\":\\\"\\\"}\","
    "\"valueDesc\": \"{}\", \"valueType\": \"Boolean\", \"enumMappingMap\": {}"
    "}"
    "]"
    "}";

/// dp report types result: "minux" is the protocol typo (stored verbatim).
constexpr const char* kDpReportTypesJson =
    "["
    "{\"dp_code\": \"bright_value\", \"report_type\": \"minux\"},"
    "{\"dp_code\": \"switch_led\", \"report_type\": \"sum\"},"
    "{\"dp_code\": \"unknown_code\", \"report_type\": \"sum\"},"
    "{\"dp_code\": \"\", \"report_type\": \"un_known\"}"
    "]";

// Test sections

void TestParseHomes(TestResult& r) {
    std::vector<Home> homes;
    const bool ok = [&] {
        JsonPtr json(kHomesJson);
        return ParseHomes(json.root(), homes);
    }();
    CheckTrue(r, "homes.parse_ok", ok);
    CheckEqInt(r, "homes.count", 2, static_cast<long long>(homes.size()));
    if (homes.size() == 2) {
        CheckEqStr(r, "homes.0.id_numeric_ownerid", "123456789012", homes[0].id);
        CheckEqStr(r, "homes.0.name", "Home A", homes[0].name);
        CheckEqStr(r, "homes.1.id_string_ownerid", "987654321098", homes[1].id);
        CheckEqStr(r, "homes.1.name", "Home B", homes[1].name);
    }
}

void TestParseDevice(TestResult& r) {
    Device full;
    Device minimal;
    bool full_ok = false;
    bool minimal_ok = false;
    {
        JsonPtr json(kDeviceListJson);
        if (cJSON_IsArray(json.root())) {
            cJSON* first = json.root()->child;
            cJSON* second = first != nullptr ? first->next : nullptr;
            full_ok = ParseDevice(first, full);
            minimal_ok = ParseDevice(second, minimal);
        }
    }
    CheckTrue(r, "device.full_ok", full_ok);
    CheckEqStr(r, "device.full.id", "6cd4a1f9c9abc", full.id);
    CheckEqStr(r, "device.full.name", "Bedroom Light", full.name);
    CheckEqStr(r, "device.full.category", "dj", full.category);
    CheckEqStr(r, "device.full.product_id", "ltkgwrq1", full.product_id);
    CheckEqStr(r, "device.full.product_name", "Smart LED", full.product_name);
    CheckEqStr(r, "device.full.model", "SL-100", full.model);
    CheckTrue(r, "device.full.online", full.online);
    CheckTrue(r, "device.full.not_sub", !full.sub);
    CheckEqInt(r, "device.full.active_time", 1700000000, full.active_time);
    CheckEqInt(r, "device.full.create_time", 1690000000, full.create_time);
    CheckEqInt(r, "device.full.update_time", 1750000000, full.update_time);

    // status list -> map: 4 of 5 rows (the one without "value" is skipped).
    CheckEqInt(r, "device.full.status.count", 4, static_cast<long long>(full.status.size()));
    CheckTrue(r, "device.full.status.switch_led",
              full.status.count("switch_led") == 1 &&
                  full.status["switch_led"].type == DpValue::Type::kBool &&
                  full.status["switch_led"].b);
    CheckTrue(r, "device.full.status.work_mode",
              full.status.count("work_mode") == 1 &&
                  full.status["work_mode"].type == DpValue::Type::kString &&
                  full.status["work_mode"].s == "white");
    CheckTrue(r, "device.full.status.bright_value",
              full.status.count("bright_value") == 1 &&
                  full.status["bright_value"].type == DpValue::Type::kInt &&
                  full.status["bright_value"].i == 128);
    CheckTrue(r, "device.full.status.temp_value",
              full.status.count("temp_value") == 1 &&
                  full.status["temp_value"].type == DpValue::Type::kDouble &&
                  full.status["temp_value"].d > 25.4 && full.status["temp_value"].d < 25.6);
    CheckTrue(r, "device.full.status.dropped_broken_row",
              full.status.count("broken_no_value") == 0);

    // Defaults on a minimal row.
    CheckTrue(r, "device.minimal_ok", minimal_ok);
    CheckEqStr(r, "device.minimal.id", "6cd4a1f9d0", minimal.id);
    CheckEqStr(r, "device.minimal.name", "Socket", minimal.name);
    CheckTrue(r, "device.minimal.offline_default", !minimal.online);
    CheckTrue(r, "device.minimal.support_local_default", !minimal.support_local);
    CheckTrue(r, "device.minimal.set_up_default", !minimal.set_up);
    CheckEqInt(r, "device.minimal.empty_status", 0, static_cast<long long>(minimal.status.size()));
    CheckEqInt(r, "device.minimal.empty_category", 0,
               static_cast<long long>(minimal.category.size()));
}

/// Parses the full discovery chain for one device (as RunDiscovery does).
Device BuildLightDevice(TestResult& r) {
    Device device;
    {
        JsonPtr list(kDeviceListJson);
        ParseDevice(list.root()->child, device);
    }
    {
        JsonPtr specs(kSpecificationsJson);
        ParseSpecifications(specs.root(), device);
    }
    {
        JsonPtr strategy(kStatusStrategyJson);
        ParseStatusStrategy(strategy.root(), device);
    }
    {
        JsonPtr reports(kDpReportTypesJson);
        ParseDpReportTypes(reports.root(), device);
    }
    return device;
}

void TestParseSpecifications(TestResult& r) {
    Device device = BuildLightDevice(r);

    CheckEqInt(r, "specs.functions.count", 2, static_cast<long long>(device.function.size()));
    CheckTrue(r, "specs.functions.switch_led",
              device.function.count("switch_led") == 1 &&
                  device.function["switch_led"].name == "Switch" &&
                  device.function["switch_led"].desc == "Light switch" &&
                  device.function["switch_led"].type == "Boolean");
    CheckTrue(r, "specs.functions.values_json_verbatim",
              device.function.count("bright_value") == 1 &&
                  device.function["bright_value"].values_json ==
                      "{\"min\":10,\"max\":1000,\"scale\":0,\"step\":1}");

    CheckEqInt(r, "specs.status.count", 3, static_cast<long long>(device.status_range.size()));
    CheckTrue(r, "specs.status.work_mode",
              device.status_range.count("work_mode") == 1 &&
                  device.status_range["work_mode"].type == "Enum" &&
                  device.status_range["work_mode"].values_json ==
                      "{\"range\":[\"white\",\"colour\",\"scene\",\"music\"]}");

    // "minux" stored verbatim; unknown/empty codes ignored.
    CheckTrue(r, "reports.bright_value_minux",
              device.status_range.count("bright_value") == 1 &&
                  device.status_range["bright_value"].report_type == "minux");
    CheckTrue(r, "reports.switch_led_sum",
              device.status_range.count("switch_led") == 1 &&
                  device.status_range["switch_led"].report_type == "sum");
    CheckTrue(r, "reports.work_mode_unset", device.status_range["work_mode"].report_type.empty());
}

void TestParseStatusStrategy(TestResult& r) {
    Device device = BuildLightDevice(r);
    CheckTrue(r, "strategy.support_local_true", device.support_local);
    CheckEqInt(r, "strategy.local_strategy.count", 3,
               static_cast<long long>(device.local_strategy.size()));
    CheckTrue(r, "strategy.dp20_default",
              device.local_strategy.count(20) == 1 &&
                  device.local_strategy[20].value_convert == "default" &&
                  device.local_strategy[20].status_code == "switch_led" &&
                  device.local_strategy[20].status_format_json == "{\"switch_led\":\"\"}" &&
                  device.local_strategy[20].value_type == "Boolean");
    CheckTrue(r, "strategy.dp61_enum_mapping",
              device.local_strategy.count(61) == 1 &&
                  device.local_strategy[61].enum_mapping.size() == 3 &&
                  device.local_strategy[61].enum_mapping["0"] == "white" &&
                  device.local_strategy[61].enum_mapping["1"] == "colour" &&
                  device.local_strategy[61].enum_mapping["2"] == "scene");
    CheckTrue(r, "strategy.dp61_valueless_entry_omitted",
              device.local_strategy[61].enum_mapping.count("9") == 0);
    CheckTrue(r, "strategy.dp22_value_desc",
              device.local_strategy.count(22) == 1 &&
                  device.local_strategy[22].value_desc_json ==
                      "{\"min\":10,\"max\":1000,\"scale\":0,\"step\":1}");
    // A fractional dpId never lands (1.5 must not become key 1).
    CheckTrue(r, "strategy.fractional_dpid_skipped", device.local_strategy.count(1) == 0);

    // The not-local variant: one false DTO flips the device AND empties the map.
    Device not_local;
    {
        JsonPtr strategy(kStatusStrategyNotLocalJson);
        ParseStatusStrategy(strategy.root(), not_local);
    }
    CheckTrue(r, "strategy.not_local.support_local_false", !not_local.support_local);
    CheckEqInt(r, "strategy.not_local.empty_map", 0,
               static_cast<long long>(not_local.local_strategy.size()));

    // No usable dpStatusRelationDTOS => unknown, never a false local claim.
    Device absent;
    {
        JsonPtr strategy("{\"productKey\": \"px\"}");
        ParseStatusStrategy(strategy.root(), absent);
    }
    CheckTrue(r, "strategy.absent.support_local_false", !absent.support_local);
    CheckEqInt(r, "strategy.absent.empty_map", 0,
               static_cast<long long>(absent.local_strategy.size()));

    Device non_array;
    {
        JsonPtr strategy("{\"dpStatusRelationDTOS\": {\"dpId\": 1}}");
        ParseStatusStrategy(strategy.root(), non_array);
    }
    CheckTrue(r, "strategy.non_array.support_local_false", !non_array.support_local);
    CheckEqInt(r, "strategy.non_array.empty_map", 0,
               static_cast<long long>(non_array.local_strategy.size()));

    Device empty_array;
    {
        JsonPtr strategy("{\"dpStatusRelationDTOS\": []}");
        ParseStatusStrategy(strategy.root(), empty_array);
    }
    CheckTrue(r, "strategy.empty_array.support_local_false", !empty_array.support_local);
    CheckEqInt(r, "strategy.empty_array.empty_map", 0,
               static_cast<long long>(empty_array.local_strategy.size()));
}

void TestStrategies(TestResult& r) {
    Device device = BuildLightDevice(r);
    std::string code;
    DpValue value;

    // --- enum strategy ---
    const StrategyConfig& enum_cfg = device.local_strategy[61];
    CheckTrue(r, "convert.enum.hit",
              Strategies::Convert(enum_cfg, 61, DpValue::FromString("2"), code, value,
                                  device.status_range) &&
                  code == "work_mode" && value.type == DpValue::Type::kString &&
                  value.s == "scene");
    CheckTrue(
        r, "convert.enum.int_key_hit",
        Strategies::Convert(enum_cfg, 61, DpValue::FromInt(1), code, value, device.status_range) &&
            code == "work_mode" && value.s == "colour");
    CheckTrue(r, "convert.enum.miss_falls_to_default",
              Strategies::Convert(enum_cfg, 61, DpValue::FromString("9"), code, value,
                                  device.status_range) &&
                  code == "work_mode" && value.s == "white");
    CheckTrue(r, "convert.enum.empty_raw_falls_to_default",
              Strategies::Convert(enum_cfg, 61, DpValue::FromString(""), code, value,
                                  device.status_range) &&
                  code == "work_mode" && value.s == "white");

    // --- default strategy ---
    const StrategyConfig& bool_cfg = device.local_strategy[20];
    CheckTrue(r, "convert.default.bool_passthrough",
              Strategies::Convert(bool_cfg, 20, DpValue::FromBool(true), code, value,
                                  device.status_range) &&
                  code == "switch_led" && value.type == DpValue::Type::kBool && value.b);
    CheckTrue(r, "convert.default.bool_fallback_false",
              Strategies::Convert(bool_cfg, 20, DpValue::FromString(""), code, value,
                                  device.status_range) &&
                  code == "switch_led" && value.type == DpValue::Type::kBool && !value.b);
    const StrategyConfig& int_cfg = device.local_strategy[22];
    CheckTrue(
        r, "convert.default.int_passthrough",
        Strategies::Convert(int_cfg, 22, DpValue::FromInt(42), code, value, device.status_range) &&
            code == "bright_value" && value.type == DpValue::Type::kInt && value.i == 42);
    CheckTrue(r, "convert.default.int_fallback_min",
              Strategies::Convert(int_cfg, 22, DpValue::FromString(""), code, value,
                                  device.status_range) &&
                  code == "bright_value" && value.type == DpValue::Type::kInt && value.i == 10);

    // --- Enum range gate: a mapped value NOT in the range -> drop ---
    StrategyConfig gate_cfg = enum_cfg;
    gate_cfg.enum_mapping["5"] = "disco";  // "disco" is not in range
    CheckTrue(r, "convert.gate.drop_out_of_range",
              !Strategies::Convert(gate_cfg, 61, DpValue::FromString("5"), code, value,
                                   device.status_range));
    // Code without a status_range entry -> no gate -> keep.
    StrategyConfig no_range_cfg = enum_cfg;
    no_range_cfg.status_format_json = "{\"unknown_code\":\"\"}";
    CheckTrue(r, "convert.gate.missing_range_keeps",
              Strategies::Convert(no_range_cfg, 61, DpValue::FromString("2"), code, value,
                                  device.status_range) &&
                  code == "unknown_code" && value.s == "scene");
    // Non-Enum status_range type -> no gate -> keep.
    StrategyConfig int_no_gate = int_cfg;
    int_no_gate.status_format_json = "{\"bright_value\":\"\"}";
    CheckTrue(r, "convert.gate.integer_not_gated",
              Strategies::Convert(int_no_gate, 22, DpValue::FromInt(5000), code, value,
                                  device.status_range) &&
                  code == "bright_value" && value.i == 5000);
    // Unparsable values_json -> warn + keep.
    Device broken_range_device = device;
    broken_range_device.status_range["work_mode"].values_json = "not json";
    CheckTrue(r, "convert.gate.unparsable_values_keeps",
              Strategies::Convert(enum_cfg, 61, DpValue::FromString("2"), code, value,
                                  broken_range_device.status_range) &&
                  value.s == "scene");

    // --- Drop paths: unknown strategy name -> drop ---
    StrategyConfig unknown_cfg = enum_cfg;
    unknown_cfg.value_convert = "cz_timer1_alg";  // not implemented
    CheckTrue(r, "convert.unknown_strategy_drops",
              !Strategies::Convert(unknown_cfg, 61, DpValue::FromString("2"), code, value,
                                   device.status_range));
    // Malformed statusFormat -> drop.
    StrategyConfig bad_format_cfg = enum_cfg;
    bad_format_cfg.status_format_json = "not json";
    CheckTrue(r, "convert.bad_statusformat_drops",
              !Strategies::Convert(bad_format_cfg, 61, DpValue::FromString("2"), code, value,
                                   device.status_range));
    // Enum valueDesc without a range -> drop.
    StrategyConfig no_range_desc_cfg = enum_cfg;
    no_range_desc_cfg.value_desc_json = "{}";
    CheckTrue(r, "convert.enum_fallback_without_range_drops",
              !Strategies::Convert(no_range_desc_cfg, 61, DpValue::FromString("9"), code, value,
                                   device.status_range));

    // --- DpValue helpers ---
    CheckEqStr(r, "dpvalue.tostring.bool", "true", DpValue::FromBool(true).ToString());
    CheckEqStr(r, "dpvalue.tostring.int", "128", DpValue::FromInt(128).ToString());
    CheckEqStr(r, "dpvalue.tostring.string", "\"white\"", DpValue::FromString("white").ToString());
    CheckEqStr(r, "dpvalue.keystring.bool", "true", DpValue::FromBool(true).AsKeyString());
    CheckEqStr(r, "dpvalue.keystring.int", "7", DpValue::FromInt(7).AsKeyString());
    CheckEqStr(r, "dpvalue.keystring.string", "white", DpValue::FromString("white").AsKeyString());
    CheckTrue(r, "dpvalue.null_or_empty",
              DpValue::FromString("").IsNullOrEmpty() &&
                  !DpValue::FromBool(false).IsNullOrEmpty() &&
                  !DpValue::FromInt(0).IsNullOrEmpty());
    // ParseDpValue typing (JSON null -> empty string; integral -> Int).
    {
        JsonPtr json("{\"b\": true, \"i\": 120, \"f\": 2.5, \"s\": \"x\", \"n\": null}");
        CheckTrue(r, "dpvalue.parse.bool",
                  ParseDpValue(cJSON_GetObjectItemCaseSensitive(json.root(), "b")).type ==
                      DpValue::Type::kBool);
        CheckTrue(r, "dpvalue.parse.int",
                  ParseDpValue(cJSON_GetObjectItemCaseSensitive(json.root(), "i")).type ==
                      DpValue::Type::kInt);
        CheckTrue(r, "dpvalue.parse.double",
                  ParseDpValue(cJSON_GetObjectItemCaseSensitive(json.root(), "f")).type ==
                      DpValue::Type::kDouble);
        CheckTrue(r, "dpvalue.parse.string",
                  ParseDpValue(cJSON_GetObjectItemCaseSensitive(json.root(), "s")).type ==
                      DpValue::Type::kString);
        CheckTrue(r, "dpvalue.parse.null",
                  ParseDpValue(cJSON_GetObjectItemCaseSensitive(json.root(), "n")).IsNullOrEmpty());
        CheckTrue(r, "dpvalue.parse.absent", ParseDpValue(nullptr).IsNullOrEmpty());
    }
}

void TestRegistry(TestResult& r) {
    DeviceRegistry& registry = DeviceRegistry::GetInstance();

    // Event capture: record payloads only.
    std::vector<RegistryEventPayload> events;
    registry.SetEventListener([&events](const RegistryEventPayload& ev) { events.push_back(ev); });

    registry.ClearAll();
    CheckEqInt(r, "registry.empty_after_clear", 0, static_cast<long long>(registry.Count()));

    Device device = BuildLightDevice(r);
    registry.SetHomes({{"123456789012", "Home A", {}}, {"987654321098", "Home B", {}}});
    CheckEqInt(r, "registry.homes_count", 2, static_cast<long long>(registry.GetHomes().size()));

    registry.UpsertDevice(device);
    CheckEqInt(r, "registry.count_after_upsert", 1, static_cast<long long>(registry.Count()));

    Device out;
    CheckTrue(r, "registry.getdevice", registry.GetDevice(device.id, out));
    CheckEqStr(r, "registry.getdevice.name", "Bedroom Light", out.name);

    // UpdateStatus + dp timestamp.
    CheckTrue(
        r, "registry.update_status",
        registry.UpdateStatus(device.id, "switch_led", DpValue::FromBool(false), 1752456620499));
    CheckTrue(r, "registry.update_status_unknown_dev",
              !registry.UpdateStatus("nope", "switch_led", DpValue::FromBool(false), 1));
    CheckTrue(r, "registry.getdevice.updated_value",
              registry.GetDevice(device.id, out) && !out.status["switch_led"].b);
    CheckEqInt(r, "registry.dp_timestamp_stored", 1752456620499,
               out.dp_timestamps.count("switch_led") == 1 ? out.dp_timestamps["switch_led"] : 0);
    // t == 0 must NOT create/overwrite a timestamp.
    registry.UpdateStatus(device.id, "switch_led", DpValue::FromBool(true), 0);
    registry.GetDevice(device.id, out);
    CheckEqInt(r, "registry.dp_timestamp_zero_ignored", 1752456620499,
               out.dp_timestamps.count("switch_led") == 1 ? out.dp_timestamps["switch_led"] : 0);

    // Re-discovery replaces fully (no merge).
    Device replacement;
    replacement.id = device.id;
    replacement.name = "Renamed Light";
    registry.UpsertDevice(replacement);
    registry.GetDevice(device.id, out);
    CheckEqStr(r, "registry.upsert_replaces", "Renamed Light", out.name);
    CheckEqInt(r, "registry.upsert_replaces_status", 0, static_cast<long long>(out.status.size()));

    // SetOnline fires only on change; RenameDevice reports update.
    events.clear();
    CheckTrue(r, "registry.set_online", registry.SetOnline(device.id, true));
    CheckTrue(r, "registry.set_online_noop", registry.SetOnline(device.id, true));
    CheckTrue(r, "registry.rename", registry.RenameDevice(device.id, "Kitchen Light"));
    // No-op save: same-name rename and an unchanged entity-enable stay
    // silent (the count pins below must not move) but still return true.
    CheckTrue(r, "registry.rename_noop", registry.RenameDevice(device.id, "Kitchen Light"));
    CheckTrue(r, "registry.set_entity_enabled_noop",
              registry.SetEntityEnabled(device.id, "switch_led", true));
    registry.GetDevice(device.id, out);
    CheckTrue(r, "registry.online_flag", out.online);
    CheckEqInt(r, "registry.online_rename_events", 2, static_cast<long long>(events.size()));

    CheckTrue(r, "registry.remove", registry.RemoveDevice(device.id));
    CheckTrue(r, "registry.remove_missing", !registry.RemoveDevice(device.id));
    CheckEqInt(r, "registry.count_after_remove", 0, static_cast<long long>(registry.Count()));

    // Event kinds observed during the above sequence.
    int added = 0, updated = 0, removed = 0, online = 0, homes = 0, cleared = 0;
    for (const RegistryEventPayload& ev : events) {
        switch (ev.event) {
            case RegistryEvent::kDeviceAdded: ++added; break;
            case RegistryEvent::kDeviceUpdated: ++updated; break;
            case RegistryEvent::kDeviceRemoved: ++removed; break;
            case RegistryEvent::kDeviceOnlineChanged: ++online; break;
            case RegistryEvent::kHomesUpdated: ++homes; break;
            case RegistryEvent::kRoomsUpdated: break;  // pinned by TestRegistryRooms
            case RegistryEvent::kDevicesCleared: ++cleared; break;
        }
    }
    CheckEqInt(r, "registry.events.online", 1, online);
    CheckEqInt(r, "registry.events.updated", 1, updated);
    CheckEqInt(r, "registry.events.removed", 1, removed);
    CheckEqInt(r, "registry.events.added", 0, added);
    CheckEqInt(r, "registry.events.homes", 0, homes);
    CheckEqInt(r, "registry.events.cleared", 0, cleared);
    // UpdateStatus events carry the changed code list.
    events.clear();
    registry.UpsertDevice(device);
    registry.UpdateStatus(device.id, "switch_led", DpValue::FromBool(false), 0);
    bool code_in_payload = false;
    for (const RegistryEventPayload& ev : events) {
        if (ev.event == RegistryEvent::kDeviceUpdated && ev.dev_id == std::string_view(device.id) &&
            ev.changed_codes.size() == 1 && ev.changed_codes[0] == "switch_led") {
            code_in_payload = true;
        }
    }
    CheckTrue(r, "registry.events.changed_codes", code_in_payload);

    // Dedup: an identical re-report fires no event (the timestamp still
    // records); a value change, a new key, or a type flip each fire one.
    events.clear();
    registry.UpdateStatus(device.id, "switch_led", DpValue::FromBool(false), 4242);
    int dedup_events = 0;
    for (const RegistryEventPayload& ev : events) {
        if (ev.event == RegistryEvent::kDeviceUpdated) {
            ++dedup_events;
        }
    }
    CheckEqInt(r, "registry.dedup.identical_no_event", 0, dedup_events);
    registry.GetDevice(device.id, out);
    CheckEqInt(r, "registry.dedup.timestamp_still_recorded", 4242,
               out.dp_timestamps.count("switch_led") == 1 ? out.dp_timestamps["switch_led"] : 0);
    registry.UpdateStatus(device.id, "switch_led", DpValue::FromBool(true), 0);
    registry.UpdateStatus(device.id, "bright_new", DpValue::FromInt(7), 0);
    registry.UpdateStatus(device.id, "bright_new", DpValue::FromString("7"), 0);
    dedup_events = 0;
    for (const RegistryEventPayload& ev : events) {
        if (ev.event == RegistryEvent::kDeviceUpdated) {
            ++dedup_events;
        }
    }
    CheckEqInt(r, "registry.dedup.change_new_key_type_fire", 3, dedup_events);

    // Detach the listener (the test lambda capture would dangle).
    registry.SetEventListener(nullptr);
    registry.ClearAll();
}

/// Live Tuya IoT captures (privileged qa/probe_rooms_*.js runs, result subtree
/// verbatim): a room list nested in the home object, and the room's devices.
constexpr const char* kRoomsHomeJson =
    "{\"geo_name\":\"P. Thanh Sơn\",\"home_id\":263448032,\"lat\":\"21.04\","
    "\"lon\":\"106.74\",\"name\":\"Nhà của tôi …\","
    "\"rooms\":[{\"name\":\"Phòng Làm Việc\","
    "\"room_id\":178870394}]}";
constexpr const char* kRoomDevicesJson =
    "[{\"active_time\":1792317228,\"biz_type\":18,\"category\":\"cz\","
    "\"create_time\":1757178719,\"icon\":\"smart/icon/ay15317909071177zKnI/"
    "07f8ae3d4c8f57451521a88005f8c382.png\",\"id\":\"eb591ca6f47192bc1danki\","
    "\"ip\":\"222.252.188.38\",\"lat\":\"21.04\",\"local_key\":\"?5H_o\\u003eM$v8*`)`OJ\","
    "\"lon\":\"106.74\",\"model\":\"Ổ cắm đơn thông minh V2\",\"name\":\"ổ quạt\","
    "\"online\":true,"
    "\"owner_id\":\"263448032\",\"product_id\":\"rpvbmmhxjekuildw\",\"product_name\":\""
    "Ổ cắm đơn thông minh V2\","
    "\"status\":[{\"code\":\"switch_1\",\"value\":true},{\"code\":\"countdown_1\","
    "\"value\":0},{\"code\":\"relay_status\",\"value\":\"last\"}],\"sub\":false,"
    "\"time_zone\":\"+07:00\",\"uid\":\"az1757064314079LqZel\",\"update_time\":1792585065,"
    "\"uuid\":\"ecd871cc65440e4c\"}]";

void TestParseRooms(TestResult& r) {
    std::vector<Room> rooms;
    const bool ok = [&rooms]() {
        JsonPtr json(kRoomsHomeJson);
        return ParseRooms(json.root(), rooms);
    }();
    CheckTrue(r, "rooms.parse_ok", ok);
    CheckEqInt(r, "rooms.count", 1, static_cast<long long>(rooms.size()));
    if (rooms.size() == 1) {
        // The live room_id is a JSON NUMBER: it must be stringified, not dropped.
        CheckEqStr(r, "rooms.0.id_numeric", "178870394", rooms[0].id);
        CheckEqStr(r, "rooms.0.name_utf8", "Phòng Làm Việc", rooms[0].name);
        CheckEqInt(r, "rooms.0.sort_absent", 0, rooms[0].sort);
    }

    // String ids, a bare rows array and an id-less row are all tolerated.
    std::vector<Room> extra;
    CheckTrue(r, "rooms.string_id_and_bare", [&extra]() {
        JsonPtr json("[{\"room_id\":\"7\",\"name\":\"Bed room\",\"sort\":2},{\"name\":\"no id\"}]");
        if (!ParseRooms(json.root(), extra)) {
            return false;
        }
        JsonPtr bare(kRoomsHomeJson);
        const cJSON* list = cJSON_GetObjectItemCaseSensitive(bare.root(), "rooms");
        return ParseRooms(list, extra);  // a bare list appended after the string-id rows
    }());
    CheckEqInt(r, "rooms.mixed_count", 2, static_cast<long long>(extra.size()));
    if (extra.size() == 2) {
        CheckEqStr(r, "rooms.string_id", "7", extra[0].id);
        CheckEqInt(r, "rooms.string_id_sort", 2, extra[0].sort);
        CheckEqStr(r, "rooms.append_keeps_earlier", "178870394", extra[1].id);
    }
    CheckTrue(
        r, "rooms.object_without_list", ![]() {
            JsonPtr json("{\"home_id\":\"263448032\"}");
            std::vector<Room> none;
            return ParseRooms(json.root(), none);
        }());
    CheckTrue(
        r, "rooms.not_a_list", ![]() {
            JsonPtr json("\"nope\"");
            std::vector<Room> none;
            return ParseRooms(json.root(), none);
        }());

    std::vector<Room> from_text;
    CheckTrue(r, "rooms.text_ok", ParseRoomsJson(kRoomsHomeJson, from_text));
    CheckEqInt(r, "rooms.text_count", 1, static_cast<long long>(from_text.size()));
}

void TestParseRoomDevices(TestResult& r) {
    std::vector<std::string> ids;
    const bool ok = [&ids]() {
        JsonPtr json(kRoomDevicesJson);
        return ParseRoomDevices(json.root(), ids);
    }();
    CheckTrue(r, "room_devices.parse_ok", ok);
    // Full device objects joined by "id" — the room holds exactly one device.
    CheckEqInt(r, "room_devices.count", 1, static_cast<long long>(ids.size()));
    if (ids.size() == 1) {
        CheckEqStr(r, "room_devices.0", "eb591ca6f47192bc1danki", ids[0]);
    }
    // Bare id strings, duplicates and id-less rows: counted once, dropped otherwise.
    std::vector<std::string> bare;
    CheckTrue(r, "room_devices.bare_ids", [&bare]() {
        JsonPtr json("[\"a1\",\"b2\",\"a1\",{\"name\":\"no id\"},7]");
        return ParseRoomDevices(json.root(), bare);
    }());
    CheckEqInt(r, "room_devices.bare_count", 2, static_cast<long long>(bare.size()));
    CheckTrue(r, "room_devices.empty_list", [&bare]() {
        JsonPtr json("[]");
        return ParseRoomDevices(json.root(), bare) && bare.size() == 2;
    }());
    CheckTrue(
        r, "room_devices.not_a_list", ![]() {
            JsonPtr json("{}");
            std::vector<std::string> none;
            return ParseRoomDevices(json.root(), none);
        }());

    std::vector<std::string> from_text;
    CheckTrue(r, "room_devices.text_ok", ParseRoomDevicesJson(kRoomDevicesJson, from_text));
    CheckEqInt(r, "room_devices.text_count", 1, static_cast<long long>(from_text.size()));
}

void TestRegistryRooms(TestResult& r) {
    DeviceRegistry& registry = DeviceRegistry::GetInstance();
    std::vector<RegistryEventPayload> events;
    registry.SetEventListener([&events](const RegistryEventPayload& ev) { events.push_back(ev); });

    registry.ClearAll();
    registry.SetHomes({{"123456789012", "Home A", {}}, {"987654321098", "Home B", {}}});
    Device a = BuildLightDevice(r);
    a.home_id = "123456789012";
    registry.UpsertDevice(a);
    Device b;
    b.id = "b-device";
    b.name = "Second";
    b.home_id = "123456789012";
    registry.UpsertDevice(b);
    Device other;
    other.id = "c-device";
    other.name = "Other home";
    other.home_id = "987654321098";
    registry.UpsertDevice(other);

    // SetRooms fires once per real change; a steady-state refresh stays silent.
    const std::vector<Room> rooms = {{"178870394", "Phòng Làm Việc", 1},
                                     {"178870395", "Bedroom", 2}};
    events.clear();
    registry.SetRooms("123456789012", rooms);
    CheckEqInt(r, "registry.rooms.set_events", 1, static_cast<long long>(events.size()));
    CheckTrue(r, "registry.rooms.set_payload",
              !events.empty() && events[0].event == RegistryEvent::kRoomsUpdated &&
                  events[0].home_id == "123456789012" && events[0].dev_id.empty());
    CheckTrue(r, "registry.rooms.stored_home", [&registry, &rooms]() {
        for (const Home& home : registry.GetHomes()) {
            if (std::string_view(home.id) == "123456789012") {
                return home.rooms.size() == rooms.size() &&
                       std::string_view(home.rooms[0].id) == "178870394" &&
                       std::string_view(home.rooms[0].name) == std::string_view(rooms[0].name) &&
                       home.rooms[1].sort == 2;
            }
        }
        return false;
    }());
    events.clear();
    registry.SetRooms("123456789012", rooms);
    CheckEqInt(r, "registry.rooms.unchanged_no_event", 0, static_cast<long long>(events.size()));
    registry.SetRooms("nope", rooms);
    CheckEqInt(r, "registry.rooms.unknown_home_no_event", 0, static_cast<long long>(events.size()));

    // Both devices start unassigned, so the first sweep changes one of them.
    events.clear();
    registry.AssignDeviceRooms("123456789012", {{"178870395", "no-such-device"}});
    CheckEqInt(r, "registry.rooms.assign_initial", 0, static_cast<long long>(events.size()));
    // One event per CHANGED device (a room change only — test b gets its own).
    events.clear();
    registry.AssignDeviceRooms("123456789012", {{"178870394", std::string(a.id)}});
    int updates = 0;
    bool payload_ok = true;
    for (const RegistryEventPayload& ev : events) {
        if (ev.event == RegistryEvent::kDeviceUpdated) {
            ++updates;
            payload_ok = payload_ok && ev.changed_codes.empty() && ev.dev_id == std::string(a.id);
        }
    }
    CheckEqInt(r, "registry.rooms.assign_events", 1, updates);
    CheckTrue(r, "registry.rooms.assign_payload", payload_ok);
    Device out;
    CheckTrue(r, "registry.rooms.assign_a",
              registry.GetDevice(a.id, out) && std::string_view(out.room_id) == "178870394");
    CheckTrue(r, "registry.rooms.assign_b_unassigned",
              registry.GetDevice("b-device", out) && out.room_id.empty());
    CheckTrue(r, "registry.rooms.assign_other_home_untouched",
              registry.GetDevice("c-device", out) && out.room_id.empty());

    // An identical sweep fires nothing.
    events.clear();
    registry.AssignDeviceRooms("123456789012", {{"178870394", std::string(a.id)}});
    CheckEqInt(r, "registry.rooms.assign_noop", 0, static_cast<long long>(events.size()));

    // Complete-sweep semantics: a device absent from the sweep is cleared.
    events.clear();
    registry.AssignDeviceRooms("123456789012", {{"178870395", "b-device"}});
    CheckEqInt(r, "registry.rooms.clear_events", 2, static_cast<long long>(events.size()));
    CheckTrue(r, "registry.rooms.clear_a", registry.GetDevice(a.id, out) && out.room_id.empty());
    CheckTrue(r, "registry.rooms.clear_b_moved",
              registry.GetDevice("b-device", out) && std::string_view(out.room_id) == "178870395");

    registry.SetEventListener(nullptr);
    registry.ClearAll();
}

/// Presence-poll merge: changed codes only, a rerun is silent.
void TestApplyPresenceStatus(TestResult& r) {
    DeviceRegistry& registry = DeviceRegistry::GetInstance();
    std::vector<RegistryEventPayload> events;
    registry.SetEventListener([&events](const RegistryEventPayload& ev) { events.push_back(ev); });
    registry.ClearAll();

    Device device;
    device.id = "presence-dev";
    device.name = "Presence Device";
    device.status["switch_1"] = DpValue::FromBool(true);
    device.status["countdown_1"] = DpValue::FromInt(0);
    registry.UpsertDevice(device);

    // The poll may carry per-DP report times; the changed code is written once.
    Device poll = device;
    poll.status["switch_1"] = DpValue::FromBool(false);
    poll.dp_timestamps["switch_1"] = 1752456620499;
    events.clear();
    PresenceApplyResult merged = ApplyPresenceStatus(registry, poll);
    CheckEqInt(r, "presence.apply.changed_applied", 1, static_cast<long long>(merged.applied));
    CheckEqInt(r, "presence.apply.changed_unchanged", 1, static_cast<long long>(merged.unchanged));
    CheckEqInt(r, "presence.apply.changed_events", 1, static_cast<long long>(events.size()));
    Device out;
    CheckTrue(r, "presence.apply.value_stored",
              registry.GetDevice(device.id, out) && !out.status["switch_1"].b);
    CheckEqInt(r, "presence.apply.timestamp_stored", 1752456620499,
               out.dp_timestamps.count("switch_1") == 1 ? out.dp_timestamps["switch_1"] : 0);
    CheckTrue(r, "presence.apply.not_polled_no_timestamp",
              out.dp_timestamps.count("countdown_1") == 0);

    // An identical rerun: nothing written, so NOT one event per code.
    events.clear();
    merged = ApplyPresenceStatus(registry, poll);
    CheckEqInt(r, "presence.apply.rerun_applied", 0, static_cast<long long>(merged.applied));
    CheckEqInt(r, "presence.apply.rerun_unchanged", 2, static_cast<long long>(merged.unchanged));
    CheckEqInt(r, "presence.apply.rerun_no_event", 0, static_cast<long long>(events.size()));

    // Several moves in one poll are applied together.
    Device both = poll;
    both.status["switch_1"] = DpValue::FromBool(true);
    both.status["countdown_1"] = DpValue::FromInt(30);
    events.clear();
    merged = ApplyPresenceStatus(registry, both);
    CheckEqInt(r, "presence.apply.multi_applied", 2, static_cast<long long>(merged.applied));
    CheckEqInt(r, "presence.apply.multi_unchanged", 0, static_cast<long long>(merged.unchanged));
    CheckEqInt(r, "presence.apply.multi_events", 2, static_cast<long long>(events.size()));

    // Not committed locally: nothing to merge into.
    Device ghost;
    ghost.id = "presence-ghost";
    ghost.status["switch_1"] = DpValue::FromBool(true);
    events.clear();
    merged = ApplyPresenceStatus(registry, ghost);
    CheckTrue(r, "presence.apply.unknown_device_noop",
              merged.applied == 0 && merged.unchanged == 0);

    // A disabled entity never takes the cloud value.
    CheckTrue(r, "presence.apply.disable_entity",
              registry.SetEntityEnabled(device.id, "countdown_1", false));
    Device disabled_poll = both;
    disabled_poll.status["countdown_1"] = DpValue::FromInt(45);
    events.clear();
    merged = ApplyPresenceStatus(registry, disabled_poll);
    CheckEqInt(r, "presence.apply.disabled_applied", 0, static_cast<long long>(merged.applied));
    CheckEqInt(r, "presence.apply.disabled_unchanged", 2, static_cast<long long>(merged.unchanged));
    CheckEqInt(r, "presence.apply.disabled_no_event", 0, static_cast<long long>(events.size()));
    CheckTrue(r, "presence.apply.disabled_value_kept",
              registry.GetDevice(device.id, out) && out.status["countdown_1"].i == 30);

    // A poll without a status list (both arrays absent) is a no-op.
    Device no_status;
    no_status.id = device.id;
    merged = ApplyPresenceStatus(registry, no_status);
    CheckTrue(r, "presence.apply.empty_status_noop", merged.applied == 0 && merged.unchanged == 0);

    registry.SetEventListener(nullptr);
    registry.ClearAll();
}

}  // namespace

bool RunModelSelfTest() {
    TestResult r;
    TestParseHomes(r);
    TestParseDevice(r);
    TestParseSpecifications(r);
    TestParseStatusStrategy(r);
    TestStrategies(r);
    TestRegistry(r);
    TestParseRooms(r);
    TestParseRoomDevices(r);
    TestRegistryRooms(r);
    TestApplyPresenceStatus(r);

    if (r.passed == r.total) {
        ESP_LOGI(TAG, "MODEL SELFTEST PASS (%d checks)", r.total);
        return true;
    }
    ESP_LOGE(TAG, "MODEL SELFTEST FAIL (%d/%d)", r.passed, r.total);
    return false;
}

}  // namespace hub_core
