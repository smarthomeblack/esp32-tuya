#include "hub_core/device_store_test.h"

#include <cJSON.h>
#include <esp_log.h>

#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "device_store_internal.h"
#include "hub_core/device.h"
#include "hub_core/dp_value.h"

static const char* TAG = "DeviceStoreTest";

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

// Golden fragments — the byte-exact version-1 registry.json contract.

/// The golden device: every field populated, all four DpValue types.
constexpr const char* kGoldenDeviceJson =
    "{\"id\":\"6cd4a1f9c9abcf3d\",\"name\":\"Kitchen Meter\","
    "\"category\":\"cz\",\"product_id\":\"keyju9kk\",\"product_name\":\"Kitchen Meter Double\","
    "\"model\":\"TS0111\",\"uuid\":\"7c25f2e5e0e9d5f2\",\"asset_id\":\"8796093022204\","
    "\"icon\":\"smart/bn_icon_ci9j.png\",\"time_zone\":\"+07:00\","
    "\"sub\":true,\"online\":true,\"active_time\":1234567890,\"create_time\":1234567890,"
    "\"update_time\":1234567890,\"home_id\":\"123456789012\",\"room_id\":\"178870394\","
    "\"support_local\":true,"
    "\"status\":{\"bright_value\":[\"i\",500],\"colour_data\":[\"s\",\"h:210,s:800,v:900\"],"
    "\"cur_current\":[\"d\",2.5],\"fault\":[\"s\",\"\"],\"switch_1\":[\"b\",true]},"
    "\"function\":{\"bright_value\":{\"name\":\"Brightness\",\"desc\":\"Brightness level\","
    "\"type\":\"Integer\",\"values\":\"{\\\"min\\\":10,\\\"max\\\":1000,\\\"scale\\\":0,"
    "\\\"step\\\":1}\"}},"
    "\"status_range\":{\"cur_power\":{\"name\":\"Power\",\"type\":\"Integer\","
    "\"values\":\"{\\\"min\\\":0,\\\"max\\\":5000,\\\"scale\\\":1,\\\"step\\\":1}\","
    "\"report_type\":\"minux\"}},"
    "\"local_strategy\":{\"20\":{\"support_local\":true,\"value_convert\":\"default\","
    "\"status_code\":\"switch_1\",\"status_format_json\":\"{\\\"switch_1\\\":\\\"\\\"}\","
    "\"value_type\":\"Boolean\",\"value_desc_json\":\"{}\","
    "\"enum_mapping\":{\"0\":\"close\",\"1\":\"open\"},\"use_open_api\":true,"
    "\"property_update\":true}},"
    "\"disabled_entities\":[\"add_ele\",\"countdown_1\"]}";

