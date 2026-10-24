#pragma once

// Host shim shadowing <esp_timer.h>: CLOCK_MONOTONIC microseconds.

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

int64_t esp_timer_get_time(void);

#ifdef __cplusplus
}
#endif
