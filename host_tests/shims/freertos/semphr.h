#pragma once

// Host shim shadowing <freertos/semphr.h>. Binary semaphore ops always
// succeed; the self-test suites never wait on one for real.

#include "freertos/FreeRTOS.h"

typedef void* SemaphoreHandle_t;

#ifdef __cplusplus
extern "C" {
#endif

SemaphoreHandle_t xSemaphoreCreateBinary(void);
SemaphoreHandle_t xSemaphoreCreateMutex(void);
BaseType_t xSemaphoreTake(SemaphoreHandle_t sem, TickType_t ticks);
BaseType_t xSemaphoreGive(SemaphoreHandle_t sem);
void vSemaphoreDelete(SemaphoreHandle_t sem);

#ifdef __cplusplus
}
#endif
