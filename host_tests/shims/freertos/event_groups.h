#pragma once

// Host shim shadowing <freertos/event_groups.h>: opaque handle types only.

#include "freertos/FreeRTOS.h"

typedef void* EventGroupHandle_t;
typedef uint32_t EventBits_t;