/// The full file body: one home (with its room list) + the golden device (saved_at fixed).
constexpr const char* kGoldenStoreJson =
    "{\"version\":1,\"uid\":\"az1757064314079LqZel\",\"saved_at\":1234567890,"
    "\"homes\":[{\"id\":\"123456789012\",\"name\":\"Home A\","
    "\"rooms\":[{\"id\":\"178870394\",\"name\":\"Phòng Làm Việc\",\"sort\":1}]}],"
    "\"devices\":["
    "{\"id\":\"6cd4a1f9c9abcf3d\",\"name\":\"Kitchen Meter\","
    "\"category\":\"cz\",\"product_id\":\"keyju9kk\",\"product_name\":\"Kitchen Meter Double\","
    "\"model\":\"TS0111\",\"uuid\":\"7c25f2e5e0e9d5f2\",\"asset_id\":\"8796093022204\","
    "\"icon\":\"smart/bn_icon_ci9j.png\",\"time_zone\":\"+07:00\","
    "\"sub\":true,\"online\":true,\"active_time\":1234567890,\"create_time\":1234567890,"
    "\"update_time\":1234567890,\"home_id\":\"123456789012\",\"room_id\":\"178870394\","
    "\"support_local\":true,"
    "\"status\":{\"bright_value\":[\"i\",500],\"colour_data\":[\"s\",\"h:210,s:800,v:900\"],"
    "\"cur_current\":[\"d\",2.5],\"fault\":[\"s\",\"\"],\"switch_1\":[\"b\",true]},"
    "\"function\":{\"bright_value\":{\"name\":\"Brightness\",\"desc\":\"Brightness level\","
    "\"type\":\"Integer\",\"values\":\"{\\\"min\\\":10,\\\"max\\\":1000,\\\"scale\\\":0,"
    "\\\"step\\\":1}\"}},"
    "\"status_range\":{\"cur_power\":{\"name\":\"Power\",\"type\":\"Integer\","
    "\"values\":\"{\\\"min\\\":0,\\\"max\\\":5000,\\\"scale\\\":1,\\\"step\\\":1}\","
    "\"report_type\":\"minux\"}},"
    "\"local_strategy\":{\"20\":{\"support_local\":true,\"value_convert\":\"default\","
    "\"status_code\":\"switch_1\",\"status_format_json\":\"{\\\"switch_1\\\":\\\"\\\"}\","
    "\"value_type\":\"Boolean\",\"value_desc_json\":\"{}\","
    "\"enum_mapping\":{\"0\":\"close\",\"1\":\"open\"},\"use_open_api\":true,"
    "\"property_update\":true}},"
    "\"disabled_entities\":[\"add_ele\",\"countdown_1\"]}"
    "]}";

/// A snapshot written before rooms existed: no devices[].room_id, no homes[].rooms.
constexpr const char* kLegacyStoreJson =
    "{\"version\":1,\"uid\":\"az1757064314079LqZel\",\"saved_at\":1234567890,"
    "\"homes\":[{\"id\":\"123456789012\",\"name\":\"Home A\"}],"
    "\"devices\":[{\"id\":\"6cd4a1f9d0e1f2a3\",\"name\":\"Kitchen Socket\","
    "\"home_id\":\"123456789012\",\"status\":{\"switch_1\":[\"b\",false]}}]}";

/// Every corruption the tolerance policy names (bad tag/length/key/value).
constexpr const char* kCorruptDeviceJson =
    "{\"id\":\"bad1\",\"name\":\"Corrupt\","
    "\"status\":{\"broken_tag\":[\"x\",\"y\"],\"short\":[\"b\"],"
    "\"long\":[\"b\",true,false],\"good\":[\"b\",false]},"
    "\"local_strategy\":{\"abc\":{\"support_local\":true},"
    "\"21\":{\"value_convert\":\"default\",\"enum_mapping\":{\"0\":\"ok\",\"1\":5}}},"
    "\"function\":{\"fn1\":{\"name\":\"F\",\"type\":\"Integer\",\"values\":\"{}\"}},"
    "\"disabled_entities\":[\"gone_1\",\"gone_2\"]}";

/// Forward-compat device: an unknown extra field, all optional fields missing.
constexpr const char* kForwardDeviceJson =
    "{\"future_field\":123,\"nested\":{\"deep\":true},\"id\":\"fwd1\",\"name\":\"Forward\"}";

// Builders + comparators

