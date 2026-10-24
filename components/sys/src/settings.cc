#include "sys/settings.h"

#include <esp_err.h>
#include <esp_log.h>

static const char* TAG = "Settings";

namespace sys {

Settings::Settings(const std::string& ns, bool read_write) : ns_(ns), read_write_(read_write) {
    esp_err_t err = nvs_open(ns.c_str(), read_write_ ? NVS_READWRITE : NVS_READONLY, &nvs_handle_);
    if (err != ESP_OK && read_write_) {
        // A read-write open failing is abnormal (corrupt NVS handled earlier at boot).
        ESP_LOGE(TAG, "nvs_open('%s', RW) failed: %s", ns.c_str(), esp_err_to_name(err));
        nvs_handle_ = 0;
    }
    // Read-only open of a not-yet-existing namespace is normal: defaults are used.
}

Settings::~Settings() {
    if (nvs_handle_ != 0) {
        if (read_write_ && dirty_) {
            esp_err_t err = nvs_commit(nvs_handle_);
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "nvs_commit failed: %s (NVS full?)", esp_err_to_name(err));
            }
        }
        nvs_close(nvs_handle_);
    }
}

std::string Settings::GetString(const std::string& key, const std::string& default_value) {
    if (nvs_handle_ == 0) {
        return default_value;
    }
    size_t length = 0;
    if (nvs_get_str(nvs_handle_, key.c_str(), nullptr, &length) != ESP_OK) {
        return default_value;
    }
    // Soft getter: read errors fall back to the default (no abort); a value
    // that lengthened between probe and read is re-probed and retried once.
    std::string value;
    bool ok = length == 0;
    for (int attempt = 0; !ok && attempt < 2; ++attempt) {
        value.resize(length);
        if (nvs_get_str(nvs_handle_, key.c_str(), value.data(), &length) == ESP_OK) {
            ok = true;
            break;
        }
        if (nvs_get_str(nvs_handle_, key.c_str(), nullptr, &length) != ESP_OK) {
            return default_value;
        }
    }
    if (!ok) {
        return default_value;
    }
    // Some writers store a trailing NUL; strip it.
    while (!value.empty() && value.back() == '\0') {
        value.pop_back();
    }
    return value;
}

void Settings::SetString(const std::string& key, const std::string& value) {
    if (!read_write_ || nvs_handle_ == 0) {
        ESP_LOGW(TAG, "Namespace '%s' is not open for writing", ns_.c_str());
        return;
    }
    // Non-fatal on purpose: a full NVS partition must not abort (reboot loop).
    esp_err_t err = nvs_set_str(nvs_handle_, key.c_str(), value.c_str());
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_set_str('%s') failed: %s (NVS full?)", key.c_str(),
                 esp_err_to_name(err));
        return;
    }
    dirty_ = true;
}

bool Settings::GetBool(const std::string& key, bool default_value) {
    if (nvs_handle_ == 0) {
        return default_value;
    }
    uint8_t value = default_value ? 1 : 0;
    if (nvs_get_u8(nvs_handle_, key.c_str(), &value) != ESP_OK) {
        return default_value;
    }
    return value != 0;
}

void Settings::SetBool(const std::string& key, bool value) {
    if (!read_write_ || nvs_handle_ == 0) {
        ESP_LOGW(TAG, "Namespace '%s' is not open for writing", ns_.c_str());
        return;
    }
    esp_err_t err = nvs_set_u8(nvs_handle_, key.c_str(), value ? 1 : 0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_set_u8('%s') failed: %s (NVS full?)", key.c_str(), esp_err_to_name(err));
        return;
    }
    dirty_ = true;
}

void Settings::EraseKey(const std::string& key) {
    if (!read_write_ || nvs_handle_ == 0) {
        ESP_LOGW(TAG, "Namespace '%s' is not open for writing", ns_.c_str());
        return;
    }
    esp_err_t err = nvs_erase_key(nvs_handle_, key.c_str());
    if (err != ESP_OK && err != ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGE(TAG, "nvs_erase_key('%s') failed: %s", key.c_str(), esp_err_to_name(err));
        return;
    }
    dirty_ = true;
}

void Settings::EraseAll() {
    if (!read_write_ || nvs_handle_ == 0) {
        ESP_LOGW(TAG, "Namespace '%s' is not open for writing", ns_.c_str());
        return;
    }
    esp_err_t err = nvs_erase_all(nvs_handle_);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_erase_all failed: %s", esp_err_to_name(err));
        return;
    }
    dirty_ = true;
}

}  // namespace sys
