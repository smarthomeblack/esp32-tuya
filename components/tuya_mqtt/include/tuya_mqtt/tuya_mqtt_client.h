#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>

// Opaque esp-mqtt handle, forward-declared at global scope so this public
// header stays free of <mqtt_client.h> (a PRIVATE dependency).
struct esp_mqtt_client;

namespace tuya_mqtt {

/// Connection status snapshot, published on every CONNECTED / DISCONNECTED
/// and on connect-phase failures.
struct MqttStatus {
    bool connected = false;
    /// esp-mqtt connection-refused code (5 == NOT_AUTHORIZED); 0 when the
    /// status carries no protocol error.
    int error_code = 0;
    /// Human-readable detail ("" when clean).
    std::string error;
};

/// Parsed POST /v1.0/m/life/ha/access/config result; the cJSON-based
/// parse function stays internal (src/mqtt_message.h).
struct MqttConfig {
    /// result.url verbatim, e.g. "ssl://host:8883".
    std::string url_raw;
    /// esp-mqtt form of the URL: "ssl://" -> "mqtts://"; "mqtt://" as-is.
    std::string broker_uri;
    std::string host;
    uint16_t port = 0;
    std::string client_id;
    std::string username;
    std::string password;
    /// result.expireTime in SECONDS (~7200); the cycle refreshes 60 s before.
    int64_t expire_time_s = 0;
    /// result.topic.ownerId.sub — "{ownerId}" template (server-provided).
    std::string owner_topic_template;
    /// result.topic.devId.sub — "{devId}" template + "/pen"|"/sta" per device.
    std::string dev_topic_template;
};

/// Tuya sharing MQTT push channel: one management task owns the
/// config-fetch -> connect -> subscribe -> expire lifecycle.
class TuyaMqttClient {
public:
    static TuyaMqttClient& GetInstance() {
        static TuyaMqttClient instance;
        return instance;
    }
    TuyaMqttClient(const TuyaMqttClient&) = delete;
    TuyaMqttClient& operator=(const TuyaMqttClient&) = delete;

    /// Invoked from the esp-mqtt task on every status change. Keep it
    /// fast — marshal to your own loop. Replaces the previous listener.
    using StatusListener = std::function<void(const MqttStatus&)>;

    /// Spawns the management task ("tuya_mqtt_mgmt", 12 KB stack); false when
    /// already running or a previous task is still shutting down. Call AFTER MarkAllSetUp.
    bool Start();

    /// Requests a clean shutdown; blocks up to ~5 s. Never call from the
    /// esp-mqtt event handler.
    void Stop();

    bool IsConnected() const { return connected_.load(); }

    /// True while the management task is alive (a refused Start() self-heals
    /// once it exits).
    bool IsRunning() const { return running_.load(); }

    void SetStatusListener(StatusListener listener);

    /// Snapshot of the current status (copy under the status mutex).
    MqttStatus GetStatus() const;

private:
    TuyaMqttClient();
    ~TuyaMqttClient();

    /// Management task body.
    void MgmtTask();

    /// One connection cycle: store config, start the client, wait for
    /// CONNECTED (30 s).
    enum class CycleResult {
        kConnected,    ///< broker accepted the connection
        kAuthRefused,  ///< broker refused the credentials (refetch now)
        kFailed,       ///< connect timeout / init failure (1 s + retry)
    };
    CycleResult StartCycle(const MqttConfig& config);

    /// Tears the client down; management task ONLY (the esp-mqtt API
    /// forbids stop/destroy from the event handler).
    void DestroyClient();

    /// Fetches the access config (3 attempts, 1 s apart); false when all
    /// attempts fail.
    bool FetchConfig(MqttConfig& out);

    // ---- called (only) from the esp-mqtt event handler (mqtt task) ----

    /// Subscribes owner + tracked device topics on every
    /// MQTT_EVENT_CONNECTED.
    void SubscribeAll(struct esp_mqtt_client* client);

    /// One complete (reassembled if fragmented) payload: the protocol dispatch.
    void HandlePayload(struct esp_mqtt_client* client, const char* data, size_t len);

    /// protocol 20 event dispatch; `client` is used to UNSUBSCRIBE on delete.
    void HandleDeviceEvent(struct esp_mqtt_client* client, const std::string& biz_code,
                           const std::string& dev_id, const std::string& new_name);

    // ---- called (only) from the management task ----

    /// Deferred bindUser handling: refetch the device detail, upsert + subscribe.
    void ProcessBindUser(const std::string& dev_id);

    /// Publishes a status snapshot to the listener (copy under the lock).
    void NotifyStatus(bool connected, int error_code, std::string error);

    /// Management task trampoline.
    static void MgmtTaskTrampoline(void* arg);

    /// esp-mqtt event handler (runs on the esp-mqtt task).
    static void MqttEventHandler(void* arg, const char* event_base, int32_t event_id,
                                 void* event_data);

    /// One bindUser request for the FreeRTOS queue (fixed-size copy).
    struct BindUserItem {
        char dev_id[64];
    };

    // Opaque FreeRTOS task/queue/event-group handles; the .cc casts them
    // to the real types (keeps this header freertos-include-free).
    void* mgmt_task_ = nullptr;
    void* bind_user_queue_ = nullptr;  // BindUserItem
    void* events_ = nullptr;
    std::atomic<bool> running_{false};
    std::atomic<bool> stop_requested_{false};
    std::atomic<bool> connected_{false};

    /// Current connection's config: management task writes before the
    /// client starts, event handler reads (mutex-guarded, two tasks).
    MqttConfig config_;
    mutable std::mutex config_mutex_;

    /// Management task only; the event handler always uses event->client.
    struct esp_mqtt_client* client_ = nullptr;

    mutable std::mutex status_mutex_;
    MqttStatus status_;
    StatusListener status_listener_;  // guarded by status_mutex_
};

}  // namespace tuya_mqtt