/// The golden device (dp_timestamps / set_up NOT persisted).
Device BuildGoldenDevice() {
    Device device;
    device.id = "6cd4a1f9c9abcf3d";
    device.name = "Kitchen Meter";
    device.category = "cz";
    device.product_id = "keyju9kk";
    device.product_name = "Kitchen Meter Double";
    device.model = "TS0111";
    device.uuid = "7c25f2e5e0e9d5f2";
    device.asset_id = "8796093022204";
    device.icon = "smart/bn_icon_ci9j.png";
    device.time_zone = "+07:00";
    device.sub = true;
    device.online = true;
    device.active_time = 1234567890;
    device.create_time = 1234567890;
    device.update_time = 1234567890;
    device.home_id = "123456789012";
    device.room_id = "178870394";
    device.support_local = true;

    // All four DpValue types + the empty-string kString pin ("fault").
    device.status["bright_value"] = DpValue::FromInt(500);
    device.status["colour_data"] = DpValue::FromString("h:210,s:800,v:900");
    device.status["cur_current"] = DpValue::FromDouble(2.5);
    device.status["fault"] = DpValue::FromString("");
    device.status["switch_1"] = DpValue::FromBool(true);

    DeviceFunction fn;
    fn.code = "bright_value";
    fn.name = "Brightness";
    fn.desc = "Brightness level";
    fn.type = "Integer";
    fn.values_json = "{\"min\":10,\"max\":1000,\"scale\":0,\"step\":1}";
    device.function.emplace(fn.code, std::move(fn));

    DeviceStatusRange range;
    range.code = "cur_power";
    range.name = "Power";
    range.type = "Integer";
    range.values_json = "{\"min\":0,\"max\":5000,\"scale\":1,\"step\":1}";
    range.report_type = "minux";
    device.status_range.emplace(range.code, std::move(range));

    StrategyConfig cfg;
    cfg.dp_id = 20;
    cfg.support_local = true;
    cfg.value_convert = "default";
    cfg.status_code = "switch_1";
    cfg.status_format_json = "{\"switch_1\":\"\"}";
    cfg.value_type = "Boolean";
    cfg.value_desc_json = "{}";
    cfg.enum_mapping["0"] = "close";
    cfg.enum_mapping["1"] = "open";
    cfg.use_open_api = true;
    cfg.property_update = true;
    device.local_strategy.emplace(20, std::move(cfg));

    // Insertion order deliberately unsorted — the set serializes sorted.
    device.disabled_entities.insert("countdown_1");
    device.disabled_entities.insert("add_ele");
    return device;
}

/// Second round-trip device: mostly defaults plus one bool status.
Device BuildSocketDevice() {
    Device device;
    device.id = "6cd4a1f9d0e1f2a3";
    device.name = "Kitchen Socket";
    device.category = "kg";
    device.product_id = "keyju9kk";
    device.product_name = "Socket S1";
    device.active_time = 1000000000;
    device.home_id = "987654321098";
    device.status["switch_1"] = DpValue::FromBool(false);
    return device;
}

/// Per-value map equality with a custom comparator (no operator==).
template <typename MapT, typename ValueEq>
bool MapsEqual(const MapT& a, const MapT& b, ValueEq value_eq) {
    if (a.size() != b.size()) {
        return false;
    }
    for (const auto& entry : a) {
        const auto found = b.find(entry.first);
        if (found == b.end() || !value_eq(entry.second, found->second)) {
            return false;
        }
    }
    return true;
}

bool FunctionsEqual(const DeviceFunction& a, const DeviceFunction& b) {
    return a.code == b.code && a.name == b.name && a.desc == b.desc && a.type == b.type &&
           a.values_json == b.values_json;
}

bool StatusRangesEqual(const DeviceStatusRange& a, const DeviceStatusRange& b) {
    return a.code == b.code && a.name == b.name && a.type == b.type &&
           a.values_json == b.values_json && a.report_type == b.report_type;
}

bool StrategyConfigsEqual(const StrategyConfig& a, const StrategyConfig& b) {
    return a.dp_id == b.dp_id && a.support_local == b.support_local &&
           a.value_convert == b.value_convert && a.status_code == b.status_code &&
           a.status_format_json == b.status_format_json && a.value_type == b.value_type &&
           a.value_desc_json == b.value_desc_json && a.enum_mapping == b.enum_mapping &&
           a.use_open_api == b.use_open_api && a.property_update == b.property_update;
}

