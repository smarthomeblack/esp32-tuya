#include "tuya_mqtt/mqtt_message_test.h"

#include <cJSON.h>
#include <esp_log.h>

#include <cstring>
#include <string>

#include "hub_core/dp_value.h"
#include "mqtt_message.h"
#include "tuya_mqtt/tuya_mqtt_client.h"

static const char* TAG = "MqttSelfTest";

namespace tuya_mqtt {

namespace {

struct TestResult {
    int total = 0;
    int passed = 0;
};

/// strlen-based wrapper; the tests always pass NUL-terminated literals.
bool ParseMsg(const char* text, MqttMessage& out) {
    return ParseMqttMessage(text, std::strlen(text), out);
}

void CheckTrue(TestResult& r, const char* name, bool condition) {
    ++r.total;
    if (condition) {
        ++r.passed;
        return;
    }
    ESP_LOGE(TAG, "FAIL %s", name);
}

void CheckEqStr(TestResult& r, const char* name, const std::string& expected,
                const std::string& got) {
    ++r.total;
    if (expected == got) {
        ++r.passed;
        return;
    }
    ESP_LOGE(TAG, "FAIL %s: expected=\"%s\" got=\"%s\"", name, expected.c_str(), got.c_str());
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

// ---- Golden samples — shaped like real MQTT payloads ----

/// protocol 4, support_local form: raw {dpId, value, t} items; one
/// without t, one missing dpId, one string value, one fractional dpId.
constexpr const char* kReportLocalJson =
    "{"
    "\"protocol\": 4, "
    "\"pv\": \"1.0\", \"t\": 1752456620512, \"sig\": \"abcdef0123456789\", "
    "\"data\": {"
    "\"devId\": \"6cd4a1f9c9abc\", "
    "\"status\": ["
    "{\"dpId\": 1, \"t\": 1752456620499, \"value\": 120},"
    "{\"dpId\": 20, \"value\": true},"
    "{\"dpId\": 61, \"value\": \"2\", \"t\": 1752456620500},"
    "{\"value\": 9},"
    "{\"dpId\": 1.5, \"value\": 7}"
    "]"
    "}"
    "}";

/// protocol 4, non-local form: standard {code, value, t} items, plus a
/// broken row without a code.
constexpr const char* kReportNonLocalJson =
    "{"
    "\"protocol\": 4, "
    "\"data\": {"
    "\"devId\": \"6cd4a1f9d0\", "
    "\"status\": ["
    "{\"code\": \"switch_led\", \"value\": true, \"t\": 1752456620500},"
    "{\"code\": \"work_mode\", \"value\": \"white\"},"
    "{\"code\": \"bright_value\", \"value\": 25.5},"
    "{\"value\": 1}"
    "]"
    "}"
    "}";

/// protocol 4 with a missing devId — parse must fail.
constexpr const char* kReportNoDevIdJson =
    "{\"protocol\": 4, \"data\": {\"status\": [{\"code\": \"switch_led\", \"value\": true}]}}";

/// protocol 20 online / offline / delete / bindUser / dpNameUpdate.
constexpr const char* kEventOnlineJson =
    "{\"protocol\": 20, \"data\": {\"bizCode\": \"online\", \"bizData\": {\"devId\": "
    "\"6cd4a1f9c9abc\"}}}";
constexpr const char* kEventOfflineJson =
    "{\"protocol\": 20, \"data\": {\"bizCode\": \"offline\", \"bizData\": {\"devId\": "
    "\"6cd4a1f9d0\"}}}";
constexpr const char* kEventDeleteJson =
    "{\"protocol\": 20, \"data\": {\"bizCode\": \"delete\", \"bizData\": {\"devId\": "
    "\"6cd4a1f9c9abc\"}}}";
constexpr const char* kEventBindUserJson =
    "{\"protocol\": 20, \"data\": {\"bizCode\": \"bindUser\", \"bizData\": {\"devId\": "
    "\"6cd4a1f9c9abc\", \"uid\": \"u-001\"}}}";
constexpr const char* kEventDpNameUpdateJson =
    "{\"protocol\": 20, \"data\": {\"bizCode\": \"dpNameUpdate\", \"bizData\": {\"devId\": "
    "\"6cd4a1f9c9abc\", \"dpName\": \"brightness\"}}}";

/// protocol 20 nameUpdate — bizData.name carries the new name.
constexpr const char* kEventNameUpdateJson =
    "{"
    "\"protocol\": 20, "
    "\"data\": {\"bizCode\": \"nameUpdate\", \"bizData\": {\"devId\": \"6cd4a1f9c9abc\", "
    "\"name\": \"Kitchen Light\"}}"
    "}";

/// protocol 20 with an UNKNOWN bizCode: the message parses and the
/// dispatch ignores it (bizData never touched).
constexpr const char* kEventUnknownBizJson =
    "{\"protocol\": 20, \"data\": {\"bizCode\": \"somethingNew\"}}";

/// protocol 20 with a known bizCode but missing bizData — parse must fail.
constexpr const char* kEventNoBizDataJson =
    "{\"protocol\": 20, \"data\": {\"bizCode\": \"online\"}}";

/// Not JSON at all / a JSON array.
constexpr const char* kGarbagePayload = "not json at all";
constexpr const char* kArrayPayload = "[1, 2, 3]";

/// protocol 5 (unhandled): parses, dispatch ignores.
constexpr const char* kProtocolOtherJson = "{\"protocol\": 5, \"data\": {\"x\": 1}}";

/// No protocol key at all: defaults to 0.
constexpr const char* kNoProtocolJson = "{\"data\": {\"x\": 1}}";

/// Access-config result — real field/topic shape, invented values.
constexpr const char* kConfigJson = "{"
                                    "\"url\": \"ssl://mq3g.iotbing.com:8883\","
                                    "\"clientId\": \"cloudmix.e8f1a2b3c4d5\","
                                    "\"username\": \"suba1b2c3d4e5f6\","
                                    "\"password\": \"1ca8e9d0c7b2\","
                                    "\"expireTime\": 7200,"
                                    "\"topic\": {"
                                    "\"ownerId\": {\"sub\": \"cloud/mix/{ownerId}.d\"},"
                                    "\"devId\": {\"sub\": \"cloud/mix/d/{devId}\"}"
                                    "}"
                                    "}";

/// mqtts:// scheme without a port (defaults to 8883).
constexpr const char* kConfigMqttsNoPortJson =
    "{"
    "\"url\": \"mqtts://mq3g.example.com\","
    "\"clientId\": \"c\", \"username\": \"u\", \"password\": \"p\", \"expireTime\": 3600,"
    "\"topic\": {\"ownerId\": {\"sub\": \"a/{ownerId}\"}, \"devId\": {\"sub\": \"b/{devId}\"}}"
    "}";

/// Access config missing the topic templates — unusable, parse fails.
constexpr const char* kConfigNoTopicJson =
    "{"
    "\"url\": \"ssl://h:8883\", \"clientId\": \"c\", \"username\": \"u\", \"password\": \"p\","
    "\"expireTime\": 7200"
    "}";

// ---- Test sections ----

void TestParseReportLocal(TestResult& r) {
    MqttMessage msg;
    CheckTrue(r, "report_local.parse_ok",
              ParseMsg(kReportLocalJson, msg) && msg.protocol == kProtocolDeviceReport);
    CheckEqStr(r, "report_local.dev_id", "6cd4a1f9c9abc", msg.dev_id);
    CheckEqInt(r, "report_local.item_count", 5, static_cast<long long>(msg.items.size()));
    if (msg.items.size() == 5) {
        CheckTrue(r, "report_local.item0.has_dpId_value_t",
                  msg.items[0].has_dp_id && msg.items[0].dp_id == 1 && msg.items[0].has_value &&
                      msg.items[0].t == 1752456620499);
        CheckEqInt(r, "report_local.item0.value_int", 120,
                   static_cast<long long>(msg.items[0].value.i));
        CheckTrue(r, "report_local.item0.value_is_int",
                  msg.items[0].value.type == hub_core::DpValue::Type::kInt);
        CheckTrue(r, "report_local.item1.value_bool",
                  msg.items[1].value.type == hub_core::DpValue::Type::kBool &&
                      msg.items[1].value.b);
        CheckEqInt(r, "report_local.item1.t_absent", 0, msg.items[1].t);
        CheckEqStr(r, "report_local.item2.value_string", "2", msg.items[2].value.s);
        // A value without dpId parses but the dispatch drops it.
        CheckTrue(r, "report_local.item3.no_dpId", !msg.items[3].has_dp_id);
        // A fractional dpId is unusable: parsed like a missing dpId.
        CheckTrue(r, "report_local.item4.fractional_dpId", !msg.items[4].has_dp_id);
    }
}

void TestParseReportNonLocal(TestResult& r) {
    MqttMessage msg;
    CheckTrue(r, "report_nonlocal.parse_ok",
              ParseMsg(kReportNonLocalJson, msg) && msg.protocol == kProtocolDeviceReport);
    CheckEqStr(r, "report_nonlocal.dev_id", "6cd4a1f9d0", msg.dev_id);
    CheckEqInt(r, "report_nonlocal.item_count", 4, static_cast<long long>(msg.items.size()));
    if (msg.items.size() == 4) {
        CheckTrue(r, "report_nonlocal.item0.has_code_value_t",
                  msg.items[0].has_code && msg.items[0].code == "switch_led" &&
                      msg.items[0].has_value && msg.items[0].t == 1752456620500);
        CheckTrue(r, "report_nonlocal.item0.value_bool",
                  msg.items[0].value.type == hub_core::DpValue::Type::kBool &&
                      msg.items[0].value.b);
        CheckEqStr(r, "report_nonlocal.item1.value_string", "white", msg.items[1].value.s);
        CheckTrue(r, "report_nonlocal.item2.value_double",
                  msg.items[2].value.type == hub_core::DpValue::Type::kDouble);
        CheckTrue(r, "report_nonlocal.item3.no_code", !msg.items[3].has_code);
    }
}

void TestParseEvents(TestResult& r) {
    MqttMessage msg;
    CheckTrue(r, "event_online.parse_ok",
              ParseMsg(kEventOnlineJson, msg) && msg.protocol == kProtocolOther);
    CheckEqStr(r, "event_online.biz_code", "online", msg.biz_code);
    CheckEqStr(r, "event_online.dev_id", "6cd4a1f9c9abc", msg.dev_id);
    CheckEqStr(r, "event_online.biz_name", "", msg.biz_name);

    CheckTrue(r, "event_offline.parse_ok", ParseMsg(kEventOfflineJson, msg));
    CheckEqStr(r, "event_offline.biz_code", "offline", msg.biz_code);
    CheckEqStr(r, "event_offline.dev_id", "6cd4a1f9d0", msg.dev_id);

    CheckTrue(r, "event_delete.parse_ok", ParseMsg(kEventDeleteJson, msg));
    CheckEqStr(r, "event_delete.biz_code", "delete", msg.biz_code);
    CheckEqStr(r, "event_delete.dev_id", "6cd4a1f9c9abc", msg.dev_id);

    CheckTrue(r, "event_binduser.parse_ok", ParseMsg(kEventBindUserJson, msg));
    CheckEqStr(r, "event_binduser.biz_code", "bindUser", msg.biz_code);
    CheckEqStr(r, "event_binduser.dev_id", "6cd4a1f9c9abc", msg.dev_id);

    CheckTrue(r, "event_name_update.parse_ok", ParseMsg(kEventNameUpdateJson, msg));
    CheckEqStr(r, "event_name_update.biz_code", "nameUpdate", msg.biz_code);
    CheckEqStr(r, "event_name_update.biz_name", "Kitchen Light", msg.biz_name);

    CheckTrue(r, "event_dpname_update.parse_ok", ParseMsg(kEventDpNameUpdateJson, msg));
    CheckEqStr(r, "event_dpname_update.biz_code", "dpNameUpdate", msg.biz_code);

    // Unknown bizCode: parses and is ignored by the dispatch.
    CheckTrue(r, "event_unknown_biz.parse_ok", ParseMsg(kEventUnknownBizJson, msg));
    CheckEqStr(r, "event_unknown_biz.biz_code", "somethingNew", msg.biz_code);
    CheckEqStr(r, "event_unknown_biz.no_dev_id", "", msg.dev_id);
}

void TestParseFailures(TestResult& r) {
    MqttMessage msg;
    CheckTrue(r, "failure.garbage", !ParseMsg(kGarbagePayload, msg));
    CheckTrue(r, "failure.array", !ParseMsg(kArrayPayload, msg));
    CheckTrue(r, "failure.no_devid", !ParseMsg(kReportNoDevIdJson, msg));
    CheckTrue(r, "failure.no_bizdata", !ParseMsg(kEventNoBizDataJson, msg));
    // Zero-length payload.
    CheckTrue(r, "failure.empty", !ParseMsg("", msg));

    // Unhandled protocol values parse fine (dispatch ignores).
    CheckTrue(r, "ignore.protocol5", ParseMsg(kProtocolOtherJson, msg) && msg.protocol == 5);
    CheckTrue(r, "ignore.no_protocol", ParseMsg(kNoProtocolJson, msg) && msg.protocol == 0);
}

void TestParseConfig(TestResult& r) {
    MqttConfig cfg;
    bool ok = false;
    {
        JsonPtr json(kConfigJson);
        ok = ParseMqttConfig(json.root(), cfg);
    }
    CheckTrue(r, "config.parse_ok", ok);
    CheckEqStr(r, "config.url_raw", "ssl://mq3g.iotbing.com:8883", cfg.url_raw);
    CheckEqStr(r, "config.broker_uri", "mqtts://mq3g.iotbing.com:8883", cfg.broker_uri);
    CheckEqStr(r, "config.host", "mq3g.iotbing.com", cfg.host);
    CheckEqInt(r, "config.port", 8883, cfg.port);
    CheckEqStr(r, "config.client_id", "cloudmix.e8f1a2b3c4d5", cfg.client_id);
    CheckEqStr(r, "config.username", "suba1b2c3d4e5f6", cfg.username);
    CheckEqStr(r, "config.password", "1ca8e9d0c7b2", cfg.password);
    CheckEqInt(r, "config.expire_time", 7200, cfg.expire_time_s);
    CheckEqStr(r, "config.owner_template", "cloud/mix/{ownerId}.d", cfg.owner_topic_template);
    CheckEqStr(r, "config.dev_template", "cloud/mix/d/{devId}", cfg.dev_topic_template);

    // Explicit mqtts:// without a port: defaults to 8883.
    {
        JsonPtr json(kConfigMqttsNoPortJson);
        CheckTrue(r, "config.mqtts_no_port.parse_ok", ParseMqttConfig(json.root(), cfg));
    }
    CheckEqStr(r, "config.mqtts_no_port.broker_uri", "mqtts://mq3g.example.com", cfg.broker_uri);
    CheckEqInt(r, "config.mqtts_no_port.port", 8883, cfg.port);

    // Missing topic templates: unusable config.
    {
        JsonPtr json(kConfigNoTopicJson);
        MqttConfig bad;
        CheckTrue(r, "config.no_topic_rejected", !ParseMqttConfig(json.root(), bad));
    }
}

void TestTranslateUri(TestResult& r) {
    std::string uri;
    std::string host;
    uint16_t port = 0;
    CheckTrue(r, "uri.ssl", TranslateBrokerUri("ssl://h1.example.com:8883", uri, host, port));
    CheckEqStr(r, "uri.ssl.broker_uri", "mqtts://h1.example.com:8883", uri);
    CheckEqStr(r, "uri.ssl.host", "h1.example.com", host);
    CheckEqInt(r, "uri.ssl.port", 8883, port);

    CheckTrue(r, "uri.ssl_default_port",
              TranslateBrokerUri("ssl://h2.example.com", uri, host, port));
    CheckEqStr(r, "uri.ssl_default_port.uri", "mqtts://h2.example.com", uri);
    CheckEqInt(r, "uri.ssl_default_port.port", 8883, port);

    CheckTrue(r, "uri.mqtt", TranslateBrokerUri("mqtt://h3.example.com:1883", uri, host, port));
    CheckEqStr(r, "uri.mqtt.broker_uri", "mqtt://h3.example.com:1883", uri);
    CheckEqInt(r, "uri.mqtt.port", 1883, port);

    CheckTrue(r, "uri.mqtt_default_port",
              TranslateBrokerUri("mqtt://h4.example.com", uri, host, port));
    CheckEqInt(r, "uri.mqtt_default_port.port", 1883, port);

    CheckTrue(r, "uri.mqtts_passthrough",
              TranslateBrokerUri("mqtts://h5.example.com:9443", uri, host, port));
    CheckEqStr(r, "uri.mqtts_passthrough.broker_uri", "mqtts://h5.example.com:9443", uri);

    CheckTrue(r, "uri.ssl_with_path",
              TranslateBrokerUri("ssl://h6.example.com:8883/mqtt", uri, host, port));
    CheckEqStr(r, "uri.ssl_with_path.host", "h6.example.com", host);
    CheckEqInt(r, "uri.ssl_with_path.port", 8883, port);

    CheckTrue(r, "uri.bad_scheme", !TranslateBrokerUri("ws://h7.example.com", uri, host, port));
    CheckTrue(r, "uri.empty", !TranslateBrokerUri("", uri, host, port));
    CheckTrue(r, "uri.no_host", !TranslateBrokerUri("ssl://:8883", uri, host, port));
}

void TestTopics(TestResult& r) {
    // Owner topic: format(ownerId=...), no suffix.
    CheckEqStr(r, "topic.owner", "cloud/mix/123456789012.d",
               FormatTopicTemplate("cloud/mix/{ownerId}.d", "123456789012"));
    // Device topic: format(devId=...) + "/pen" or "/sta".
    CheckEqStr(r, "topic.dev_pen", "cloud/mix/d/6cd4a1f9c9abc/pen",
               DeviceTopic("cloud/mix/d/{devId}", "6cd4a1f9c9abc", true));
    CheckEqStr(r, "topic.dev_sta", "cloud/mix/d/6cd4a1f9d0/sta",
               DeviceTopic("cloud/mix/d/{devId}", "6cd4a1f9d0", false));
    // Repeated placeholders: all occurrences replaced.
    CheckEqStr(r, "topic.dev_repeat", "d/x/y/x/pen", DeviceTopic("d/{devId}/y/{devId}", "x", true));
}

void TestReassembly(TestResult& r) {
    PayloadAssembler assembler;
    std::string out;

    // Two chunks of a 20-byte message.
    CheckTrue(r, "rx.first_chunk_pending",
              !assembler.Feed(0, 20, "0123456789", 10, out) && assembler.active());
    CheckTrue(r, "rx.second_chunk_completes",
              assembler.Feed(10, 20, "ABCDEFGHIJ", 10, out) && !assembler.active());
    CheckEqStr(r, "rx.reassembled", "0123456789ABCDEFGHIJ", out);

    // Whole message in a single event (the common case).
    CheckTrue(r, "rx.single_event", assembler.Feed(0, 5, "hello", 5, out));
    CheckEqStr(r, "rx.single_event.payload", "hello", out);

    // A chunk with no start (lost head) is dropped whole.
    CheckTrue(r, "rx.orphan_chunk", !assembler.Feed(7, 20, "xyz", 3, out));
    CheckEqStr(r, "rx.orphan_dropped", "", out);

    // A gap between chunks drops the message.
    CheckTrue(r, "rx.start", !assembler.Feed(0, 20, "0123456789", 10, out));
    CheckTrue(r, "rx.gap_dropped", !assembler.Feed(15, 20, "wxyz", 4, out));

    // Oversized messages are dropped before buffering.
    CheckTrue(r, "rx.oversized",
              !assembler.Feed(0, PayloadAssembler::kMaxPayloadBytes + 1, "0123456789", 10, out));
    CheckEqStr(r, "rx.oversized_dropped", "", out);

    // Reset drops any partial state.
    CheckTrue(r, "rx.start_again", !assembler.Feed(0, 20, "0123456789", 10, out));
    assembler.Reset();
    CheckTrue(r, "rx.reset", !assembler.active());
}

}  // namespace

bool RunMqttMessageSelfTest() {
    TestResult r;
    TestParseReportLocal(r);
    TestParseReportNonLocal(r);
    TestParseEvents(r);
    TestParseFailures(r);
    TestParseConfig(r);
    TestTranslateUri(r);
    TestTopics(r);
    TestReassembly(r);

    if (r.passed == r.total) {
        ESP_LOGI(TAG, "MQTT SELFTEST PASS (%d checks)", r.total);
        return true;
    }
    ESP_LOGE(TAG, "MQTT SELFTEST FAIL (%d/%d)", r.passed, r.total);
    return false;
}

}  // namespace tuya_mqtt
