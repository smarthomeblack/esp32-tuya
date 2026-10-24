#pragma once

#include <esp_event.h>
#include <esp_netif.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/event_groups.h>

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

namespace sys {

/// One Wi-Fi scan result (strongest first, deduplicated).
struct WifiApRecord {
    std::string ssid;  ///< Network name (empty for hidden networks).
    int8_t rssi = 0;
    bool open = false;   ///< true when the network needs no password.
    bool valid = false;  ///< false only for the scan-failure sentinel record.
};

/// Station driver: single SSID, credentials in the "wifi" NVS namespace.
/// Retries forever.
class WifiStation {
public:
    static WifiStation& GetInstance() {
        static WifiStation instance;
        return instance;
    }
    WifiStation(const WifiStation&) = delete;
    WifiStation& operator=(const WifiStation&) = delete;

    /// Brings the station up from stored credentials. Returns false when no
    /// credentials are stored or driver initialization fails.
    bool Start();

    /// Tears the driver down; Start() can be called again afterwards.
    void Stop();

    /// Waits for an IP address. Returns true when connected.
    bool WaitForConnected(uint32_t timeout_ms);

    /// True while the station holds an IP address.
    bool IsConnected() const;

    /// Current IP address ("" while not connected).
    std::string GetIp() const;

    /// Configured SSID ("" when never started).
    std::string GetSsid() const;

    /// RSSI cached at association time (0 when never connected).
    int8_t GetRssi() const;

    /// Blocking scan (STA/APSTA mode); sorted by RSSI, deduplicated by
    /// SSID; failure yields one record with valid == false.
    std::vector<WifiApRecord> Scan(uint32_t max_ms = 4000);

private:
    WifiStation();
    // Singleton destroyed only at process exit; FreeRTOS handles are never released.

    static void WifiEventHandler(void* arg, esp_event_base_t event_base, int32_t event_id,
                                 void* event_data);
    static void IpEventHandler(void* arg, esp_event_base_t event_base, int32_t event_id,
                               void* event_data);
    static void OnRetryTimer(void* arg);

    EventGroupHandle_t event_group_ = nullptr;
    esp_netif_t* sta_netif_ = nullptr;
    esp_timer_handle_t retry_timer_ = nullptr;
    esp_event_handler_instance_t wifi_event_instance_ = nullptr;
    esp_event_handler_instance_t ip_event_instance_ = nullptr;
    std::atomic<bool> started_{false};
    std::atomic<int> retry_count_{0};
    std::atomic<int8_t> rssi_{0};
    mutable std::mutex state_mutex_;  // guards ssid_ / ip_
    std::string ssid_;
    std::string ip_;
};

}  // namespace sys
