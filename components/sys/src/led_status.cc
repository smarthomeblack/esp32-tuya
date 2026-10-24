#include "sys/led_status.h"

#include <driver/gpio.h>
#include <esp_err.h>
#include <esp_log.h>

#include "sdkconfig.h"

static const char* TAG = "LedStatus";

namespace sys {

#if CONFIG_SYS_LED_GPIO >= 0
constexpr int kLedGpio = CONFIG_SYS_LED_GPIO;
#endif

void LedStatus::Start() {
#if CONFIG_SYS_LED_GPIO >= 0
    gpio_config_t io_conf = {};
    io_conf.pin_bit_mask = 1ULL << kLedGpio;
    io_conf.mode = GPIO_MODE_OUTPUT;
    io_conf.pull_up_en = GPIO_PULLUP_DISABLE;
    io_conf.pull_down_en = GPIO_PULLDOWN_DISABLE;
    io_conf.intr_type = GPIO_INTR_DISABLE;
    ESP_ERROR_CHECK(gpio_config(&io_conf));
    ESP_ERROR_CHECK(gpio_set_level(static_cast<gpio_num_t>(kLedGpio), 0));

    const esp_timer_create_args_t timer_args = {
        .callback = [](void* arg) { static_cast<LedStatus*>(arg)->OnBlinkTick(); },
        .arg = this,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "led_blink",
        .skip_unhandled_events = true,
    };
    ESP_ERROR_CHECK(esp_timer_create(&timer_args, &blink_timer_));
    ESP_LOGI(TAG, "Status LED on GPIO %d", kLedGpio);
#else
    ESP_LOGI(TAG, "No status LED configured (headless board)");
#endif
}

void LedStatus::OnStateChanged(DeviceState state) {
#if CONFIG_SYS_LED_GPIO >= 0
    switch (state) {
        case kDeviceStateStarting:
        case kDeviceStateWifiConfig:
        case kDeviceStateNeedsSetup:
        case kDeviceStateCloudConnecting:
            SetBlink(500000);  // 500ms blink
            break;
        case kDeviceStateLinked: SetSolid(true); break;
        case kDeviceStateIdle: SetSolid(false); break;
        case kDeviceStateFatalError:
            SetBlink(150000);  // fast blink
            break;
        default: break;
    }
#endif
}

void LedStatus::SetSolid(bool on) {
#if CONFIG_SYS_LED_GPIO >= 0
    StopBlink();
    led_level_.store(on);
    gpio_set_level(static_cast<gpio_num_t>(kLedGpio), on ? 1 : 0);
#else
    (void)on;
#endif
}

void LedStatus::SetBlink(uint64_t period_us) {
#if CONFIG_SYS_LED_GPIO >= 0
    if (blink_timer_ == nullptr) {
        return;  // Start() not called yet (or no LED compiled in)
    }
    // Stop first: esp_timer_start_periodic errors when the timer is already running.
    esp_timer_stop(blink_timer_);
    esp_err_t err = esp_timer_start_periodic(blink_timer_, period_us);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "esp_timer_start_periodic failed: %s", esp_err_to_name(err));
    }
#else
    (void)period_us;
#endif
}

void LedStatus::StopBlink() {
#if CONFIG_SYS_LED_GPIO >= 0
    if (blink_timer_ != nullptr) {
        esp_timer_stop(blink_timer_);
    }
#endif
}

void LedStatus::OnBlinkTick() {
#if CONFIG_SYS_LED_GPIO >= 0
    bool next = !led_level_.load();
    led_level_.store(next);
    gpio_set_level(static_cast<gpio_num_t>(kLedGpio), next ? 1 : 0);
#endif
}

}  // namespace sys
