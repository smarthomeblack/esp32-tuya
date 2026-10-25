#include "tuya_mqtt/tuya_mqtt_client.h"

#include <esp_crt_bundle.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <mqtt_client.h>

#include <freertos/FreeRTOS.h>
#include <freertos/event_groups.h>
#include <freertos/queue.h>
#include <freertos/task.h>

#include <cJSON.h>

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "hub_core/device_registry.h"
#include "hub_core/main_thread.h"
#include "hub_core/parse.h"
#include "tuya_cloud/api_response.h"
#include "tuya_cloud/constants.h"
#include "tuya_cloud/customer_api.h"
#include "tuya_cloud/http_method.h"
#include "tuya_cloud/paths.h"
#include "tuya_crypto/json_fields.h"
#include "tuya_crypto/json_writer.h"
#include "tuya_crypto/uuid.h"

#include "mqtt_message.h"

static const char* TAG = "TuyaMqtt";

namespace tuya_mqtt {

using hub_core::Device;
using hub_core::DeviceRegistry;
using tuya_cloud::ApiResponse;
using tuya_cloud::CustomerApi;
using tuya_cloud::DevicePath;
using tuya_cloud::HttpMethod;

namespace {

// ---- Tunables ----

/// Management task stack (bytes): esp_http_client + mbedTLS + cJSON run
/// here during config fetches; also the esp-mqtt task stack.
constexpr uint32_t kMgmtTaskStackBytes = 12288;
constexpr UBaseType_t kMgmtTaskPriority = 3;

/// Config-fetch retries: 3 attempts, 1 s apart.
constexpr int kConfigFetchAttempts = 3;
constexpr uint32_t kConfigRetryDelayMs = 1000;

/// Seconds before expireTime at which the cycle refreshes.
constexpr int64_t kExpireSlackSeconds = 60;
/// Cycle floor: a bogus expireTime must not spin the loop.
constexpr int64_t kMinCycleSeconds = 30;
/// Cycle cap (defensive upper bound on a bogus expireTime, ~1 day).
constexpr int64_t kMaxExpireSeconds = 86400;

/// MQTT connect timeout (ms); 30 s covers one mbedTLS handshake.
constexpr uint32_t kConnectTimeoutMs = 30000;
/// esp-mqtt network timeout (ms): a stuck socket must not freeze the client task.
constexpr int kNetworkTimeoutMs = 30000;
/// esp-mqtt auto-reconnect delay (ms) — esp-mqtt's own default, kept.
constexpr int kReconnectTimeoutMs = 10000;
/// MQTT keepalive (s); the cloud config never supplies one.
constexpr int kKeepaliveSeconds = 60;
/// esp-mqtt client buffer (default 1024); 4096 fits typical status reports.
constexpr int kClientBufferBytes = 4096;
/// Device topics per SUBSCRIBE packet.
constexpr size_t kSubscribeBatchSize = 20;

/// bindUser deferral queue; the management task drains it at 1 Hz.
constexpr int kBindUserQueueLength = 8;

/// Event bits the esp-mqtt event handler sets for the management task.
constexpr EventBits_t kBitConnected = 1u << 0;
constexpr EventBits_t kBitAuthError = 1u << 1;  // broker refused credentials

/// Hard bound on one JSON re-serialization (bogus huge API result).
constexpr size_t kMaxJsonTextBytes = 128 * 1024;

/// Query plaintext {"devIds":"<id>"} — exact bytes matter (server contract).
std::string DevIdsParams(const std::string& dev_id) {
    tuya_crypto::JsonWriter writer;
    writer.BeginObject();
    writer.Key("devIds");
    writer.String(dev_id);
    writer.EndObject();
    return writer.Str();
}

/// linkId body {"linkId":"tuya-device-sharing-sdk-esp32.<uuid4>"}; any
/// unique string, regenerated per fetch.
std::string BuildLinkIdBody() {
    tuya_crypto::JsonWriter writer;
    writer.BeginObject();
    writer.Key("linkId");
    writer.String("tuya-device-sharing-sdk-esp32." + tuya_crypto::Uuid4());
    writer.EndObject();
    return writer.Str();
}

/// ApiResponse result node -> bounded JSON text (the hub_core parse
/// wrappers are cJSON-free); false when null/failed/oversized.
bool ResultToText(const cJSON* result, std::string& out) {
    if (result == nullptr) {
        return false;
    }
    tuya_crypto::JsonText text(result);
    out = text.str();
    if (out.empty() || out.size() > kMaxJsonTextBytes) {
        out.clear();
        return false;
    }
    return true;
}

/// Overlays the cloud-owned identity fields, online and timestamps; set_up,
/// disabled entities and strategy maps stay. `sub` is cloud-derived, preserved
/// by design.
void OverlayCloudIdentity(Device& device, const Device& cloud) {
    device.name = cloud.name;
    device.category = cloud.category;
    device.product_id = cloud.product_id;
    device.product_name = cloud.product_name;
    device.model = cloud.model;
    device.uuid = cloud.uuid;
    device.asset_id = cloud.asset_id;
    device.icon = cloud.icon;
    device.time_zone = cloud.time_zone;
    device.online = cloud.online;
    device.active_time = cloud.active_time;
    device.create_time = cloud.create_time;
    device.update_time = cloud.update_time;
}

/// File-scope reassembly state for fragmented DATA events (mqtt task
/// only; keeps the internal type out of the public header).
PayloadAssembler s_rx_assembler;
/// Topic of the in-progress DATA message (mqtt task only); esp-mqtt carries
/// it on the first fragment, so it lives until the message completes.
std::string s_rx_topic;

}  // namespace

// ---- TuyaMqttClient — lifecycle ----

TuyaMqttClient::TuyaMqttClient() {
    events_ = xEventGroupCreate();
    bind_user_queue_ = xQueueCreate(kBindUserQueueLength, sizeof(BindUserItem));
    if (events_ == nullptr || bind_user_queue_ == nullptr) {
        ESP_LOGE(TAG, "Failed to create the mqtt event group / bindUser queue");
        abort();  // boot-time OOM, unrecoverable
    }
}

TuyaMqttClient::~TuyaMqttClient() {
    Stop();
    if (events_ != nullptr) {
        vEventGroupDelete(static_cast<EventGroupHandle_t>(events_));
        events_ = nullptr;
    }
    if (bind_user_queue_ != nullptr) {
        vQueueDelete(static_cast<QueueHandle_t>(bind_user_queue_));
        bind_user_queue_ = nullptr;
    }
}

bool TuyaMqttClient::Start() {
    bool expected = false;
    if (!running_.compare_exchange_strong(expected, true)) {
        if (stop_requested_.load()) {
            ESP_LOGW(TAG, "MQTT still shutting down — Start() ignored");
        } else {
            ESP_LOGW(TAG, "MQTT already running — Start() ignored");
        }
        return false;
    }
    stop_requested_.store(false);
    xEventGroupClearBits(static_cast<EventGroupHandle_t>(events_), kBitConnected | kBitAuthError);
    TaskHandle_t mgmt_task = nullptr;
    const BaseType_t ok = xTaskCreate(MgmtTaskTrampoline, "tuya_mqtt_mgmt", kMgmtTaskStackBytes,
                                      this, kMgmtTaskPriority, &mgmt_task);
    if (ok != pdPASS) {
        running_.store(false);
        ESP_LOGE(TAG, "Failed to create the mqtt management task");
        return false;
    }
    mgmt_task_ = mgmt_task;
    ESP_LOGI(TAG, "MQTT management task started (tuya_mqtt_mgmt)");
    return true;
}

void TuyaMqttClient::Stop() {
    hub_core::AssertMainThread("TuyaMqttClient::Stop");
    if (!running_.load() && mgmt_task_ == nullptr) {
        return;  // never started / already stopped
    }
    stop_requested_.store(true);
    // The management task polls stop_requested_ in 1 s slices and tears
    // the client down itself (stop/destroy must not run on the handler).
    for (int i = 0; i < 50 && running_.load(); ++i) {
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    if (running_.load()) {
        // Still winding down: keep mgmt_task_ so a refused Start can say so.
        ESP_LOGE(TAG, "MQTT management task did not stop within 5 s — still shutting down");
    } else {
        mgmt_task_ = nullptr;  // only clear once the task has really exited
    }
}

void TuyaMqttClient::SetStatusListener(StatusListener listener) {
    std::lock_guard<std::mutex> lock(status_mutex_);
    status_listener_ = std::move(listener);
}

MqttStatus TuyaMqttClient::GetStatus() const {
    std::lock_guard<std::mutex> lock(status_mutex_);
    return status_;
}

void TuyaMqttClient::NotifyStatus(bool connected, int error_code, std::string error) {
    MqttStatus snapshot;
    StatusListener listener;
    {
        std::lock_guard<std::mutex> lock(status_mutex_);
        status_.connected = connected;
        status_.error_code = error_code;
        status_.error = std::move(error);
        snapshot = status_;
        listener = status_listener_;
    }
    if (listener) {
        listener(snapshot);  // outside the lock — listener contract
    }
}

void TuyaMqttClient::MgmtTaskTrampoline(void* arg) {
    auto* self = static_cast<TuyaMqttClient*>(arg);
    self->MgmtTask();
    self->running_.store(false);
    vTaskDelete(nullptr);
}

void TuyaMqttClient::MgmtTask() {
    ESP_LOGI(TAG, "MQTT management loop entered");
    while (!stop_requested_.load()) {
        // ---- config fetch ----
        MqttConfig config;
        if (!FetchConfig(config)) {
            ESP_LOGW(TAG, "mqtt config fetch failed after %d attempts — retrying in 1 s",
                     kConfigFetchAttempts);
            vTaskDelay(pdMS_TO_TICKS(kConfigRetryDelayMs));
            continue;
        }
        ESP_LOGI(TAG, "mqtt config: broker=%s expire=%llds owner_tpl=%s dev_tpl=%s",
                 config.broker_uri.c_str(), static_cast<long long>(config.expire_time_s),
                 config.owner_topic_template.c_str(), config.dev_topic_template.c_str());

        // ---- connect cycle ----
        const CycleResult cycle = StartCycle(config);
        if (cycle == CycleResult::kFailed) {
            vTaskDelay(pdMS_TO_TICKS(kConfigRetryDelayMs));
            continue;
        }
        if (cycle == CycleResult::kAuthRefused) {
            // Re-run with a fresh config; the delay keeps broker hiccups
            // from busy-looping the cycle.
            vTaskDelay(pdMS_TO_TICKS(kConfigRetryDelayMs));
            continue;
        }

        // ---- hold the connection ----
        int64_t cycle_seconds = config.expire_time_s - kExpireSlackSeconds;
        if (cycle_seconds < kMinCycleSeconds) {
            ESP_LOGW(TAG, "bogus expireTime %lld s — clamping the cycle to %lld s",
                     static_cast<long long>(config.expire_time_s),
                     static_cast<long long>(kMinCycleSeconds));
            cycle_seconds = kMinCycleSeconds;
        } else if (cycle_seconds > kMaxExpireSeconds) {
            cycle_seconds = kMaxExpireSeconds;
        }
        const int64_t deadline_us = esp_timer_get_time() + cycle_seconds * 1000000LL;
        ESP_LOGI(TAG, "mqtt connected — next config refresh in %lld s",
                 static_cast<long long>(cycle_seconds));

        bool auth_refetch = false;
        while (!stop_requested_.load()) {
            // Drain deferred bindUser work; one item at a time (each
            // refetch sleeps 1 s + 4 HTTP calls).
            BindUserItem item;
            while (xQueueReceive(static_cast<QueueHandle_t>(bind_user_queue_), &item, 0) ==
                   pdTRUE) {
                ProcessBindUser(item.dev_id);
            }
            if (stop_requested_.load()) {
                break;
            }

            const int64_t now_us = esp_timer_get_time();
            if (now_us >= deadline_us) {
                break;  // (a) deadline: stop + refetch a fresh config
            }
            int64_t slice_ms = (deadline_us - now_us) / 1000;
            if (slice_ms > 1000) {
                slice_ms = 1000;  // poll stop_/queue at 1 Hz
            }
            const EventBits_t bits =
                xEventGroupWaitBits(static_cast<EventGroupHandle_t>(events_), kBitAuthError, pdTRUE,
                                    pdFALSE, pdMS_TO_TICKS(static_cast<uint32_t>(slice_ms)));
            if (bits & kBitAuthError) {
                // (b) broker refused the credentials — short-circuit the deadline.
                ESP_LOGW(TAG, "broker refused credentials — refetching mqtt config");
                auth_refetch = true;
                break;
            }
            // Transient disconnects: esp-mqtt auto-reconnects and the
            // CONNECTED handler re-subscribes; the deadline caps the cycle.
        }
        if (auth_refetch || stop_requested_.load()) {
            continue;  // auth: fresh config now; stop: loop condition exits
        }
        ESP_LOGI(TAG, "mqtt cycle expired — refreshing config");
    }

    DestroyClient();
    connected_.store(false);
    NotifyStatus(false, 0, "stopped");
    ESP_LOGI(TAG, "MQTT management loop exited");
}

bool TuyaMqttClient::FetchConfig(MqttConfig& out) {
    CustomerApi& api = CustomerApi::GetInstance();
    for (int attempt = 1; attempt <= kConfigFetchAttempts; ++attempt) {
        if (stop_requested_.load()) {
            return false;
        }
        // Success = HTTP 2xx AND top-level success == true.
        ApiResponse resp = api.Request(HttpMethod::kPost, std::string(tuya_cloud::kPathMqttConfig),
                                       "", BuildLinkIdBody());
        if (resp.ok && resp.result != nullptr && cJSON_IsObject(resp.result) &&
            ParseMqttConfig(resp.result, out)) {
            return true;
        }
        ESP_LOGW(TAG, "mqtt config fetch attempt %d/%d failed (%s)", attempt, kConfigFetchAttempts,
                 resp.msg.c_str());
        vTaskDelay(pdMS_TO_TICKS(kConfigRetryDelayMs));
    }
    return false;
}

TuyaMqttClient::CycleResult TuyaMqttClient::StartCycle(const MqttConfig& config) {
    DestroyClient();  // no client may outlive its credentials

    // Written before the client exists; the handler reads it during the
    // cycle (destroyed before rewrite).
    {
        std::lock_guard<std::mutex> lock(config_mutex_);
        config_ = config;
    }
    s_rx_assembler.Reset();
    xEventGroupClearBits(static_cast<EventGroupHandle_t>(events_), kBitConnected | kBitAuthError);

    // esp_mqtt_client_init deep-copies (strdups) its string fields, so
    // pointing at this cycle's storage is safe.
    esp_mqtt_client_config_t mqtt_cfg = {};
    mqtt_cfg.broker.address.uri = config.broker_uri.c_str();  // mqtts://host:port
    // Default CA verification via the cert bundle.
    mqtt_cfg.broker.verification.crt_bundle_attach = esp_crt_bundle_attach;
    mqtt_cfg.credentials.client_id = config.client_id.c_str();
    mqtt_cfg.credentials.username = config.username.c_str();
    mqtt_cfg.credentials.authentication.password = config.password.c_str();
    mqtt_cfg.session.protocol_ver = MQTT_PROTOCOL_V_3_1_1;
    // Clean session stays enabled (default).
    mqtt_cfg.session.keepalive = kKeepaliveSeconds;
    mqtt_cfg.network.timeout_ms = kNetworkTimeoutMs;
    mqtt_cfg.network.reconnect_timeout_ms = kReconnectTimeoutMs;
    // auto-reconnect stays enabled; the deadline caps the cycle.
    mqtt_cfg.buffer.size = kClientBufferBytes;
    // The esp-mqtt task also runs the TLS handshake + dispatch; give it
    // the same stack headroom.
    mqtt_cfg.task.stack_size = static_cast<int>(kMgmtTaskStackBytes);

    client_ = esp_mqtt_client_init(&mqtt_cfg);
    if (client_ == nullptr) {
        ESP_LOGE(TAG, "esp_mqtt_client_init failed");
        return CycleResult::kFailed;
    }
    const esp_err_t reg_err =
        esp_mqtt_client_register_event(client_, MQTT_EVENT_ANY, MqttEventHandler, this);
    if (reg_err != ESP_OK) {
        ESP_LOGE(TAG, "esp_mqtt_client_register_event failed: %s", esp_err_to_name(reg_err));
        DestroyClient();
        return CycleResult::kFailed;
    }

    connected_.store(false);
    const esp_err_t err = esp_mqtt_client_start(client_);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_mqtt_client_start failed: %s", esp_err_to_name(err));
        DestroyClient();
        return CycleResult::kFailed;
    }

    // Wait for CONNECTED (the handler subscribes), a refusal, or the
    // timeout — in 1 s slices so Stop() stays responsive.
    const int64_t deadline_ms = esp_timer_get_time() / 1000 + kConnectTimeoutMs;
    while (true) {
        int64_t slice_ms = deadline_ms - esp_timer_get_time() / 1000;
        if (slice_ms <= 0) {
            ESP_LOGW(TAG, "mqtt connect timed out after %u ms",
                     static_cast<unsigned>(kConnectTimeoutMs));
            DestroyClient();
            return CycleResult::kFailed;
        }
        if (slice_ms > 1000) {
            slice_ms = 1000;
        }
        const EventBits_t bits = xEventGroupWaitBits(
            static_cast<EventGroupHandle_t>(events_), kBitConnected | kBitAuthError, pdTRUE,
            pdFALSE, pdMS_TO_TICKS(static_cast<uint32_t>(slice_ms)));
        if (bits & kBitConnected) {
            return CycleResult::kConnected;
        }
        if (bits & kBitAuthError) {
            DestroyClient();
            return CycleResult::kAuthRefused;
        }
        if (stop_requested_.load()) {
            DestroyClient();
            return CycleResult::kFailed;
        }
    }
}

void TuyaMqttClient::DestroyClient() {
    if (client_ == nullptr) {
        return;
    }
    // Forbidden from the event handler — management task only.
    esp_mqtt_client_stop(client_);
    esp_mqtt_client_destroy(client_);
    client_ = nullptr;
    connected_.store(false);
}

// ---- esp-mqtt event handler (runs on the esp-mqtt task) ----

void TuyaMqttClient::MqttEventHandler(void* arg, const char* event_base, int32_t event_id,
                                      void* event_data) {
    auto* self = static_cast<TuyaMqttClient*>(arg);
    auto* event = static_cast<esp_mqtt_event_t*>(event_data);
    (void)event_base;

    switch (static_cast<esp_mqtt_event_id_t>(event_id)) {
        case MQTT_EVENT_CONNECTED: {
            self->connected_.store(true);
            s_rx_assembler.Reset();  // a fresh session has no partial message
            s_rx_topic.clear();
            self->NotifyStatus(true, 0, "");
            // Wake the connect wait; subscribing happens right here.
            xEventGroupSetBits(static_cast<EventGroupHandle_t>(self->events_), kBitConnected);
            self->SubscribeAll(event->client);
            break;
        }

        case MQTT_EVENT_DISCONNECTED: {
            // Unexpected disconnection: esp-mqtt auto-reconnects and the
            // next CONNECTED re-subscribes.
            if (self->connected_.exchange(false)) {
                self->NotifyStatus(false, 0, "disconnected");
            }
            break;
        }

        case MQTT_EVENT_ERROR: {
            // Only rc 4/5 mean stale credentials; other refusals follow the
            // broker reconnect backoff.
            const esp_mqtt_error_codes_t* err = event->error_handle;
            if (err != nullptr && err->error_type == MQTT_ERROR_TYPE_CONNECTION_REFUSED) {
                const int rc = err->connect_return_code;
                ESP_LOGE(TAG, "mqtt connection refused: rc=%d", rc);
                self->NotifyStatus(false, rc, "connection refused");
                if (rc == MQTT_CONNECTION_REFUSE_BAD_USERNAME ||
                    rc == MQTT_CONNECTION_REFUSE_NOT_AUTHORIZED) {
                    xEventGroupSetBits(static_cast<EventGroupHandle_t>(self->events_),
                                       kBitAuthError);
                }
            } else if (err != nullptr && err->error_type == MQTT_ERROR_TYPE_TCP_TRANSPORT) {
                // TLS/socket-level trouble: esp-mqtt retries — log only.
                ESP_LOGW(TAG, "mqtt transport error (esp_tls=0x%x errno=%d)",
                         static_cast<unsigned>(err->esp_tls_last_esp_err),
                         err->esp_transport_sock_errno);
            } else if (err != nullptr && err->error_type == MQTT_ERROR_TYPE_SUBSCRIBE_FAILED) {
                ESP_LOGW(TAG, "mqtt subscribe failed (msg_id=%d)", event->msg_id);
            } else {
                ESP_LOGW(TAG, "mqtt error event (type=%d)",
                         err != nullptr ? static_cast<int>(err->error_type) : -1);
            }
            break;
        }

        case MQTT_EVENT_DATA: {
            // Reassemble fragmented DATA events, then dispatch. The client
            // handle comes from THIS event (never the cross-task client_).
            if (event->topic != nullptr && event->topic_len > 0) {
                s_rx_topic.assign(event->topic, static_cast<size_t>(event->topic_len));
                ESP_LOGI(TAG, "mqtt data topic=%.*s off=%d len=%d total=%d", event->topic_len,
                         event->topic, event->current_data_offset, event->data_len,
                         event->total_data_len);
            }
            std::string payload;
            if (s_rx_assembler.Feed(event->current_data_offset, event->total_data_len, event->data,
                                    event->data_len, payload)) {
                self->HandlePayload(event->client, s_rx_topic, payload.data(), payload.size());
            }
            break;
        }

        case MQTT_EVENT_SUBSCRIBED:
            ESP_LOGI(TAG, "mqtt subscribed (msg_id=%d)", event->msg_id);
            break;

        case MQTT_EVENT_UNSUBSCRIBED:
            ESP_LOGD(TAG, "mqtt unsubscribed (msg_id=%d)", event->msg_id);
            break;

        case MQTT_EVENT_BEFORE_CONNECT: ESP_LOGD(TAG, "mqtt connecting..."); break;

        default: break;  // PUBLISHED etc. — this channel never publishes
    }
}

void TuyaMqttClient::SubscribeAll(struct esp_mqtt_client* client) {
    DeviceRegistry& registry = DeviceRegistry::GetInstance();
    MqttConfig cfg;
    {
        std::lock_guard<std::mutex> lock(config_mutex_);
        cfg = config_;
    }
    if (cfg.owner_topic_template.empty() && cfg.dev_topic_template.empty()) {
        return;  // no config yet — nothing to subscribe
    }

    // Owner topics: one SUBSCRIBE per home, no suffix.
    if (!cfg.owner_topic_template.empty()) {
        int owner_subs = 0;
        for (const hub_core::Home& home : registry.GetHomes()) {
            const std::string topic = FormatTopicTemplate(cfg.owner_topic_template, home.id);
            const int msg_id = esp_mqtt_client_subscribe_single(client, topic.c_str(), 0);
            if (msg_id >= 0) {
                ++owner_subs;
            } else {
                ESP_LOGW(TAG, "owner subscribe failed for home %s (msg_id=%d)", home.id.c_str(),
                         msg_id);
            }
        }
        ESP_LOGI(TAG, "subscribed %d owner topic(s)", owner_subs);
    }

    // Device topics: only set_up devices, collected under the registry
    // lock (no blocking inside it), subscribed in batches of 20 at QoS 0.
    // Both suffixes per device: the cloud picks one per report, not by flag.
    if (cfg.dev_topic_template.empty()) {
        return;
    }
    std::vector<std::string> topics;
    size_t device_count = 0;
    registry.ForEachDevice([&](const Device& device) {
        if (device.set_up) {
            topics.push_back(DeviceTopic(cfg.dev_topic_template, device.id, true));
            topics.push_back(DeviceTopic(cfg.dev_topic_template, device.id, false));
            ++device_count;
        }
    });
    size_t device_subs = 0;
    for (size_t i = 0; i < topics.size(); i += kSubscribeBatchSize) {
        const size_t n = std::min(kSubscribeBatchSize, topics.size() - i);
        std::vector<esp_mqtt_topic_t> batch(n);
        for (size_t j = 0; j < n; ++j) {
            batch[j].filter = topics[i + j].c_str();
            batch[j].qos = 0;
        }
        const int msg_id =
            esp_mqtt_client_subscribe_multiple(client, batch.data(), static_cast<int>(n));
        if (msg_id >= 0) {
            device_subs += n;
        } else {
            ESP_LOGW(TAG, "device subscribe batch failed (filters %u..%u of %u, msg_id=%d)",
                     static_cast<unsigned>(i), static_cast<unsigned>(i + n - 1),
                     static_cast<unsigned>(topics.size()), msg_id);
        }
    }
    ESP_LOGI(TAG, "subscribed %u device topic(s) for %u tracked device(s)",
             static_cast<unsigned>(device_subs), static_cast<unsigned>(device_count));
}

// ---- Message dispatch ----

void TuyaMqttClient::HandlePayload(struct esp_mqtt_client* client, std::string_view topic,
                                   const char* data, size_t len) {
    MqttMessage msg;
    if (!ParseMqttMessage(data, len, msg)) {
        ESP_LOGW(TAG, "unparsable mqtt payload (%u B) dropped", static_cast<unsigned>(len));
        return;  // drop the message, keep the client
    }
    ESP_LOGI(TAG, "mqtt msg protocol=%d dev=%s items=%u", msg.protocol, msg.dev_id.c_str(),
             static_cast<unsigned>(msg.items.size()));
    if (msg.protocol == kProtocolDeviceReport) {
        ApplyDeviceReport(msg, topic);
        return;
    }
    if (msg.protocol == kProtocolOther) {
        HandleDeviceEvent(client, msg.biz_code, msg.dev_id, msg.biz_name);
        return;
    }
    ESP_LOGD(TAG, "mqtt message with protocol %d ignored", msg.protocol);
}

void TuyaMqttClient::HandleDeviceEvent(struct esp_mqtt_client* client, const std::string& biz_code,
                                       const std::string& dev_id, const std::string& new_name) {
    DeviceRegistry& registry = DeviceRegistry::GetInstance();
    // bindUser first: deferred to the management task (sleep 1 s + HTTP
    // refetch — too heavy for the mqtt task).
    if (biz_code == kBizCodeBindUser) {
        if (dev_id.size() >= sizeof(BindUserItem::dev_id)) {
            ESP_LOGW(TAG, "bindUser devId too long (%u B) — dropped",
                     static_cast<unsigned>(dev_id.size()));
            return;
        }
        BindUserItem item{};
        std::memcpy(item.dev_id, dev_id.c_str(), dev_id.size());
        if (xQueueSend(static_cast<QueueHandle_t>(bind_user_queue_), &item, 0) != pdTRUE) {
            ESP_LOGW(TAG, "bindUser queue full — event dropped");
        } else {
            ESP_LOGD(TAG, "bindUser deferred: %s", dev_id.c_str());
        }
        return;  // the refetch happens off-task
    }

    if (biz_code == kBizCodeOnline || biz_code == kBizCodeOffline) {
        // Unknown ids: registry no-ops.
        registry.SetOnline(dev_id, biz_code == kBizCodeOnline);
        return;
    }
    if (biz_code == kBizCodeNameUpdate) {
        // Never rename to an empty string.
        if (new_name.empty()) {
            ESP_LOGW(TAG, "nameUpdate without a name for %s — ignored", dev_id.c_str());
        } else {
            registry.RenameDevice(dev_id, new_name);
        }
        return;
    }
    if (biz_code == kBizCodeDpNameUpdate) {
        ESP_LOGD(TAG, "dpNameUpdate ignored");
        return;
    }
    if (biz_code == kBizCodeDelete) {
        // Remove from the registry, then unsubscribe both topics that the
        // connect-time subscription added for this device.
        Device device;
        if (!registry.GetDevice(dev_id, device)) {
            return;  // unknown device: nothing to unsubscribe
        }
        MqttConfig cfg;
        {
            std::lock_guard<std::mutex> lock(config_mutex_);
            cfg = config_;
        }
        if (registry.RemoveDevice(dev_id) && client != nullptr && !cfg.dev_topic_template.empty()) {
            for (const bool support_local : {true, false}) {
                const std::string topic =
                    DeviceTopic(cfg.dev_topic_template, dev_id, support_local);
                esp_mqtt_client_unsubscribe(client, topic.c_str());
            }
            ESP_LOGI(TAG, "device %s deleted — unsubscribed both report topics", dev_id.c_str());
        }
        return;
    }
    // Unknown bizCode: parsed but ignored.
    ESP_LOGD(TAG, "bizCode %s ignored", biz_code.c_str());
}

// ---- bindUser refetch — management task ----

void TuyaMqttClient::ProcessBindUser(const std::string& dev_id) {
    // 1 s settle so the server-side sync lands first.
    vTaskDelay(pdMS_TO_TICKS(1000));

    CustomerApi& api = CustomerApi::GetInstance();
    DeviceRegistry& registry = DeviceRegistry::GetInstance();

    // The re-fetched entry keeps the row's client-side state; only the
    // cloud-owned identity and the model maps are replaced.
    ApiResponse resp = api.Request(HttpMethod::kGet, std::string(tuya_cloud::kPathDeviceDetail),
                                   DevIdsParams(dev_id), "");
    std::string text;
    if (!resp.ok || !cJSON_IsArray(resp.result) || !ResultToText(resp.result, text)) {
        ESP_LOGW(TAG, "bindUser %s: detail refetch failed (%s) — ignored", dev_id.c_str(),
                 resp.msg.c_str());
        return;
    }
    Device cloud;
    if (!hub_core::ParseDeviceDetailResultJson(text, cloud)) {
        // The id vanished server-side between event and refetch — skip.
        ESP_LOGD(TAG, "bindUser %s: not present after refetch — nothing to add", dev_id.c_str());
        return;
    }

    Device device;
    if (registry.GetDevice(cloud.id, device)) {
        OverlayCloudIdentity(device, cloud);
        // Codes the cloud snapshot omits keep their last known value.
        for (const auto& item : cloud.status) {
            device.status.insert_or_assign(item.first, item.second);
        }
    } else {
        device = cloud;  // a new id starts from the fresh entry
        // Single-home shortcut; multi-home resolves via the next discovery commit.
        const std::vector<hub_core::Home> homes = registry.GetHomes();
        if (homes.size() == 1) {
            device.home_id = homes.front().id;
        }
    }

    // Re-run the 3 spec calls; a failed call keeps the maps the row has.
    ApiResponse sub = api.Request(HttpMethod::kGet,
                                  DevicePath(tuya_cloud::kPathSpecifications, device.id), "", "");
    if (sub.ok && ResultToText(sub.result, text)) {
        hub_core::ParseSpecificationsJson(text, device);
    } else if (!sub.ok) {
        ESP_LOGW(TAG, "bindUser %s: specifications failed (%s)", device.id.c_str(),
                 sub.msg.c_str());
    }

    const bool had_local_strategy = device.support_local;
    sub = api.Request(HttpMethod::kGet, DevicePath(tuya_cloud::kPathStatusStrategy, device.id), "",
                      "");
    if (sub.ok && ResultToText(sub.result, text)) {
        hub_core::ParseStatusStrategyJson(text, device);
        // An empty DTO list reads as "no local support" — never erase a
        // known strategy (same rule as the discovery refresh).
        if (had_local_strategy && !device.support_local) {
            device.support_local = true;
        }
    } else if (!sub.ok) {
        ESP_LOGW(TAG, "bindUser %s: status strategy failed (%s) — support_local=%d",
                 device.id.c_str(), sub.msg.c_str(), device.support_local ? 1 : 0);
    }

    sub = api.Request(HttpMethod::kGet, DevicePath(tuya_cloud::kPathDpReportTypes, device.id), "",
                      "");
    if (sub.ok && ResultToText(sub.result, text)) {
        hub_core::ParseDpReportTypesJson(text, device);
    } else if (!sub.ok) {
        ESP_LOGW(TAG, "bindUser %s: dp report types failed (%s)", device.id.c_str(),
                 sub.msg.c_str());
    }

    // Freshly bound device: tracked from the start (no entities to wait for).
    device.set_up = true;
    registry.UpsertDevice(device);  // fires kDeviceAdded / kDeviceUpdated
    ESP_LOGI(TAG, "bindUser %s: upserted (support_local=%d status=%u)", device.id.c_str(),
             device.support_local ? 1 : 0, static_cast<unsigned>(device.status.size()));

    // Subscribe its topic directly — safe from this task.
    MqttConfig cfg;
    {
        std::lock_guard<std::mutex> lock(config_mutex_);
        cfg = config_;
    }
    if (client_ != nullptr && IsConnected() && !cfg.dev_topic_template.empty()) {
        // Both suffixes: a late-bound device gets the same coverage.
        for (const bool support_local : {true, false}) {
            const std::string topic = DeviceTopic(cfg.dev_topic_template, device.id, support_local);
            const int msg_id = esp_mqtt_client_subscribe_single(client_, topic.c_str(), 0);
            ESP_LOGI(TAG, "bindUser %s: subscribed %s (msg_id=%d)", device.id.c_str(),
                     topic.c_str(), msg_id);
        }
    }
}

}  // namespace tuya_mqtt
