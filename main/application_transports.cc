#include "application.h"

#include <esp_log.h>

#include "hub_core/device_registry.h"
#include "sys/system_info.h"
#include "tuya_mqtt/tuya_mqtt_client.h"

// Log tag mirrors Application so the boot log stays byte-identical.
static const char* TAG = "Application";

bool Application::StartMqttPush() {
    // Only a linked hub pushes (the config fetch needs credentials).
    if (GetState() != kDeviceStateLinked) {
        ESP_LOGD(TAG, "StartMqttPush skipped — not linked (state=%d)",
                 static_cast<int>(GetState()));
        return false;
    }
    // The transition attempt is latched below (success OR refusal), so the
    // kDeviceAdded resubscribe stays suppressed until the tick heals it.
    mqtt_push_started_ = true;
    const bool started = StartMqttPushTransport();
    // One blocking scene-list GET per home; the task is spawned off the loop.
    // Skipped with no homes (the first homes/device event retries): an empty
    // list must never replace a stale-but-better cache.
    if (!hub_core::DeviceRegistry::GetInstance().GetHomes().empty()) {
        ScheduleSceneFetch();
    }
    return started;
}

bool Application::StartMqttPushTransport() {
    // Arm the clock-tick retry on EVERY transport failure below.
    const size_t tracked = hub_core::DeviceRegistry::GetInstance().MarkAllSetUp();
    ESP_LOGI(TAG, "MarkAllSetUp: %u device(s) tracked for the push transport",
             static_cast<unsigned>(tracked));
    const bool started = tuya_mqtt::TuyaMqttClient::GetInstance().Start();
    if (started) {
        mqtt_push_next_retry_ms_ = 0;
        return true;
    }
    // Refused: a just-stopped mgmt task can lag minutes in FetchConfig; a
    // pending resubscribe does not run Start itself, so both cases retry here.
    mqtt_push_retries_ = 0;
    mqtt_push_next_retry_ms_ =
        static_cast<int64_t>(sys::SystemInfo::GetUptimeMs()) + kMqttPushRetryFirstMs;
    ESP_LOGW(TAG, "MQTT start refused — self-heal retry in %lld s",
             static_cast<long long>(kMqttPushRetryFirstMs / 1000));
    return false;
}
