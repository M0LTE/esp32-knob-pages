// Pin map and board bring-up for the Guition JC3636K718 knob display.
// See docs/hardware.md for sources. Display, touch and bezel code is the
// bring-up app's (hello/), proven on this hardware.
#pragma once

#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "lvgl.h"

#define LCD_H_RES 360
#define LCD_V_RES 360

#define PIN_LCD_SCK   GPIO_NUM_11
#define PIN_LCD_CS    GPIO_NUM_12
#define PIN_LCD_D0    GPIO_NUM_13
#define PIN_LCD_D1    GPIO_NUM_14
#define PIN_LCD_D2    GPIO_NUM_15
#define PIN_LCD_D3    GPIO_NUM_16
#define PIN_LCD_RST   GPIO_NUM_17
#define PIN_LCD_TE    GPIO_NUM_18
#define PIN_LCD_BLK   GPIO_NUM_21

#define PIN_I2C_SDA   GPIO_NUM_9
#define PIN_I2C_SCL   GPIO_NUM_10
#define PIN_TP_INT    GPIO_NUM_7
#define PIN_TP_RST    GPIO_NUM_8

#define PIN_KNOB_A    GPIO_NUM_2
#define PIN_KNOB_B    GPIO_NUM_1

#define PIN_I2S_BCK   GPIO_NUM_3
#define PIN_I2S_WS    GPIO_NUM_45
#define PIN_I2S_DO    GPIO_NUM_42
#define PIN_PA_CTRL   GPIO_NUM_46   // high enables the speaker amp

#define PIN_RGB_DATA  GPIO_NUM_0    // shared with the BOOT strap
#define RGB_LED_COUNT 12

typedef void (*knob_step_cb_t)(int delta);

void board_init(void);
lv_display_t *board_display(void);
i2c_master_bus_handle_t board_i2c(void);
void board_set_knob_cb(knob_step_cb_t cb);
void board_backlight(uint8_t percent);
// The scope: true waits (up to 20 ms) for the display bus to fall idle and
// keeps it idle; false lets the display carry on. True returns false if the
// bus never fell idle (then there is nothing to release).
bool board_lcd_quiet(bool on);

void leds_set(int i, uint8_t r, uint8_t g, uint8_t b);
void leds_show(void);

// Display performance since the last call: frames rendered, mean and max time
// per frame (render plus flush), and the part of that spent waiting for the bus.
typedef struct {
    uint32_t frames;
    float fps;
    float frame_ms_avg, frame_ms_max;
    float wait_ms_avg;
} disp_perf_t;
void board_disp_perf(disp_perf_t *out);
uint32_t board_frames_total(void);
void board_panel_sleep(void);       // display off and sleep-in (for deep sleep)
void board_panel_on(bool on);       // display on / off (the panel keeps its contents and settings)
// Every touch read passes through filter (LVGL task); returning true hides it
// from the UI (the reading becomes "released").
void board_set_touch_filter(bool (*filter)(bool pressed));  // frames drawn since boot
