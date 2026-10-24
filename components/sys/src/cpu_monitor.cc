#include "sys/cpu_monitor.h"

#include <esp_log.h>
#include <esp_timer.h>

#include <atomic>

#include "sdkconfig.h"

#if CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS
#include <freertos/FreeRTOS.h>
#include <freertos/idf_additions.h>

static const char* TAG = "CpuMonitor";
#endif

namespace sys {
namespace CpuMonitor {
namespace {

std::atomic<bool> g_valid{false};
std::atomic<int> g_usage{0};
std::atomic<int> g_core0{0};
std::atomic<int> g_core1{0};

int ClampPct(int64_t pct) {
    if (pct < 0) {
        return 0;
    }
    if (pct > 100) {
        return 100;
    }
    return static_cast<int>(pct);
}

#if CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS

constexpr int64_t kSamplePeriodUs = 5000000;  // 5 s

// Single-writer state: only the esp_timer callback touches it.
CpuSample g_prev{};

// ulTaskGetIdleRunTimeCounterForCore: freertos_tasks_c_additions.h:487
// (idle counters are esp_timer us — the default stats clock).
CpuSample TakeSample() {
    CpuSample s{};
    s.t_us = static_cast<uint64_t>(esp_timer_get_time());
    s.idle0_us = static_cast<uint64_t>(ulTaskGetIdleRunTimeCounterForCore(0));
    s.idle1_us = static_cast<uint64_t>(ulTaskGetIdleRunTimeCounterForCore(1));
    return s;
}

void PublishSample(const CpuSample& now) {
    const int usage = ComputeCpuUsagePct(g_prev, now);
    if (usage < 0) {
        return;  // invalid window — keep the last published values
    }
    g_usage.store(usage);
    g_core0.store(ComputeCoreUsagePct(g_prev, now, 0));
    g_core1.store(ComputeCoreUsagePct(g_prev, now, 1));
    g_valid.store(true);
    g_prev = now;
}

void OnSample(void* /*arg*/) {
    PublishSample(TakeSample());
}

#endif  // CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS

}  // namespace

int ComputeCpuUsagePct(const CpuSample& a, const CpuSample& b) {
    if (b.t_us <= a.t_us) {
        return -1;
    }
    const uint64_t wall = b.t_us - a.t_us;
    const uint64_t idle = (b.idle0_us - a.idle0_us) + (b.idle1_us - a.idle1_us);
    const uint64_t capacity = 2 * wall;  // both cores
    if (idle >= capacity) {
        return 0;  // fully idle (or a counter glitch) — never negative
    }
    return ClampPct(100 - static_cast<int64_t>((idle * 100) / capacity));
}

int ComputeCoreUsagePct(const CpuSample& a, const CpuSample& b, int core) {
    if (b.t_us <= a.t_us || (core != 0 && core != 1)) {
        return -1;
    }
    const uint64_t wall = b.t_us - a.t_us;
    const uint64_t idle = core == 0 ? (b.idle0_us - a.idle0_us) : (b.idle1_us - a.idle1_us);
    if (idle >= wall) {
        return 0;
    }
    return ClampPct(100 - static_cast<int64_t>((idle * 100) / wall));
}

void Start() {
    static bool started = false;
    if (started) {
        return;
    }
    started = true;
#if CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS
    g_prev = TakeSample();  // base sample — first valid value at +5 s
    const esp_timer_create_args_t timer_args = {
        .callback = &OnSample,
        .arg = nullptr,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "cpu_mon",
        .skip_unhandled_events = true,
    };
    esp_timer_handle_t timer = nullptr;
    esp_err_t err = esp_timer_create(&timer_args, &timer);
    if (err != ESP_OK || timer == nullptr) {
        ESP_LOGW(TAG, "cpu monitor timer create failed: %s", esp_err_to_name(err));
        return;
    }
    err = esp_timer_start_periodic(timer, kSamplePeriodUs);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "cpu monitor timer start failed: %s", esp_err_to_name(err));
        esp_timer_delete(timer);
    }
#endif
}

Snapshot Get() {
    Snapshot snapshot{};
    snapshot.valid = g_valid.load();
    snapshot.usage_pct = g_usage.load();
    snapshot.core0_pct = g_core0.load();
    snapshot.core1_pct = g_core1.load();
    return snapshot;
}

}  // namespace CpuMonitor
}  // namespace sys
