#pragma once

/// INTERNAL (src-only) — testable pieces of the MQTT message path
/// (parsing, topics, fragmented-DATA reassembly); no public includes.

#include <cJSON.h>

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "hub_core/dp_value.h"
#include "tuya_mqtt/tuya_mqtt_client.h"  // MqttConfig

namespace tuya_mqtt {

// ---- Protocol constants ----

/// {"protocol": 4} — device status report.
constexpr int kProtocolDeviceReport = 4;
/// {"protocol": 20} — other events.
constexpr int kProtocolOther = 20;

/// protocol 20 bizCode values.
inline constexpr const char* kBizCodeBindUser = "bindUser";
inline constexpr const char* kBizCodeOnline = "online";
inline constexpr const char* kBizCodeOffline = "offline";
inline constexpr const char* kBizCodeNameUpdate = "nameUpdate";
inline constexpr const char* kBizCodeDpNameUpdate = "dpNameUpdate";
inline constexpr const char* kBizCodeDelete = "delete";

/// True when `biz_code` is one of the six handled codes.
inline bool IsKnownBizCode(const std::string& biz_code) {
    return biz_code == kBizCodeBindUser || biz_code == kBizCodeOnline ||
           biz_code == kBizCodeOffline || biz_code == kBizCodeNameUpdate ||
           biz_code == kBizCodeDpNameUpdate || biz_code == kBizCodeDelete;
}

// ---- Parsed message ----

/// One protocol-4 status item: raw local form {dpId, value, t}
/// (support_local) or standard form {code, value, t}.
struct StatusItem {
    bool has_dp_id = false;
    int dp_id = 0;  ///< raw dpId (local form)
    bool has_code = false;
    std::string code;  ///< standard status code (non-local form)
    bool has_value = false;
    hub_core::DpValue value;  ///< reported value (any JSON type)
    /// Report timestamp in MILLISECONDS; 0 = absent.
    int64_t t = 0;
};

/// Parsed push message. Only the top-level "protocol" and "data" keys are
/// read per the Tuya MQTT link protocol; the fields below feed the dispatch.
struct MqttMessage {
    int protocol = 0;  ///< 0 when absent (the dispatch ignores it)

    // protocol 4
    std::string dev_id;
    std::vector<StatusItem> items;

    // protocol 20
    std::string biz_code;
    /// bizData.name when present (nameUpdate).
    std::string biz_name;
};

/// cJSON node -> DpValue: bool->kBool, integral->kInt, other numbers->
/// kDouble, string->kString; null/absent/containers -> empty (use default).
hub_core::DpValue DpValueFromJsonNode(const cJSON* node);

/// Parses a UTF-8 JSON push payload; false = drop (not an object, or a
/// dispatch-read field is missing: p4 devId+status, p20 bizData.devId).
bool ParseMqttMessage(const char* payload, size_t len, MqttMessage& out);

// ---- Access-config parsing ----

/// Parses the `result` of /v1.0/m/life/ha/access/config (incl. the
/// ssl:// -> mqtts:// translation); false when a MANDATORY field is missing.
bool ParseMqttConfig(const cJSON* result, MqttConfig& out);

/// "ssl://host[:port]" -> "mqtts://..."; "mqtt://" passes through;
/// ports default to 8883/1883. False for an unknown scheme.
bool TranslateBrokerUri(const std::string& url_raw, std::string& broker_uri, std::string& host,
                        uint16_t& port);

// ---- Topic building ----

/// Substitutes the template's "{ownerId}"/"{devId}" placeholders
/// (every occurrence of each).
std::string FormatTopicTemplate(const std::string& templ, std::string_view id);

/// Device topic: template + "/pen" (support_local — raw dpId reports)
/// or "/sta" (standard code reports).
std::string DeviceTopic(const std::string& templ, std::string_view dev_id, bool support_local);

// ---- Fragmented DATA-event reassembly (esp-mqtt MQTT_EVENT_DATA) ----

/// Reassembles one MQTT message from multiple MQTT_EVENT_DATA events;
/// ONE instance serves the connection (mqtt task, not thread-safe).
class PayloadAssembler {
public:
    /// Hard cap: a Tuya push payload is a few hundred bytes; bigger is
    /// bogus and dropped whole.
    static constexpr int kMaxPayloadBytes = 16 * 1024;

    /// Feeds one DATA event chunk. Returns true when THIS call completes
    /// the message (`out` then holds the full payload).
    bool Feed(int current_data_offset, int total_data_len, const char* data, int data_len,
              std::string& out);

    /// Drops any partially reassembled message.
    void Reset();

    /// True while a multi-chunk message is in progress.
    bool active() const { return total_len_ > 0; }

private:
    std::string buffer_;
    int total_len_ = 0;  // 0 = no message in progress
};

}  // namespace tuya_mqtt
