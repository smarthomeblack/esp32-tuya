// Host shim implementing shims/nvs.h with an in-memory store.

#include "nvs.h"

#include <cstring>

#include <map>
#include <string>
#include <vector>

namespace {

struct NvsNamespace {
    std::map<std::string, std::string> strings;
    std::map<std::string, uint8_t> u8s;
};

std::map<std::string, NvsNamespace>& Store() {
    static std::map<std::string, NvsNamespace> store;
    return store;
}

std::map<uint32_t, std::string>& Handles() {
    static std::map<uint32_t, std::string> handles;
    return handles;
}

uint32_t NextHandle() {
    static uint32_t next = 1;
    return next++;
}

}  // namespace

extern "C" {

esp_err_t nvs_open(const char* namespace_name, nvs_open_mode_t open_mode,
                   nvs_handle_t* out_handle) {
    if (namespace_name == nullptr || out_handle == nullptr) {
        return ESP_ERR_NVS_INVALID_STATE;
    }
    if (open_mode == NVS_READONLY && Store().find(namespace_name) == Store().end()) {
        // Read-only open of a missing namespace is the normal "defaults" path.
        return ESP_ERR_NVS_NOT_FOUND;
    }
    const uint32_t handle = NextHandle();
    Handles()[handle] = namespace_name;
    *out_handle = handle;
    return ESP_OK;
}

void nvs_close(nvs_handle_t handle) {
    Handles().erase(handle);
}

esp_err_t nvs_get_str(nvs_handle_t handle, const char* key, char* out_value, size_t* length) {
    const auto hit = Handles().find(handle);
    if (hit == Handles().end() || key == nullptr || length == nullptr) {
        return ESP_ERR_NVS_INVALID_STATE;
    }
    const auto ns = Store().find(hit->second);
    if (ns == Store().end()) {
        return ESP_ERR_NVS_NOT_FOUND;
    }
    const auto entry = ns->second.strings.find(key);
    if (entry == ns->second.strings.end()) {
        return ESP_ERR_NVS_NOT_FOUND;
    }
    const size_t with_nul = entry->second.size() + 1;  // NVS lengths include the NUL
    if (out_value == nullptr) {
        *length = with_nul;
        return ESP_OK;
    }
    if (*length < with_nul) {
        *length = with_nul;
        return ESP_ERR_NVS_INVALID_STATE;
    }
    std::memcpy(out_value, entry->second.data(), entry->second.size());
    out_value[entry->second.size()] = '\0';
    *length = with_nul;
    return ESP_OK;
}

esp_err_t nvs_set_str(nvs_handle_t handle, const char* key, const char* value) {
    const auto hit = Handles().find(handle);
    if (hit == Handles().end() || key == nullptr || value == nullptr) {
        return ESP_ERR_NVS_INVALID_STATE;
    }
    Store()[hit->second].strings[key] = value;
    return ESP_OK;
}

esp_err_t nvs_get_u8(nvs_handle_t handle, const char* key, uint8_t* out_value) {
    const auto hit = Handles().find(handle);
    if (hit == Handles().end() || key == nullptr || out_value == nullptr) {
        return ESP_ERR_NVS_INVALID_STATE;
    }
    const auto ns = Store().find(hit->second);
    if (ns == Store().end()) {
        return ESP_ERR_NVS_NOT_FOUND;
    }
    const auto entry = ns->second.u8s.find(key);
    if (entry == ns->second.u8s.end()) {
        return ESP_ERR_NVS_NOT_FOUND;
    }
    *out_value = entry->second;
    return ESP_OK;
}

esp_err_t nvs_set_u8(nvs_handle_t handle, const char* key, uint8_t value) {
    const auto hit = Handles().find(handle);
    if (hit == Handles().end() || key == nullptr) {
        return ESP_ERR_NVS_INVALID_STATE;
    }
    Store()[hit->second].u8s[key] = value;
    return ESP_OK;
}

esp_err_t nvs_commit(nvs_handle_t handle) {
    return Handles().find(handle) == Handles().end() ? ESP_ERR_NVS_INVALID_STATE : ESP_OK;
}

esp_err_t nvs_erase_key(nvs_handle_t handle, const char* key) {
    const auto hit = Handles().find(handle);
    if (hit == Handles().end() || key == nullptr) {
        return ESP_ERR_NVS_INVALID_STATE;
    }
    const auto ns = Store().find(hit->second);
    if (ns == Store().end() || ns->second.strings.erase(key) == 0) {
        return ESP_ERR_NVS_NOT_FOUND;
    }
    return ESP_OK;
}

esp_err_t nvs_erase_all(nvs_handle_t handle) {
    const auto hit = Handles().find(handle);
    if (hit == Handles().end()) {
        return ESP_ERR_NVS_INVALID_STATE;
    }
    Store()[hit->second] = NvsNamespace{};
    return ESP_OK;
}

const char* esp_err_to_name(esp_err_t err) {
    switch (err) {
        case ESP_OK: return "ESP_OK";
        case ESP_ERR_NVS_NOT_FOUND: return "ESP_ERR_NVS_NOT_FOUND";
        case ESP_ERR_NVS_INVALID_STATE: return "ESP_ERR_NVS_INVALID_STATE";
        default: return "ESP_FAIL";
    }
}

}  // extern "C"
