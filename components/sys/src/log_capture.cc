#include "sys/log_capture.h"

#include <esp_log.h>

#include <atomic>
#include <cstdarg>
#include <cstdio>

namespace sys {
namespace LogCapture {
namespace {

constexpr size_t kRingCapacity = 32 * 1024;
constexpr size_t kFragmentMax = 512;

std::atomic<vprintf_like_t> g_prev{nullptr};

// ESP-IDF v6 log_format_text.c:55 — one ESP_LOGx arrives as three
// separate vprintf fragments (prefix, body, "\n").
int Hook(const char* format, va_list args) {
    char buf[kFragmentMax];
    char scrubbed[kFragmentMax];
    va_list copy;
    va_copy(copy, args);
    const int n = vsnprintf(buf, sizeof(buf), format, copy);
    va_end(copy);
    if (n > 0) {
        // Cloud-controlled values reach the format args: strip CTLs (keep '\n').
        const size_t len =
            n < static_cast<int>(sizeof(buf)) ? static_cast<size_t>(n) : sizeof(buf) - 1;
        Buffer().AppendFragment(scrubbed, ScrubLogFragment(buf, len, scrubbed));
    }
    // Never log in here (recursion). Keep the UART output intact.
    const vprintf_like_t prev = g_prev.load(std::memory_order_acquire);
    if (prev != nullptr) {
        return prev(format, args);
    }
    return vprintf(format, args);  // the default sink (pre-assign window)
}

}  // namespace

LogBuffer& Buffer() {
    static LogBuffer buffer;  // Meyers singleton
    return buffer;
}

void Start() {
    static bool started = false;
    if (started) {
        return;
    }
    started = true;
    Buffer().Configure(kRingCapacity);
    g_prev.store(esp_log_set_vprintf(&Hook), std::memory_order_release);
}

}  // namespace LogCapture
}  // namespace sys
