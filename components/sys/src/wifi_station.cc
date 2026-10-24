#include "sys/wifi_station.h"

#include <esp_log.h>
#include <esp_wifi.h>
#include <esp_wifi_default.h>
#include <string.h>

#include <algorithm>
#include <memory>
#include <set>
#include <utility>

#include "sys/settings.h"

static const char* TAG = "WifiStation";

namespace sys {

namespace {

constexpr EventBits_t kConnectedBit = BIT0;
constexpr int kMaxReconnect = 5;
constexpr uint64_t kRetryPeriodUs = 10ULL * 1000000ULL;  // never give up
// wifi_sta_config_t.ssid is NUL-terminated → effective max is 31; WPA passphrase max is 63.
constexpr uint32_t kMaxSsidLen = 31;
constexpr uint32_t kMaxPasswordLen = 63;
constexpr uint16_t kMaxScanResults = 30;
constexpr uint32_t kScanChannels24g = 13;       // 2.4GHz channel count
constexpr uint32_t kScanMaxPerChannelMs = 120;  // driver default active dwell
constexpr uint32_t kScanMinPerChannelMs = 30;
// Reserve for per-channel switching overhead inside the overall budget.
constexpr uint32_t kScanOverheadMs = 800;

std::vector<WifiApRecord> ScanFailure(const char* api, esp_err_t err) {
    ESP_LOGE(TAG, "%s: %s", api, esp_err_to_name(err));
    return {WifiApRecord{}};  // sentinel: valid == false
}

}  // namespace

WifiStation::WifiStation() {
    event_group_ = xEventGroupCreate();
    if (event_group_ == nullptr) {
        // Cannot happen under normal operation (OOM at boot) — unrecoverable.
        ESP_LOGE(TAG, "Failed to create event group");
        abort();
    }
}

bool WifiStation::Start() {
    bool expected = false;
    if (!started_.compare_exchange_strong(expected, true)) {
        ESP_LOGW(TAG, "Start() called while already started — ignoring");
        return true;
    }

    // Credentials from NVS; an empty SSID means provisioning mode.
    std::string ssid;
    std::string password;
    {
        Settings settings("wifi");
        ssid = settings.GetString("ssid");
        password = settings.GetString("password");
    }
    if (ssid.empty()) {
        ESP_LOGI(TAG, "No Wi-Fi credentials stored — not starting the station");
        started_.store(false);
        return false;
    }
    if (ssid.length() > kMaxSsidLen || password.length() > kMaxPasswordLen) {
        // Corrupt/out-of-range store — treat as missing credentials.
        ESP_LOGE(TAG, "Stored credentials out of range (ssid %u, password %u bytes)",
                 static_cast<unsigned>(ssid.length()), static_cast<unsigned>(password.length()));
        started_.store(false);
        return false;
    }

    // esp_netif_init() is safe when already initialized (after a Start/Stop cycle).
    esp_err_t err = esp_netif_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_netif_init: %s", esp_err_to_name(err));
        started_.store(false);
        return false;
    }

    sta_netif_ = esp_netif_create_default_wifi_sta();  // aborts internally on failure

    wifi_init_config_t init_cfg = WIFI_INIT_CONFIG_DEFAULT();
    err = esp_wifi_init(&init_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_init: %s", esp_err_to_name(err));
        Stop();
        return false;
    }

    err = esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, &WifiStation::WifiEventHandler, this, &wifi_event_instance_);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "register WIFI_EVENT handler: %s", esp_err_to_name(err));
        Stop();
        return false;
    }
    err = esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, &WifiStation::IpEventHandler, this, &ip_event_instance_);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "register IP_EVENT handler: %s", esp_err_to_name(err));
        Stop();
        return false;
    }

    // 10s retry timer, armed by the disconnect handler after the quick retries.
    const esp_timer_create_args_t timer_args = {
        .callback = &WifiStation::OnRetryTimer,
        .arg = this,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "wifi_retry",
        .skip_unhandled_events = true,
    };
    err = esp_timer_create(&timer_args, &retry_timer_);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_timer_create: %s", esp_err_to_name(err));
        Stop();
        return false;
    }

    wifi_config_t wifi_config = {};
    strlcpy(reinterpret_cast<char*>(wifi_config.sta.ssid), ssid.c_str(),
            sizeof(wifi_config.sta.ssid));
    strlcpy(reinterpret_cast<char*>(wifi_config.sta.password), password.c_str(),
            sizeof(wifi_config.sta.password));
    wifi_config.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;

    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        ssid_ = ssid;
        ip_.clear();
    }

    err = esp_wifi_set_mode(WIFI_MODE_STA);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_set_mode: %s", esp_err_to_name(err));
        Stop();
        return false;
    }
    err = esp_wifi_set_config(WIFI_IF_STA, &wifi_config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_set_config: %s", esp_err_to_name(err));
        Stop();
        return false;
    }
    err = esp_wifi_start();  // WIFI_EVENT_STA_START triggers the first connect
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_start: %s", esp_err_to_name(err));
        Stop();
        return false;
    }

    retry_count_.store(0);
    ESP_LOGI(TAG, "Station started, connecting to '%s'", ssid.c_str());
    return true;
}

