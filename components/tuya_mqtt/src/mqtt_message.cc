// MQTT message path: payload/config parsing, topics, fragmented-DATA reassembly (mqtt_message.h).

#include <cJSON.h>
#include <esp_log.h>

#include <climits>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <set>
#include <string>
#include <string_view>
#include <utility>

#include "hub_core/device_registry.h"
#include "hub_core/strategies.h"
#include "tuya_crypto/json_fields.h"
#include "tuya_crypto/json_guard.h"

#include "mqtt_message.h"

static const char* TAG = "TuyaMqtt";

namespace tuya_mqtt {

namespace {

// Guarded number -> int (naive casts truncate fractions / saturate overflow).
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

}  // namespace

hub_core::DpValue DpValueFromJsonNode(const cJSON* node) {
    if (node == nullptr || cJSON_IsNull(node)) {
        return hub_core::DpValue::FromString("");
    }
    if (cJSON_IsBool(node)) {
        return hub_core::DpValue::FromBool(cJSON_IsTrue(node));
    }
    if (cJSON_IsNumber(node)) {
        const double value = node->valuedouble;
        if (std::isfinite(value) && value == std::floor(value) &&
            value >= static_cast<double>(INT64_MIN) && value < static_cast<double>(INT64_MAX)) {
            return hub_core::DpValue::FromInt(static_cast<int64_t>(value));
        }
        return hub_core::DpValue::FromDouble(value);
    }
    if (cJSON_IsString(node)) {
        return hub_core::DpValue::FromString(node->valuestring != nullptr ? node->valuestring : "");
    }
    return hub_core::DpValue::FromString("");
}

bool ParseMqttMessage(const char* payload, size_t len, MqttMessage& out) {
    out = MqttMessage{};
    if (!tuya_crypto::JsonNestingWithinLimit(payload, len, tuya_crypto::kMaxJsonDepth)) {
        ESP_LOGW(TAG, "mqtt payload nesting over %d — dropped", tuya_crypto::kMaxJsonDepth);
        return false;
    }
    tuya_crypto::JsonRoot root_guard;
    if (!tuya_crypto::ParseJsonObject(std::string_view(payload, len), root_guard)) {
        // false = drop the message, keep the client.
        return false;
    }
    cJSON* root = root_guard.node;

    // Only these two keys are read (per the Tuya MQTT link protocol).
    out.protocol = static_cast<int>(tuya_crypto::GetInt64Field(root, "protocol"));
    const cJSON* data = cJSON_GetObjectItemCaseSensitive(root, "data");

    if (out.protocol == kProtocolDeviceReport) {
        // A missing key mirrors as a parse failure.
        if (!cJSON_IsObject(data)) {
            return false;
        }
        out.dev_id = tuya_crypto::GetStringField(data, "devId");
        const cJSON* status = cJSON_GetObjectItemCaseSensitive(data, "status");
        if (out.dev_id.empty() || !cJSON_IsArray(status)) {
            return false;
        }
        for (const cJSON* entry = status->child; entry != nullptr; entry = entry->next) {
            if (!cJSON_IsObject(entry)) {
                continue;
            }
            StatusItem item;
            const cJSON* dp_id = cJSON_GetObjectItemCaseSensitive(entry, "dpId");
            if (JsonIntValue(dp_id, item.dp_id)) {
                item.has_dp_id = true;
            }
            const cJSON* code = cJSON_GetObjectItemCaseSensitive(entry, "code");
            if (cJSON_IsString(code) && code->valuestring != nullptr) {
                item.has_code = true;
                item.code = code->valuestring;
            }
            const cJSON* value = cJSON_GetObjectItemCaseSensitive(entry, "value");
            // A JSON null is "no value", exactly like a missing key.
            if (value != nullptr && !cJSON_IsNull(value)) {
                item.has_value = true;
                item.value = DpValueFromJsonNode(value);
            }
            item.t = tuya_crypto::GetInt64Field(entry, "t");  // ms; 0 = absent
            out.items.push_back(std::move(item));
        }
        return true;
    }

    if (out.protocol == kProtocolOther) {
        // bizCode membership is checked before bizData is touched.
        if (!cJSON_IsObject(data)) {
            return false;
        }
        out.biz_code = tuya_crypto::GetStringField(data, "bizCode");
        if (out.biz_code.empty()) {
            return false;  // missing bizCode -> drop
        }
        const cJSON* biz_data = cJSON_GetObjectItemCaseSensitive(data, "bizData");
        if (IsKnownBizCode(out.biz_code)) {
            if (!cJSON_IsObject(biz_data)) {
                return false;  // malformed bizData -> drop
            }
            out.dev_id = tuya_crypto::GetStringField(biz_data, "devId");
            if (out.dev_id.empty()) {
                return false;
            }
            out.biz_name = tuya_crypto::GetStringField(biz_data, "name");
        }
        // An UNKNOWN bizCode never touches bizData: parsed, then ignored.
        return true;
    }

    // Other protocols (or none): parsed and ignored by the dispatch.
    return true;
}

bool ParseMqttConfig(const cJSON* result, MqttConfig& out) {
    if (!cJSON_IsObject(result)) {
        return false;
    }
    out = MqttConfig{};
    out.url_raw = tuya_crypto::GetStringField(result, "url");
    out.client_id = tuya_crypto::GetStringField(result, "clientId");
    out.username = tuya_crypto::GetStringField(result, "username");
    out.password = tuya_crypto::GetStringField(result, "password");
    out.expire_time_s = tuya_crypto::GetInt64Field(result, "expireTime");
    // result["topic"]["ownerId"/"devId"]["sub"] — server-provided templates.
    const cJSON* topic = cJSON_GetObjectItemCaseSensitive(result, "topic");
    const cJSON* owner = cJSON_GetObjectItemCaseSensitive(topic, "ownerId");
    out.owner_topic_template = tuya_crypto::GetStringField(owner, "sub");
    const cJSON* dev = cJSON_GetObjectItemCaseSensitive(topic, "devId");
    out.dev_topic_template = tuya_crypto::GetStringField(dev, "sub");

    if (!TranslateBrokerUri(out.url_raw, out.broker_uri, out.host, out.port)) {
        return false;
    }
    // A config without credentials or templates cannot serve the cycle.
    if (out.client_id.empty() || out.username.empty() || out.password.empty() ||
        out.owner_topic_template.empty() || out.dev_topic_template.empty() ||
        out.expire_time_s <= 0) {
        return false;
    }
    return true;
}

namespace {

// atoi silently wraps out-of-range ports; 0 = unusable (the caller rejects it).
uint16_t ParseBrokerPort(std::string_view text) {
    const std::string str(text);
    char* end = nullptr;
    const long value = std::strtol(str.c_str(), &end, 10);
    if (end == str.c_str() || *end != '\0' || value <= 0 || value > 65535) {
        return 0;
    }
    return static_cast<uint16_t>(value);
}

}  // namespace

bool TranslateBrokerUri(const std::string& url_raw, std::string& broker_uri, std::string& host,
                        uint16_t& port) {
    broker_uri.clear();
    host.clear();
    port = 0;

    // Tuya "ssl://" means TLS; esp-mqtt spells it "mqtts://".
    std::string_view url(url_raw);
    constexpr std::string_view kSslPrefix = "ssl://";
    constexpr std::string_view kMqttsPrefix = "mqtts://";
    constexpr std::string_view kMqttPrefix = "mqtt://";
    bool tls = false;
    if (url.rfind(kSslPrefix, 0) == 0) {
        url.remove_prefix(kSslPrefix.size());
        tls = true;
    } else if (url.rfind(kMqttsPrefix, 0) == 0) {
        url.remove_prefix(kMqttsPrefix.size());
        tls = true;
    } else if (url.rfind(kMqttPrefix, 0) == 0) {
        url.remove_prefix(kMqttPrefix.size());
    } else {
        ESP_LOGW(TAG, "unsupported broker url scheme: %.32s", url_raw.c_str());
        return false;
    }

    // authority = host[:port][/path]: cut the path, then split the port off
    // the last ':' (a bracketed IPv6 authority keeps its inner ':'s).
    const size_t slash = url.find('/');
    const std::string_view authority = slash == std::string_view::npos ? url : url.substr(0, slash);
    const uint16_t default_port = tls ? 8883 : 1883;  // standard MQTT ports
    uint16_t parsed_port = default_port;
    std::string_view host_view = authority;
    if (!authority.empty() && authority.front() == '[') {
        const size_t close = authority.find(']');
        if (close != std::string_view::npos) {
            host_view = authority.substr(1, close - 1);
            if (close + 1 < authority.size() && authority[close + 1] == ':') {
                parsed_port = ParseBrokerPort(authority.substr(close + 2));
            }
        }
    } else {
        const size_t colon = authority.rfind(':');
        if (colon != std::string_view::npos) {
            host_view = authority.substr(0, colon);
            parsed_port = ParseBrokerPort(authority.substr(colon + 1));
        }
    }
    if (host_view.empty() || parsed_port == 0) {
        ESP_LOGW(TAG, "broker url without a usable host/port: %.48s", url_raw.c_str());
        return false;
    }

    host = std::string(host_view);
    port = parsed_port;
    broker_uri = (tls ? "mqtts://" : "mqtt://") + std::string(authority);
    return true;
}

std::string FormatTopicTemplate(const std::string& templ, std::string_view id) {
    // Every occurrence of the placeholder is replaced.
    std::string topic(templ);
    constexpr std::string_view kPlaceholders[] = {"{ownerId}", "{devId}"};
    for (const std::string_view placeholder : kPlaceholders) {
        const std::string needle(placeholder);
        size_t pos = topic.find(needle);
        while (pos != std::string::npos) {
            topic.replace(pos, needle.size(), id);
            pos = topic.find(needle, pos + id.size());
        }
    }
    return topic;
}

std::string DeviceTopic(const std::string& templ, std::string_view dev_id, bool support_local) {
    // + "/pen" (support_local: raw dpId reports) or "/sta".
    std::string topic = FormatTopicTemplate(templ, dev_id);
    topic += support_local ? "/pen" : "/sta";
    return topic;
}

namespace {

/// One-shot report warnings (mqtt task only: ApplyDeviceReport has one
/// caller, so no mutex): per device, and per (device, dpId).
std::set<std::string> s_report_warned;
std::set<std::pair<std::string, int>> s_dp_warned;

/// True when `topic` ends with one of the two device suffixes this client
/// subscribes; `support_local` is set to the suffix's implied form.
bool TopicSupportLocal(std::string_view topic, bool& support_local) {
    if (topic.ends_with("/pen")) {
        support_local = true;
        return true;
    }
    if (topic.ends_with("/sta")) {
        support_local = false;
        return true;
    }
    return false;
}

}  // namespace

void ApplyDeviceReport(const MqttMessage& msg, std::string_view topic) {
    hub_core::DeviceRegistry& registry = hub_core::DeviceRegistry::GetInstance();
    hub_core::Device device;
    if (!registry.GetDevice(msg.dev_id, device)) {
        ESP_LOGD(TAG, "report for unknown device %s dropped", msg.dev_id.c_str());
        return;
    }
    bool topic_local = false;
    if (TopicSupportLocal(topic, topic_local) && topic_local != device.support_local &&
        s_report_warned.insert(msg.dev_id).second) {
        // The cloud does not pick the suffix by support_local: never drop on it.
        ESP_LOGW(TAG, "device %s: report on %.*s with support_local=%d — applying by item shape",
                 msg.dev_id.c_str(), static_cast<int>(topic.size()), topic.data(),
                 device.support_local ? 1 : 0);
    }
    for (const StatusItem& item : msg.items) {
        if (!item.has_dp_id && !item.has_code) {
            if (s_report_warned.insert(msg.dev_id).second) {
                ESP_LOGW(TAG, "dev=%s item has neither dpId nor code", msg.dev_id.c_str());
            }
            continue;
        }
        if (!item.has_value) {
            continue;
        }
        if (item.has_dp_id) {
            std::string code;
            hub_core::DpValue value;
            if (hub_core::ConvertDpReport(device.id, device.local_strategy, device.status_range,
                                          item.dp_id, item.value, code, value)) {
                registry.UpdateStatus(msg.dev_id, code, value, item.t);
                continue;
            }
            // An item can carry both keys: never lose an update we can apply.
            if (!item.has_code) {
                if (s_dp_warned.insert({msg.dev_id, item.dp_id}).second) {
                    ESP_LOGW(TAG, "dev=%s dpId=%d report dropped (no usable strategy)",
                             msg.dev_id.c_str(), item.dp_id);
                }
                continue;
            }
        }
        if (item.has_code) {
            registry.UpdateStatus(msg.dev_id, item.code, item.value, item.t);
        }
    }
}

bool PayloadAssembler::Feed(int current_data_offset, int total_data_len, const char* data,
                            int data_len, std::string& out) {
    out.clear();
    if (total_data_len <= 0 || data == nullptr || data_len <= 0) {
        return false;  // defensive: esp-mqtt never sends these
    }
    if (total_data_len > kMaxPayloadBytes) {
        ESP_LOGW(TAG, "dropping oversized mqtt payload (%d B)", total_data_len);
        Reset();
        return false;
    }

    // Whole message in one event (the common case).
    if (current_data_offset == 0) {
        if (data_len >= total_data_len) {
            // An interrupted earlier message must not splice into this one.
            Reset();
            out.assign(data, static_cast<size_t>(total_data_len));
            return true;
        }
        buffer_.assign(data, static_cast<size_t>(data_len));
        total_len_ = total_data_len;
        return false;
    }

    // Continuation chunk of an in-progress message.
    if (!active() || total_len_ != total_data_len) {
        // Lost head or mismatched stream — drop the message whole.
        ESP_LOGW(TAG, "dropping orphaned mqtt data chunk (offset=%d total=%d)", current_data_offset,
                 total_data_len);
        Reset();
        return false;
    }
    const size_t expected = static_cast<size_t>(current_data_offset);
    if (buffer_.size() != expected) {
        ESP_LOGW(TAG, "dropping mqtt message with a gap (have %u, chunk at %d)",
                 static_cast<unsigned>(buffer_.size()), current_data_offset);
        Reset();
        return false;
    }
    buffer_.append(data, static_cast<size_t>(data_len));
    if (static_cast<int>(buffer_.size()) >= total_len_) {
        out = std::move(buffer_);
        buffer_.clear();
        total_len_ = 0;
        return true;
    }
    return false;
}

void PayloadAssembler::Reset() {
    buffer_.clear();
    buffer_.shrink_to_fit();
    total_len_ = 0;
}

}  // namespace tuya_mqtt
