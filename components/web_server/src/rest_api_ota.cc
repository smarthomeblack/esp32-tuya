// Asset partition (WAS1) + OTA endpoints.

#include "rest_api_internal.h"

#include <esp_app_desc.h>
#include <esp_app_format.h>
#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_ota_ops.h>
#include <esp_partition.h>

#include <algorithm>
#include <cstdio>
#include <cstring>

#include "web_assets/assets.h"
#include "web_server/api_auth.h"

static const char* TAG = "RestApi";

namespace web_server {
namespace rest_api {

namespace {

/// One OTA app slot (ota_0/ota_1 are 0x3F0000 each in tuya_16m.csv).
constexpr size_t kOtaSlotSize = 0x3F0000;
/// The esp_image_header_t prefix validated before esp_ota_begin.
constexpr size_t kOtaHeaderSize = 24;
/// OTA upload scratch; internal RAM (flash-cache-safe esp_ota_write source).
constexpr size_t kOtaChunkSize = 16 * 1024;

/// The /api/asset/info "asset" object (also the upload result).
cJSON* BuildAssetInfoObject() {
    WebAssetInfo info = {};
    WebAssetsGetInfo(&info);
    cJSON* asset = cJSON_CreateObject();
    if (asset == nullptr) {
        return nullptr;
    }
    cJSON_AddBoolToObject(asset, "valid", info.valid);
    cJSON_AddNumberToObject(asset, "version", static_cast<double>(info.version));
    cJSON_AddStringToObject(asset, "build_id", info.build_id);
    cJSON_AddNumberToObject(asset, "files", static_cast<double>(info.files));
    cJSON_AddNumberToObject(asset, "payload_len", static_cast<double>(info.payload_len));
    cJSON_AddNumberToObject(asset, "partition_size", static_cast<double>(info.partition_size));
    return asset;
}

}  // namespace

// GET /api/asset/info — WAS1 store snapshot.
esp_err_t HandleAssetInfoApi(httpd_req_t* req) {
    if (RequireAuth(req) != ESP_OK)
        return ESP_FAIL;  // 401 already sent
    cJSON* root = cJSON_CreateObject();
    if (root == nullptr) {
        return RespondJsonError(req, "500 Internal Server Error", "internal", "Out of memory");
    }
    cJSON_AddBoolToObject(root, "success", true);
    AttachOwned(root, "asset", BuildAssetInfoObject());
    return SendJson(req, root);
}

// POST /api/asset/upload — whole .bin into PSRAM, validated in RAM, then
// flashed payload-first; no reboot (the store re-mounts immediately).
esp_err_t HandleAssetUploadApi(httpd_req_t* req) {
    if (RequireAuth(req) != ESP_OK)
        return ESP_FAIL;  // 401 already sent
    WebAssetInfo probe = {};
    WebAssetsGetInfo(&probe);
    uint8_t* buf = ReadBinaryBody(req, probe.partition_size != 0 ? probe.partition_size : 0x100000);
    if (buf == nullptr) {
        return ESP_FAIL;  // response already sent
    }
    WebAssetInfo uploaded = {};
    if (!AssetImageValidateBuf(buf, req->content_len, probe.partition_size, &uploaded)) {
        heap_caps_free(buf);
        return RespondJsonError(req, "400 Bad Request", "invalid_asset",
                                "Not a valid WAS1 asset image");
    }
    const esp_err_t flash_err = AssetImageFlashBuf(buf, req->content_len);
    heap_caps_free(buf);
    if (flash_err != ESP_OK) {
        return RespondJsonError(req, "500 Internal Server Error", "internal",
                                "Asset flash write failed");
    }
    cJSON* root = cJSON_CreateObject();
    if (root == nullptr) {
        return RespondJsonError(req, "500 Internal Server Error", "internal", "Out of memory");
    }
    cJSON_AddBoolToObject(root, "success", true);
    AttachOwned(root, "asset", BuildAssetInfoObject());
    return SendJson(req, root);
}

// GET /api/ota/info — running/next slot + app description.
esp_err_t HandleOtaInfoApi(httpd_req_t* req) {
    if (RequireAuth(req) != ESP_OK)
        return ESP_FAIL;  // 401 already sent
    const esp_partition_t* running = esp_ota_get_running_partition();
    const esp_partition_t* next = esp_ota_get_next_update_partition(nullptr);
    const esp_app_desc_t* desc = esp_app_get_description();
    cJSON* root = cJSON_CreateObject();
    if (root == nullptr) {
        return RespondJsonError(req, "500 Internal Server Error", "internal", "Out of memory");
    }
    cJSON_AddBoolToObject(root, "success", true);
    cJSON* ota = cJSON_AddObjectToObject(root, "ota");
    if (ota != nullptr) {
        cJSON_AddStringToObject(ota, "running", running != nullptr ? running->label : "");
        cJSON_AddStringToObject(ota, "next", next != nullptr ? next->label : "");
        cJSON_AddNumberToObject(ota, "slot_size",
                                static_cast<double>(next != nullptr ? next->size : 0));
        cJSON_AddStringToObject(ota, "version", desc != nullptr ? desc->version : "");
        // esp_app_desc_t: "time" is the clock, "date" the calendar date.
        char build_time[40] = {0};
        if (desc != nullptr) {
            snprintf(build_time, sizeof(build_time), "%.16s %.16s", desc->date, desc->time);
        }
        cJSON_AddStringToObject(ota, "build_time", build_time);
    }
    return SendJson(req, root);
}

// POST /api/ota/upload — whole .bin buffered in PSRAM; the 24-byte header
// is validated before esp_ota_begin. No reboot.
esp_err_t HandleOtaUploadApi(httpd_req_t* req) {
    if (RequireAuth(req) != ESP_OK)
        return ESP_FAIL;  // 401 already sent
    if (req->content_len > kOtaSlotSize) {
        return RespondJsonError(req, "400 Bad Request", "too_large",
                                "Image exceeds the OTA slot size");
    }

    const esp_partition_t* next = esp_ota_get_next_update_partition(nullptr);
    if (next == nullptr) {
        return RespondJsonError(req, "500 Internal Server Error", "ota_unavailable",
                                "No OTA slot available");
    }

    // Whole body into PSRAM (like the asset upload path).
    uint8_t* buf = ReadBinaryBody(req, kOtaSlotSize);
    if (buf == nullptr) {
        return ESP_FAIL;  // response already sent
    }
    if (req->content_len < kOtaHeaderSize) {
        heap_caps_free(buf);
        return RespondJsonError(req, "400 Bad Request", "invalid_param",
                                "Image shorter than the 24-byte header");
    }
    const esp_image_header_t* image_header = reinterpret_cast<const esp_image_header_t*>(buf);
    if (image_header->magic != ESP_IMAGE_HEADER_MAGIC) {
        heap_caps_free(buf);
        return RespondJsonError(req, "400 Bad Request", "invalid_param",
                                "Bad image magic (not an ESP32 app image)");
    }
    if (image_header->chip_id != ESP_CHIP_ID_ESP32S3) {
        heap_caps_free(buf);
        return RespondJsonError(req, "400 Bad Request", "invalid_param",
                                "Image is not for the ESP32-S3");
    }

    // esp_ota_write source in internal RAM; a PSRAM-mapped source would
    // also be safe — IDF v6 esp_flash_write stages non-DRAM sources.
    uint8_t* chunk = static_cast<uint8_t*>(heap_caps_malloc(kOtaChunkSize, MALLOC_CAP_INTERNAL));
    if (chunk == nullptr) {
        heap_caps_free(buf);
        return RespondJsonError(req, "500 Internal Server Error", "internal", "Out of memory");
    }

    esp_ota_handle_t handle = 0;
    esp_err_t err = esp_ota_begin(next, req->content_len, &handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_begin: %s", esp_err_to_name(err));
        heap_caps_free(chunk);
        heap_caps_free(buf);
        return RespondJsonError(req, "500 Internal Server Error", "ota_unavailable",
                                "OTA begin failed");
    }
    // Chunked copy through internal RAM; `buf` is the whole image.
    size_t written = 0;
    while (err == ESP_OK && written < req->content_len) {
        const size_t want = std::min(kOtaChunkSize, req->content_len - written);
        memcpy(chunk, buf + written, want);
        err = esp_ota_write(handle, chunk, want);
        written += want;
    }
    heap_caps_free(chunk);
    heap_caps_free(buf);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_write: %s", esp_err_to_name(err));
        esp_ota_abort(handle);
        return RespondJsonError(req, "500 Internal Server Error", "ota_unavailable",
                                "OTA write failed");
    }
    // esp_ota_end verifies the image (checksum + SHA if present).
    err = esp_ota_end(handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_end: %s", esp_err_to_name(err));
        return RespondJsonError(req, "400 Bad Request", "invalid_asset",
                                "OTA image verification failed");
    }
    err = esp_ota_set_boot_partition(next);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_set_boot_partition: %s", esp_err_to_name(err));
        return RespondJsonError(req, "500 Internal Server Error", "ota_unavailable",
                                "Setting the boot partition failed");
    }
    ESP_LOGW(TAG, "OTA image accepted: %u B -> %s (reboot pending)",
             static_cast<unsigned>(req->content_len), next->label);
    return RespondJson(req, "{\"success\":true}", "200 OK");
}

// POST /api/ota/rollback — mark the RUNNING image INVALID in otadata (the
// bootloader then picks the previous valid slot), respond, then reboot.
esp_err_t HandleOtaRollbackApi(httpd_req_t* req) {
    if (RequireAuth(req) != ESP_OK)
        return ESP_FAIL;  // 401 already sent
    const esp_err_t err = esp_ota_mark_app_invalid_rollback();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "OTA rollback: %s", esp_err_to_name(err));
        return RespondJsonError(req, "500 Internal Server Error", "ota_unavailable",
                                "Rollback is not possible for the running slot");
    }
    ESP_LOGW(TAG, "OTA rollback armed — rebooting into the previous valid slot");
    esp_err_t send_err = RespondJson(req, "{\"success\":true}", "200 OK");
    ScheduleReboot();
    return send_err;
}

}  // namespace rest_api
}  // namespace web_server