/// Field-by-field device comparison as named check groups.
void CheckDeviceMatch(TestResult& r, const std::string& prefix, const Device& expected,
                      const Device& got) {
    CheckTrue(r, (prefix + "scalars").c_str(),
              expected.id == got.id && expected.name == got.name &&
                  expected.category == got.category && expected.product_id == got.product_id &&
                  expected.product_name == got.product_name && expected.model == got.model &&
                  expected.uuid == got.uuid && expected.asset_id == got.asset_id &&
                  expected.icon == got.icon && expected.time_zone == got.time_zone &&
                  expected.sub == got.sub && expected.online == got.online &&
                  expected.active_time == got.active_time &&
                  expected.create_time == got.create_time &&
                  expected.update_time == got.update_time && expected.home_id == got.home_id &&
                  expected.room_id == got.room_id && expected.support_local == got.support_local);
    CheckTrue(r, (prefix + "status").c_str(), expected.status == got.status);
    CheckTrue(r, (prefix + "function").c_str(),
              MapsEqual(expected.function, got.function, FunctionsEqual));
    CheckTrue(r, (prefix + "status_range").c_str(),
              MapsEqual(expected.status_range, got.status_range, StatusRangesEqual));
    CheckTrue(r, (prefix + "local_strategy").c_str(),
              MapsEqual(expected.local_strategy, got.local_strategy, StrategyConfigsEqual));
    CheckTrue(r, (prefix + "disabled_entities").c_str(),
              expected.disabled_entities == got.disabled_entities);
    // The NOT-persisted fields stay default after a load.
    CheckTrue(r, (prefix + "not_persisted").c_str(), got.dp_timestamps.empty() && !got.set_up);
}

/// Logs the first differing byte offset of two unequal strings.
void LogFirstDiff(const std::string& expected, const std::string& got) {
    size_t pos = 0;
    while (pos < expected.size() && pos < got.size() && expected[pos] == got[pos]) {
        ++pos;
    }
    ESP_LOGE(TAG, "  first difference at byte %zu (expected len %zu, got len %zu)", pos,
             expected.size(), got.size());
}

// Test sections

/// Check 1: BuildStoreJson output is byte-identical to the golden.
void TestGoldenSerialization(TestResult& r) {
    std::vector<Home> homes(1);
    homes[0].id = "123456789012";
    homes[0].name = "Home A";
    homes[0].rooms.push_back(Room{"178870394", "Phòng Làm Việc", 1});
    const std::vector<Device> devices = {BuildGoldenDevice()};
    const std::string got = BuildStoreJson(homes, devices, "az1757064314079LqZel", 1234567890);
    CheckTrue(r, "golden.build_nonempty", !got.empty());
    CheckEqStr(r, "golden.store_json", kGoldenStoreJson, got);
    if (got != kGoldenStoreJson) {
        LogFirstDiff(kGoldenStoreJson, got);
    }
}

/// Check 2: the golden device JSON parses back to the original device.
void TestGoldenDeviceParse(TestResult& r) {
    const Device golden = BuildGoldenDevice();
    Device parsed;
    bool ok = false;
    {
        JsonPtr json(kGoldenDeviceJson);
        ok = DeviceFromJson(json.root(), parsed);
    }
    CheckTrue(r, "golden.device_parse_ok", ok);
    CheckDeviceMatch(r, "golden.device.", golden, parsed);
    // Pin the empty-string DpValue explicitly.
    CheckTrue(r, "golden.device.status_empty_string",
              parsed.status.count("fault") == 1 &&
                  parsed.status["fault"].type == DpValue::Type::kString &&
                  parsed.status["fault"].s.empty());
}

