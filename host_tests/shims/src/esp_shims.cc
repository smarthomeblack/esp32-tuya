// Host shim implementations: log capture, RNG, timer, FreeRTOS stubs.

#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include <string>
#include <vector>

namespace {

std::vector<std::string>& Lines() {
    static std::vector<std::string> lines;
    return lines;
}

}  // namespace

extern "C" {

void host_log_capture(char level, const char* tag, const char* fmt, ...) {
    char buf[640];
    va_list ap;
    va_start(ap, fmt);
    const int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (n < 0) {
        return;
    }
    buf[sizeof(buf) - 1] = '\0';
    printf("%c %s: %s\n", level, tag, buf);
    Lines().emplace_back(std::string(tag) + ": " + buf);
}

size_t host_log_count(void) {
    return Lines().size();
}

const char* host_log_line(size_t index) {
    if (index >= Lines().size()) {
        return nullptr;
    }
    return Lines()[index].c_str();
}

void host_log_reset(void) {
    Lines().clear();
}

void esp_fill_random(void* out, size_t len) {
    // Fixed-seed xorshift: host runs stay byte-reproducible.
    static uint64_t s = 0x9e3779b97f4a7c15ull;
    uint8_t* p = static_cast<uint8_t*>(out);
    for (size_t i = 0; i < len; ++i) {
        s ^= s << 13;
        s ^= s >> 7;
        s ^= s << 17;
        p[i] = static_cast<uint8_t>(s >> 32);
    }
}

int64_t esp_timer_get_time(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<int64_t>(ts.tv_sec) * 1000000 + ts.tv_nsec / 1000;
}

BaseType_t xTaskCreate(TaskFunction_t fn, const char* const name, uint32_t stack_bytes,
                       void* const arg, UBaseType_t prio, TaskHandle_t* const handle) {
    (void)fn;
    (void)name;
    (void)stack_bytes;
    (void)arg;
    (void)prio;
    if (handle != nullptr) {
        *handle = nullptr;
    }
    return pdFAIL;
}

TaskHandle_t xTaskGetCurrentTaskHandle(void) {
    return nullptr;
}

void vTaskDelete(TaskHandle_t task) {
    (void)task;
}

void vTaskDelay(TickType_t ticks) {
    (void)ticks;
}

SemaphoreHandle_t xSemaphoreCreateBinary(void) {
    static int dummy = 1;  // unique non-null handle; ops are no-ops
    return reinterpret_cast<SemaphoreHandle_t>(&dummy);
}

SemaphoreHandle_t xSemaphoreCreateMutex(void) {
    static int dummy = 2;
    return reinterpret_cast<SemaphoreHandle_t>(&dummy);
}

BaseType_t xSemaphoreTake(SemaphoreHandle_t sem, TickType_t ticks) {
    (void)sem;
    (void)ticks;
    return pdTRUE;
}

BaseType_t xSemaphoreGive(SemaphoreHandle_t sem) {
    (void)sem;
    return pdTRUE;
}

void vSemaphoreDelete(SemaphoreHandle_t sem) {
    (void)sem;
}

}  // extern "C"