void WifiStation::Stop() {
    if (retry_timer_ != nullptr) {
        esp_timer_stop(retry_timer_);
    }

    // Unregister handlers before deleting the timer: esp_event unregister
    // waits for in-flight dispatch, so no handler can touch it afterwards.
    if (wifi_event_instance_ != nullptr) {
        esp_event_handler_instance_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event_instance_);
        wifi_event_instance_ = nullptr;
    }
    if (ip_event_instance_ != nullptr) {
        esp_event_handler_instance_unregister(IP_EVENT, IP_EVENT_STA_GOT_IP, ip_event_instance_);
        ip_event_instance_ = nullptr;
    }

    if (retry_timer_ != nullptr) {
        esp_timer_delete(retry_timer_);
        retry_timer_ = nullptr;
    }

    esp_err_t err = esp_wifi_scan_stop();
    if (err != ESP_OK) {
        ESP_LOGD(TAG, "esp_wifi_scan_stop: %s", esp_err_to_name(err));
    }
    err = esp_wifi_stop();
    if (err != ESP_OK) {
        ESP_LOGD(TAG, "esp_wifi_stop: %s", esp_err_to_name(err));
    }
    err = esp_wifi_deinit();
    if (err != ESP_OK) {
        ESP_LOGD(TAG, "esp_wifi_deinit: %s", esp_err_to_name(err));
    }

    if (sta_netif_ != nullptr) {
        // Clears the default handlers' static netif pointers (a bare
        // esp_netif_destroy would leave them dangling).
        esp_wifi_clear_default_wifi_driver_and_handlers(sta_netif_);
        esp_netif_destroy(sta_netif_);
        sta_netif_ = nullptr;
    }

    // Prevent a premature WaitForConnected() success after a restart.
    xEventGroupClearBits(event_group_, kConnectedBit);
    rssi_.store(0);
    retry_count_.store(0);
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        ip_.clear();
    }
    started_.store(false);
}

bool WifiStation::WaitForConnected(uint32_t timeout_ms) {
    EventBits_t bits = xEventGroupWaitBits(event_group_, kConnectedBit, pdFALSE, pdFALSE,
                                           pdMS_TO_TICKS(timeout_ms));
    return (bits & kConnectedBit) != 0;
}

bool WifiStation::IsConnected() const {
    return (xEventGroupGetBits(event_group_) & kConnectedBit) != 0;
}

std::string WifiStation::GetIp() const {
    std::lock_guard<std::mutex> lock(state_mutex_);
    return ip_;
}

std::string WifiStation::GetSsid() const {
    std::lock_guard<std::mutex> lock(state_mutex_);
    return ssid_;
}

int8_t WifiStation::GetRssi() const {
    return rssi_.load();
}

std::vector<WifiApRecord> WifiStation::Scan(uint32_t max_ms) {
    // Map the overall budget to a per-channel dwell (13 channels on 2.4GHz).
    uint32_t per_channel_ms = kScanMaxPerChannelMs;
    if (max_ms > kScanOverheadMs) {
        per_channel_ms = (max_ms - kScanOverheadMs) / kScanChannels24g;
        if (per_channel_ms > kScanMaxPerChannelMs) {
            per_channel_ms = kScanMaxPerChannelMs;
        }
        if (per_channel_ms < kScanMinPerChannelMs) {
            per_channel_ms = kScanMinPerChannelMs;
        }
    } else {
        per_channel_ms = kScanMinPerChannelMs;
    }

    wifi_scan_config_t scan_config = {};
    scan_config.scan_time.active.max = per_channel_ms;

    esp_err_t err = esp_wifi_scan_start(&scan_config, true);  // blocking
    if (err != ESP_OK) {
        return ScanFailure("esp_wifi_scan_start", err);
    }

    uint16_t ap_num = 0;
    err = esp_wifi_scan_get_ap_num(&ap_num);
    if (err != ESP_OK) {
        return ScanFailure("esp_wifi_scan_get_ap_num", err);
    }
    if (ap_num == 0) {
        return {};  // nothing in range — a successful, empty scan
    }
    if (ap_num > kMaxScanResults) {
        ap_num = kMaxScanResults;  // bound memory and JSON size
    }

    std::unique_ptr<wifi_ap_record_t[]> records(new wifi_ap_record_t[ap_num]);
    uint16_t got = ap_num;
    err = esp_wifi_scan_get_ap_records(&got, records.get());
    if (err != ESP_OK) {
        return ScanFailure("esp_wifi_scan_get_ap_records", err);
    }

    // Strongest first; keep only the first record per SSID.
    std::sort(records.get(), records.get() + got,
              [](const wifi_ap_record_t& a, const wifi_ap_record_t& b) { return a.rssi > b.rssi; });
    std::vector<WifiApRecord> result;
    std::set<std::string> seen;
    for (uint16_t i = 0; i < got; ++i) {
        const wifi_ap_record_t& ap = records[i];
        std::string ssid(reinterpret_cast<const char*>(ap.ssid),
                         strnlen(reinterpret_cast<const char*>(ap.ssid), sizeof(ap.ssid)));
        if (ssid.empty() || !seen.insert(ssid).second) {
            continue;  // hidden network, or a weaker duplicate SSID
        }
        WifiApRecord record;
        record.ssid = std::move(ssid);
        record.rssi = ap.rssi;
        record.open = ap.authmode == WIFI_AUTH_OPEN;
        record.valid = true;
        result.push_back(std::move(record));
    }
    return result;
}

