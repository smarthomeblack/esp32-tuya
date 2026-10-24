#pragma once

// Host shim shadowing <esp_log.h>: printf-style output whose lines are also
// captured, so the runner can assert the exact per-suite PASS lines.

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

void host_log_capture(char level, const char* tag, const char* fmt, ...)
    __attribute__((format(printf, 3, 4)));

// Captured-line access for the runner (index 0 = oldest).
size_t host_log_count(void);
const char* host_log_line(size_t index);  // NULL when out of range
void host_log_reset(void);

#ifdef __cplusplus
}
#endif

#define HOST_LOG(level, tag, fmt, ...) host_log_capture(level, tag, fmt, ##__VA_ARGS__)

#define ESP_LOGI(tag, fmt, ...) HOST_LOG('I', tag, fmt, ##__VA_ARGS__)
#define ESP_LOGW(tag, fmt, ...) HOST_LOG('W', tag, fmt, ##__VA_ARGS__)
#define ESP_LOGE(tag, fmt, ...) HOST_LOG('E', tag, fmt, ##__VA_ARGS__)
#define ESP_LOGD(tag, fmt, ...) HOST_LOG('D', tag, fmt, ##__VA_ARGS__)
