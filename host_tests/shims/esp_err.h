#pragma once

// Host shim shadowing <esp_err.h>: the error codes the compiled TUs use.

#ifdef __cplusplus
extern "C" {
#endif

typedef int esp_err_t;

#define ESP_OK 0
#define ESP_FAIL -1
#define ESP_ERR_NVS_NOT_FOUND 0x1102
#define ESP_ERR_NVS_INVALID_STATE 0x1103

const char* esp_err_to_name(esp_err_t err);

#ifdef __cplusplus
}
#endif
