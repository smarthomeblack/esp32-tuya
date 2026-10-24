#include "tuya_cloud/time_sync.h"

#include <esp_log.h>
#include <esp_netif_sntp.h>

#include <sys/time.h>

#include "sdkconfig.h"

static const char* TAG = "TimeSync";

namespace tuya_cloud {

int64_t TimeSync::NowMs() {
    struct timeval tv = {};
    gettimeofday(&tv, nullptr);
    return static_cast<int64_t>(tv.tv_sec) * 1000 + tv.tv_usec / 1000;
}

bool TimeSync::SyncBlocking(uint32_t timeout_ms) {
    if (timeout_ms == 0) {
        // A zero timeout would fall through the semaphore take immediately.
        ESP_LOGW(TAG, "SyncBlocking with timeout 0 ms");
        return false;
    }

    // DEFAULT_CONFIG: wait_for_sync, start=true (lwip polls immediately, then hourly).
    esp_sntp_config_t config = ESP_NETIF_SNTP_DEFAULT_CONFIG(CONFIG_TUYA_NTP_SERVER);

    esp_err_t err = esp_netif_sntp_init(&config);
    if (err == ESP_ERR_INVALID_STATE) {
        // Already initialized: restart so lwip polls immediately again.
        err = esp_netif_sntp_start();
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "esp_netif_sntp_start failed: 0x%x", err);
            return false;
        }
    } else if (err != ESP_OK) {
        // esp_netif (tcpip) not up yet, or allocation failure.
        ESP_LOGE(TAG, "esp_netif_sntp_init failed: 0x%x (is the network up?)", err);
        return false;
    }

    err = esp_netif_sntp_sync_wait(pdMS_TO_TICKS(timeout_ms));
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "SNTP sync did not complete within %u ms (err=0x%x) — server %s",
                 static_cast<unsigned>(timeout_ms), err, CONFIG_TUYA_NTP_SERVER);
        return false;
    }

    ESP_LOGI(TAG, "Time synced via %s: now=%lld ms", CONFIG_TUYA_NTP_SERVER,
             static_cast<long long>(NowMs()));
    return true;
}

}  // namespace tuya_cloud
