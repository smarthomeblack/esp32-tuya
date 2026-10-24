#include "hub_core/main_thread.h"

#include <esp_log.h>

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <atomic>

static const char* TAG = "MainThread";

namespace hub_core {

namespace {

// Written once at loop start; read from any task.
std::atomic<TaskHandle_t> g_main_task{nullptr};

}  // namespace

void SetMainThread(void* handle) {
    g_main_task.store(static_cast<TaskHandle_t>(handle));
}

void AssertMainThread(const char* api) {
    const TaskHandle_t main_task = g_main_task.load();
    if (main_task == nullptr) {
        return;  // no handle registered yet
    }
    if (xTaskGetCurrentTaskHandle() != main_task) {
        ESP_LOGE(TAG, "%s called from wrong task", api);
    }
}

}  // namespace hub_core
