#include "application.h"

#include <esp_event.h>
#include <esp_log.h>

#include "nvs_secure_boot.h"
#include "tuya_crypto/at_rest_crypto.h"

static const char* TAG = "Main";

extern "C" void app_main(void) {
    // Default event loop first (WiFi / netif / httpd all post events to it).
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    // Encrypted NVS: burns the eFuse key + migrates plaintext on first boot.
    // Never aborts: on failure the hub degrades to defaults + setup mode.
    if (boot::InitSecureNvs() != ESP_OK) {
        ESP_LOGE(TAG, "NVS init failed — storage degraded, defaults in use");
    }

    // Registry-snapshot AES key (separate eFuse block); failure only disables
    // snapshot encryption — loads of legacy files keep working.
    if (!tuya_crypto::InitAtRestKey()) {
        ESP_LOGE(TAG, "Registry at-rest key unavailable — snapshot encryption disabled");
    }

    Application::GetInstance().Start();
}
