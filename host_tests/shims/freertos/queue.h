#pragma once

// Host shim shadowing <freertos/queue.h>: opaque handle type only.

#include "freertos/FreeRTOS.h"

typedef void* QueueHandle_t;
