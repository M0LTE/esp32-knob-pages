// Device side of the UI's platform hooks: model lock and clock, alert sound,
// haptics and the LED ring.
#pragma once

#include <stdint.h>

void platform_early_init(void);  // model lock; first thing in app_main
void platform_init(void);        // after board, audio and haptic are up
uint32_t model_now_ms(void);     // the same clock as lv_tick_get()
uint32_t model_wall(void);       // unix seconds, 0 if the clock is not set
void platform_knob(int delta);   // bezel step: haptic tick, then ui_knob()
void platform_test_sound(void);  // the alert sound, ignoring settings except volume
void platform_alert_cleared(void);  // the UI's alert was acknowledged or went away