/// Check 3: forward compatibility — unknown fields ignored, defaults kept.
void TestForwardCompat(TestResult& r) {
    Device parsed;
    bool ok = false;
    {
        JsonPtr json(kForwardDeviceJson);
        ok = DeviceFromJson(json.root(), parsed);
    }
    CheckTrue(r, "forward.device_ok", ok);
    CheckEqStr(r, "forward.device.id", "fwd1", parsed.id);
    CheckEqStr(r, "forward.device.name", "Forward", parsed.name);
    CheckTrue(r, "forward.device.defaults",
              !parsed.sub && !parsed.online && !parsed.support_local && parsed.active_time == 0 &&
                  parsed.category.empty() && parsed.home_id.empty() && parsed.status.empty() &&
                  parsed.function.empty() && parsed.status_range.empty() &&
                  parsed.local_strategy.empty() && parsed.disabled_entities.empty() &&
                  parsed.dp_timestamps.empty());

    std::vector<Home> homes;
    std::vector<Device> devices;
    std::string uid;
    CheckTrue(
        r, "forward.store_ok",
        ParseStoreJson("{\"version\":1,\"uid\":\"fwd_uid\",\"future\":true,"
                       "\"homes\":[{\"id\":\"h1\",\"name\":\"H\",\"extra\":1}],\"devices\":[]}",
                       homes, devices, uid));
    CheckEqStr(r, "forward.store.uid", "fwd_uid", uid);
    CheckEqInt(r, "forward.store.homes", 1, static_cast<long long>(homes.size()));
    CheckEqInt(r, "forward.store.devices", 0, static_cast<long long>(devices.size()));
    CheckTrue(r, "forward.store.home_fields",
              homes.size() == 1 && homes[0].id == "h1" && homes[0].name == "H");

    // A missing uid keeps ""; missing arrays yield empty vectors.
    homes.clear();
    devices.clear();
    uid = "stale";
    CheckTrue(r, "forward.store_minimal_ok",
              ParseStoreJson("{\"version\":1}", homes, devices, uid));
    CheckEqStr(r, "forward.store_minimal.uid", "", uid);
    CheckEqInt(r, "forward.store_minimal.homes", 0, static_cast<long long>(homes.size()));
    CheckEqInt(r, "forward.store_minimal.devices", 0, static_cast<long long>(devices.size()));
}

/// Check 4: corrupt tolerance — each bad field is dropped, no crash.
void TestCorruptTolerance(TestResult& r) {
    Device parsed;
    bool ok = false;
    {
        JsonPtr json(kCorruptDeviceJson);
        ok = DeviceFromJson(json.root(), parsed);
    }
    CheckTrue(r, "corrupt.device_returned", ok);
    CheckEqStr(r, "corrupt.id", "bad1", parsed.id);
    CheckEqInt(r, "corrupt.status_size", 1, static_cast<long long>(parsed.status.size()));
    CheckTrue(r, "corrupt.status_good_kept",
              parsed.status.count("good") == 1 &&
                  parsed.status["good"].type == DpValue::Type::kBool && !parsed.status["good"].b);
    CheckTrue(r, "corrupt.status_bad_dropped",
              parsed.status.count("broken_tag") == 0 && parsed.status.count("short") == 0 &&
                  parsed.status.count("long") == 0);
    CheckEqInt(r, "corrupt.strategy_size", 1, static_cast<long long>(parsed.local_strategy.size()));
    CheckTrue(r, "corrupt.strategy_abc_dropped", parsed.local_strategy.count(21) == 1);
    CheckTrue(r, "corrupt.strategy_enum_size",
              parsed.local_strategy.count(21) == 1 &&
                  parsed.local_strategy[21].enum_mapping.size() == 1);
    CheckTrue(r, "corrupt.strategy_enum_ok_kept",
              parsed.local_strategy.count(21) == 1 &&
                  parsed.local_strategy[21].enum_mapping.count("0") == 1 &&
                  parsed.local_strategy[21].enum_mapping["0"] == "ok");
    CheckEqInt(r, "corrupt.function_size", 1, static_cast<long long>(parsed.function.size()));
    CheckEqInt(r, "corrupt.disabled_size", 2,
               static_cast<long long>(parsed.disabled_entities.size()));
}

