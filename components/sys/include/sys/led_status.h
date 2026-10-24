#pragma once

#include <esp_timer.h>

#include <atomic>

#include "sys/device_state.h"

namespace sys {

/// Status LED: 500ms blink starting/provisioning, solid Linked, 150ms blink
/// FatalError, off Idle; no-op when CONFIG_SYS_LED_GPIO < 0.
class LedStatus {
public:
    static LedStatus& GetInstance() {
        static LedStatus instance;
        return instance;
    }
    LedStatus(const LedStatus&) = delete;
    LedStatus& operator=(const LedStatus&) = delete;

    /// Configures the GPIO and blink timer. Safe to call once at boot.
    void Start();

    /// Called whenever the hub state changes.
    void OnStateChanged(DeviceState state);

private:
    LedStatus() = default;
    void SetSolid(bool on);
    void SetBlink(uint64_t period_us);
    void StopBlink();
    void OnBlinkTick();

    esp_timer_handle_t blink_timer_ = nullptr;
    std::atomic<bool> led_level_{false};
};

}  // namespace sys
