#pragma once
#include <driver/gpio.h>

#include <functional>

// The charger IC on this board exposes plain digital status pins
// (not an ADC), so this is a simplified charging-only power manager:
// no continuous battery-percentage estimate is available.
class PowerManager {
   private:
    gpio_num_t charging_pin_;
    bool is_charging_ = false;

   public:
    PowerManager(gpio_num_t charging_pin) : charging_pin_(charging_pin) {
        gpio_config_t io_conf = {};
        io_conf.intr_type = GPIO_INTR_DISABLE;
        io_conf.mode = GPIO_MODE_INPUT;
        io_conf.pin_bit_mask = (1ULL << charging_pin_);
        io_conf.pull_down_en = GPIO_PULLDOWN_DISABLE;
        io_conf.pull_up_en = GPIO_PULLUP_DISABLE;
        gpio_config(&io_conf);
        is_charging_ = gpio_get_level(charging_pin_) == 1;
    }

    bool IsCharging() {
        is_charging_ = gpio_get_level(charging_pin_) == 1;
        return is_charging_;
    }

    bool IsDischarging() { return !IsCharging(); }

    uint8_t GetBatteryLevel() { return 100; }
};
