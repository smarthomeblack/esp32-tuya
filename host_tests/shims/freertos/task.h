#pragma once

// Host shim shadowing <freertos/task.h>. Task creation always fails; the
// self-test suites never spawn tasks, and callers treat failure as defer.

#include "freertos/FreeRTOS.h"

typedef void* TaskHandle_t;
typedef void (*TaskFunction_t)(void*);

#ifdef __cplusplus
extern "C" {
#endif

BaseType_t xTaskCreate(TaskFunction_t fn, const char* const name, uint32_t stack_bytes,
                       void* const arg, UBaseType_t prio, TaskHandle_t* const handle);
TaskHandle_t xTaskGetCurrentTaskHandle(void);
void vTaskDelete(TaskHandle_t task);
void vTaskDelay(TickType_t ticks);

#ifdef __cplusplus
}
#endif
