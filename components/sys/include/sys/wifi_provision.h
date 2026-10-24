#pragma once

#include <esp_event.h>
#include <esp_netif.h>
#include <freertos/FreeRTOS.h>
#include <freertos/event_groups.h>

#include <atomic>
#include <string>

#include "sys/dns_server.h"

namespace sys {

/// Provisioning: WPA2-PSK AP "TuyaHub-XXXX" (last 4 hex of base MAC; the
/// random persisted passphrase is printed at AP start) + captive portal on
/// 192.168.4.1. Start() never returns (reboot); call from app_main.
class WifiProvision {
public:
    static WifiProvision& GetInstance() {
        static WifiProvision instance;
        return instance;
    }
    WifiProvision(const WifiProvision&) = delete;
    WifiProvision& operator=(const WifiProvision&) = delete;

    /// Brings up the AP, DNS and HTTP servers, then blocks forever.
    void Start();

private:
    WifiProvision();
    // Singleton destroyed only at process exit; the DNS and HTTP tasks outlive it.

    void StartAccessPoint();
    void StartWebServer();  // httpd setup; implementation detail in the .cc

    static void WifiEventHandler(void* arg, esp_event_base_t event_base, int32_t event_id,
                                 void* event_data);

    DnsServer dns_server_;
    esp_netif_t* ap_netif_ = nullptr;
    esp_event_handler_instance_t wifi_event_instance_ = nullptr;
    EventGroupHandle_t event_group_ = nullptr;
    std::atomic<bool> started_{false};
    std::string ap_ssid_;  // written once in Start() before any reader
};

}  // namespace sys
