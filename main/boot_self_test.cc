#include "boot_self_test.h"

#include <esp_log.h>
#include <esp_ota_ops.h>

#include "hub_core/device_store_test.h"
#include "hub_core/model_self_test.h"
#include "hub_services/command_service_test.h"
#include "hub_services/discovery_test.h"
#include "hub_services/openapi_enrichment_test.h"
#include "sys/self_test.h"
#include "tuya_cloud/openapi_self_test.h"
#include "tuya_cloud/pipeline_self_test.h"
#include "tuya_crypto/self_test.h"
#include "tuya_mqtt/mqtt_message_test.h"
#include "web_assets/asset_self_test.h"

// Log tag mirrors Application so the boot log stays byte-identical.
static const char* TAG = "Application";

namespace boot_self_test {

bool RunAllGates() {
    if (!tuya_crypto::RunSelfTest()) {
        ESP_LOGE(TAG, "Tuya crypto self-test FAILED — skipping network startup");
        return false;
    }
    ESP_LOGI(TAG, "Crypto self-test passed");

    if (!sys::RunSelfTest()) {
        ESP_LOGE(TAG, "Sys log/cpu self-test FAILED — skipping network startup");
        return false;
    }
    ESP_LOGI(TAG, "Sys self-test passed");

    if (!tuya_cloud::RunPipelineSelfTest()) {
        ESP_LOGE(TAG, "Tuya cloud pipeline self-test FAILED — skipping network startup");
        return false;
    }
    ESP_LOGI(TAG, "Cloud pipeline self-test passed");

    if (!tuya_cloud::RunOpenApiSelfTest()) {
        ESP_LOGE(TAG, "Tuya IoT self-test FAILED — skipping network startup");
        return false;
    }
    ESP_LOGI(TAG, "Tuya IoT self-test passed");

    if (!hub_core::RunModelSelfTest()) {
        ESP_LOGE(TAG, "Hub core model self-test FAILED — skipping network startup");
        return false;
    }
    ESP_LOGI(TAG, "Hub core model self-test passed");

    if (!hub_core::RunDeviceStoreSelfTest()) {
        ESP_LOGE(TAG, "Device store self-test FAILED — skipping network startup");
        return false;
    }
    ESP_LOGI(TAG, "Device store self-test passed");

    if (!hub_core::RunOpenApiEnrichmentSelfTest()) {
        ESP_LOGE(TAG, "Tuya IoT enrichment self-test FAILED — skipping network startup");
        return false;
    }
    ESP_LOGI(TAG, "Tuya IoT enrichment self-test passed");

    if (!tuya_mqtt::RunMqttMessageSelfTest()) {
        ESP_LOGE(TAG, "Tuya MQTT message self-test FAILED — skipping network startup");
        return false;
    }
    ESP_LOGI(TAG, "MQTT message self-test passed");

    if (!hub_core::RunCommandSelfTest()) {
        ESP_LOGE(TAG, "Hub command service self-test FAILED — skipping network startup");
        return false;
    }
    ESP_LOGI(TAG, "Command service self-test passed");

    if (!hub_core::RunDiscoverySelfTest()) {
        ESP_LOGE(TAG, "Discovery service self-test FAILED — skipping network startup");
        return false;
    }
    ESP_LOGI(TAG, "Discovery service self-test passed");

    if (!web_assets::RunWebAssetSelfTest()) {
        ESP_LOGE(TAG, "Web asset self-test FAILED — skipping network startup");
        return false;
    }
    ESP_LOGI(TAG, "Web asset self-test passed");

    // All green — close the OTA rollback window (failures stay unconfirmed).
    const esp_err_t valid_err = esp_ota_mark_app_valid_cancel_rollback();
    if (valid_err == ESP_OK) {
        ESP_LOGI(TAG, "App marked valid — OTA rollback cancelled");
    } else {
        ESP_LOGW(TAG, "esp_ota_mark_app_valid_cancel_rollback: %s", esp_err_to_name(valid_err));
    }
    return true;
}

}  // namespace boot_self_test
