#pragma once

// Host shim shadowing <nvs.h>: an in-memory key-value store, so the real
// sys::Settings code runs unmodified (a fresh process starts with defaults).

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef uint32_t nvs_handle_t;

typedef enum {
    NVS_READONLY,
    NVS_READWRITE,
} nvs_open_mode_t;

esp_err_t nvs_open(const char* namespace_name, nvs_open_mode_t open_mode, nvs_handle_t* out_handle);
void nvs_close(nvs_handle_t handle);
esp_err_t nvs_get_str(nvs_handle_t handle, const char* key, char* out_value, size_t* length);
esp_err_t nvs_set_str(nvs_handle_t handle, const char* key, const char* value);
esp_err_t nvs_get_u8(nvs_handle_t handle, const char* key, uint8_t* out_value);
esp_err_t nvs_set_u8(nvs_handle_t handle, const char* key, uint8_t value);
esp_err_t nvs_commit(nvs_handle_t handle);
esp_err_t nvs_erase_key(nvs_handle_t handle, const char* key);
esp_err_t nvs_erase_all(nvs_handle_t handle);

#ifdef __cplusplus
}
#endif