/// Check 5: serialize -> parse -> serialize yields identical strings.
void TestSnapshotRoundTrip(TestResult& r) {
    std::vector<Home> homes(2);
    homes[0].id = "123456789012";
    homes[0].name = "Home A";
    homes[0].rooms.push_back(Room{"178870394", "Phòng Làm Việc", 1});
    homes[1].id = "987654321098";
    homes[1].name = "Home B";
    const std::vector<Device> devices = {BuildGoldenDevice(), BuildSocketDevice()};
    const std::string uid = "az1757064314079LqZel";

    const std::string first = BuildStoreJson(homes, devices, uid, 1234567890);
    CheckTrue(r, "roundtrip.build_nonempty", !first.empty());

    std::vector<Home> homes2;
    std::vector<Device> devices2;
    std::string uid2;
    CheckTrue(r, "roundtrip.parse_ok", ParseStoreJson(first, homes2, devices2, uid2));
    CheckEqStr(r, "roundtrip.uid", uid, uid2);
    CheckEqInt(r, "roundtrip.homes_count", 2, static_cast<long long>(homes2.size()));
    CheckEqInt(r, "roundtrip.devices_count", 2, static_cast<long long>(devices2.size()));
    CheckTrue(r, "roundtrip.homes_fields",
              homes2.size() == 2 && homes2[0].id == "123456789012" && homes2[0].name == "Home A" &&
                  homes2[1].id == "987654321098" && homes2[1].name == "Home B");
    CheckTrue(r, "roundtrip.rooms",
              homes2.size() == 2 && homes2[0].rooms.size() == 1 &&
                  homes2[0].rooms[0].id == "178870394" &&
                  homes2[0].rooms[0].name == "Phòng Làm Việc" && homes2[0].rooms[0].sort == 1 &&
                  homes2[1].rooms.empty());
    if (devices2.size() == 2) {
        // Vector order is preserved by the parse (array order).
        CheckDeviceMatch(r, "roundtrip.device0.", devices[0], devices2[0]);
        CheckDeviceMatch(r, "roundtrip.device1.", devices[1], devices2[1]);
    }
    // The golden device's room must survive the trip; the socket's stays "".
    CheckTrue(r, "roundtrip.device_room_id",
              devices2.size() == 2 && devices2[0].room_id == "178870394" &&
                  devices2[1].room_id.empty());

    const std::string second = BuildStoreJson(homes2, devices2, uid2, 1234567890);
    if (first != second) {
        LogFirstDiff(first, second);
    }
    CheckTrue(r, "roundtrip.identical_strings", first == second);
}

/// Check 6: a snapshot written before rooms existed still loads (room_id "").
void TestLegacySnapshot(TestResult& r) {
    std::vector<Home> homes;
    std::vector<Device> devices;
    std::string uid;
    CheckTrue(r, "legacy.parse_ok", ParseStoreJson(kLegacyStoreJson, homes, devices, uid));
    CheckEqInt(r, "legacy.homes_count", 1, static_cast<long long>(homes.size()));
    CheckTrue(r, "legacy.home_rooms_unknown",
              homes.size() == 1 && homes[0].rooms.empty() && homes[0].name == "Home A");
    CheckEqInt(r, "legacy.devices_count", 1, static_cast<long long>(devices.size()));
    CheckTrue(r, "legacy.device_room_unassigned",
              devices.size() == 1 && devices[0].room_id.empty() &&
                  devices[0].status.count("switch_1") == 1 &&
                  devices[0].status["switch_1"].type == DpValue::Type::kBool &&
                  !devices[0].status["switch_1"].b);
    // A re-save of the loaded model emits both optional keys (room_id "").
    const std::string rewritten = BuildStoreJson(homes, devices, uid, 1);
    CheckTrue(r, "legacy.resave_adds_room_id",
              rewritten.find("\"room_id\":\"\"") != std::string::npos &&
                  rewritten.find("\"rooms\"") == std::string::npos);
}

}  // namespace

bool RunDeviceStoreSelfTest() {
    TestResult r;
    TestGoldenSerialization(r);
    TestGoldenDeviceParse(r);
    TestForwardCompat(r);
    TestCorruptTolerance(r);
    TestSnapshotRoundTrip(r);
    TestLegacySnapshot(r);

    if (r.passed == r.total) {
        ESP_LOGI(TAG, "DEVICE STORE SELFTEST PASS (%d checks)", r.total);
        return true;
    }
    ESP_LOGE(TAG, "DEVICE STORE SELFTEST FAIL (%d/%d)", r.passed, r.total);
    return false;
}

}  // namespace hub_core