void WifiStation::OnRetryTimer(void* arg) {
    (void)arg;
    // ESP_ERR_WIFI_STATE means an attempt is already in progress.
    esp_err_t err = esp_wifi_connect();
    if (err != ESP_OK && err != ESP_ERR_WIFI_STATE) {
        ESP_LOGW(TAG, "periodic retry esp_wifi_connect: %s", esp_err_to_name(err));
    }
}

void WifiStation::WifiEventHandler(void* arg, esp_event_base_t event_base, int32_t event_id,
                                   void* event_data) {
    (void)event_base;  // registered for WIFI_EVENT only
    auto* self = static_cast<WifiStation*>(arg);
    switch (event_id) {
        case WIFI_EVENT_STA_START:
            ESP_LOGI(TAG, "Wi-Fi driver up, connecting to '%s'", self->GetSsid().c_str());
            if (esp_wifi_connect() != ESP_OK) {
                ESP_LOGE(TAG, "esp_wifi_connect failed to start");
            }
            break;
        case WIFI_EVENT_STA_DISCONNECTED: {
            auto* event = static_cast<wifi_event_sta_disconnected_t*>(event_data);
            xEventGroupClearBits(self->event_group_, kConnectedBit);
            const int attempt = self->retry_count_.fetch_add(1) + 1;
            if (attempt <= kMaxReconnect) {
                ESP_LOGI(TAG, "Disconnected (reason=%d) — reconnect attempt %d/%d",
                         static_cast<int>(event->reason), attempt, kMaxReconnect);
                if (esp_wifi_connect() != ESP_OK) {
                    ESP_LOGE(TAG, "esp_wifi_connect failed to retry");
                }
                break;
            }
            // Quick retries exhausted — retry every 10s, forever.
            // ESP_ERR_INVALID_STATE = the timer is already armed, which is fine.
            esp_err_t err = esp_timer_start_periodic(self->retry_timer_, kRetryPeriodUs);
            if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
                ESP_LOGE(TAG, "esp_timer_start_periodic: %s", esp_err_to_name(err));
            } else {
                ESP_LOGW(TAG, "Disconnected (reason=%d) after %d attempts — retrying every 10s",
                         static_cast<int>(event->reason), kMaxReconnect);
            }
            break;
        }
        default: break;
    }
}

void WifiStation::IpEventHandler(void* arg, esp_event_base_t event_base, int32_t event_id,
                                 void* event_data) {
    (void)event_base;
    (void)event_id;  // registered for IP_EVENT_STA_GOT_IP only
    auto* self = static_cast<WifiStation*>(arg);
    auto* event = static_cast<ip_event_got_ip_t*>(event_data);

    char ip_buf[16];  // "255.255.255.255" + NUL
    esp_ip4addr_ntoa(&event->ip_info.ip, ip_buf, sizeof(ip_buf));

    int8_t rssi = 0;
    wifi_ap_record_t ap_info;
    if (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK) {
        rssi = ap_info.rssi;
    }

    {
        std::lock_guard<std::mutex> lock(self->state_mutex_);
        self->ip_ = ip_buf;
    }
    self->rssi_.store(rssi);
    self->retry_count_.store(0);
    if (self->retry_timer_ != nullptr) {
        esp_timer_stop(self->retry_timer_);
    }
    ESP_LOGI(TAG, "Got IP %s (rssi %d)", ip_buf, static_cast<int>(rssi));
    xEventGroupSetBits(self->event_group_, kConnectedBit);
}

}  // namespace sys
