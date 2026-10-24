#include "sys/system_info.h"

#include <esp_app_desc.h>
#include <esp_heap_caps.h>
#include <esp_idf_version.h>
#include <esp_log.h>
#include <esp_mac.h>
#include <esp_system.h>
#include <esp_timer.h>

static const char* TAG = "SystemInfo";

namespace sys {

std::string SystemInfo::GetMacAddress() {
    uint8_t mac[6] = {0};
    esp_err_t err = esp_read_mac(mac, ESP_MAC_EFUSE_FACTORY);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "esp_read_mac failed: %s", esp_err_to_name(err));
    }
    char buf[18];
    snprintf(buf, sizeof(buf), "%02X:%02X:%02X:%02X:%02X:%02X", mac[0], mac[1], mac[2], mac[3],
             mac[4], mac[5]);
    return std::string(buf);
}

std::string SystemInfo::GetIdfVersion() {
    return esp_get_idf_version();
}

std::string SystemInfo::GetAppVersion() {
    return esp_app_get_description()->version;
}

std::string SystemInfo::GetResetReason() {
    switch (esp_reset_reason()) {
        case ESP_RST_POWERON: return "POWERON";
        case ESP_RST_SW: return "SW";
        case ESP_RST_PANIC: return "PANIC";
        case ESP_RST_INT_WDT: return "INT_WDT";
        case ESP_RST_TASK_WDT: return "TASK_WDT";
        case ESP_RST_WDT: return "WDT";
        case ESP_RST_BROWNOUT: return "BROWNOUT";
        case ESP_RST_DEEPSLEEP: return "DEEPSLEEP";
        case ESP_RST_SDIO: return "SDIO";
        case ESP_RST_PWR_GLITCH: return "PWR_GLITCH";
        default: return "UNKNOWN";
    }
}

uint32_t SystemInfo::GetFreeHeapInternal() {
    return static_cast<uint32_t>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
}

uint32_t SystemInfo::GetFreeHeapPsram() {
    return static_cast<uint32_t>(heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
}

uint32_t SystemInfo::GetMinFreeHeapInternal() {
    return static_cast<uint32_t>(heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL));
}

// heap_caps_get_total_size / heap_caps_get_largest_free_block.
uint32_t SystemInfo::GetTotalHeapInternal() {
    return static_cast<uint32_t>(heap_caps_get_total_size(MALLOC_CAP_INTERNAL));
}

uint32_t SystemInfo::GetLargestFreeHeapInternal() {
    return static_cast<uint32_t>(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
}

uint32_t SystemInfo::GetTotalHeapPsram() {
    return static_cast<uint32_t>(heap_caps_get_total_size(MALLOC_CAP_SPIRAM));
}

uint32_t SystemInfo::GetLargestFreeHeapPsram() {
    return static_cast<uint32_t>(heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM));
}

uint64_t SystemInfo::GetUptimeMs() {
    return static_cast<uint64_t>(esp_timer_get_time()) / 1000;
}

}  // namespace sys
