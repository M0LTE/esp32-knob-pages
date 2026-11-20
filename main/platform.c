#include "platform.h"

#include <math.h>
#include <string.h>
#include <time.h>

#include "audio.h"
#include "board.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "freertos/idf_additions.h"
#include "esp_heap_caps.h"
#include "haptic.h"
#include "model.h"
#include "settings.h"
#include "ui.h"

static const char *TAG = "platform";

// ---- model lock and clock ----

static SemaphoreHandle_t s_model_mux;

void model_lock(void) { xSemaphoreTake(s_model_mux, portMAX_DELAY); }
void model_unlock(void) { xSemaphoreGive(s_model_mux); }
uint32_t model_now_ms(void) { return (uint32_t)(esp_timer_get_time() / 1000); }

uint32_t model_wall(void)
{
    time_t t = time(NULL);
    return t > 1700000000 ? (uint32_t)t : 0;
}

// ---- haptics, off the timer and LVGL tasks (I2C is slow-ish) ----

static QueueHandle_t s_haptic_q;

static void haptic_task(void *arg)
{
    uint8_t e;
    for (;;)
        if (xQueueReceive(s_haptic_q, &e, portMAX_DELAY) == pdTRUE) haptic_play(e);
}

static void haptic(uint8_t effect)
{
    if (s_haptic_q) xQueueSend(s_haptic_q, &effect, 0);
}

void platform_knob(int delta)
{
    haptic(HAPTIC_SHARP_TICK);
    ui_knob(delta);
}

// ---- LED ring ----
// Ambient: all twelve very dim warm white, the current page's LED faintly amber.
// Alert: the whole ring pulses red (critical) or amber (warning) until the
// alert is acknowledged or clears, or for at most ALERT_MAX_MS.

#define ALERT_MAX_MS (10 * 60 * 1000)

static volatile int s_page, s_pages = 1;
static volatile alert_level_t s_alert;
static volatile bool s_notice;            // a notice's one sweep round the ring is running
static volatile uint32_t s_notice_since;
static volatile uint32_t s_alert_since;

void platform_page_changed(int index, int count)
{
    s_page = index;
    s_pages = count > 0 ? count : 1;
}

void platform_alert_cleared(void)
{
    s_alert = ALERT_NONE;
    s_notice = false;
}

#define NOTICE_SWEEP_MS 1400

static void leds_task(void *arg)
{
    for (;;) {
        uint32_t now = model_now_ms();
        alert_level_t a = s_alert;
        if (a != ALERT_NONE && now - s_alert_since > ALERT_MAX_MS) s_alert = a = ALERT_NONE;
        if (s_notice && now - s_notice_since >= NOTICE_SWEEP_MS) s_notice = false;
        if (s_notice && a != ALERT_WARN && a != ALERT_CRIT) {
            // One gentle amber sweep round the ring with a fading tail.
            float pos = (float)(now - s_notice_since) / NOTICE_SWEEP_MS * RGB_LED_COUNT;
            for (int i = 0; i < RGB_LED_COUNT; i++) {
                float behind = pos - i;
                float k = behind >= 0 && behind < 4 ? 1.0f - behind / 4 : 0;
                leds_set(i, (uint8_t)(2 + 22 * k), (uint8_t)(2 + 9 * k), (uint8_t)(1 + 0 * k));
            }
            leds_show();
            vTaskDelay(pdMS_TO_TICKS(30));
            continue;
        }
        if (a == ALERT_NOTICE) a = ALERT_NONE;  // notices do not pulse
        if (a != ALERT_NONE) {
            float k = (now % 1400) / 1400.0f;
            float pulse = 0.5f - 0.5f * cosf(k * 2 * (float)M_PI);
            uint8_t v = (uint8_t)(3 + 37 * pulse);
            for (int i = 0; i < RGB_LED_COUNT; i++) {
                if (a == ALERT_CRIT) leds_set(i, v, 0, 0);
                else leds_set(i, v, (uint8_t)(v * 0.45f), 0);
            }
        } else {
            for (int i = 0; i < RGB_LED_COUNT; i++) leds_set(i, 2, 2, 1);
            int at = s_pages > 1 ? s_page * RGB_LED_COUNT / s_pages : 0;
            leds_set(at, 10, 4, 0);
        }
        leds_show();
        vTaskDelay(pdMS_TO_TICKS(a != ALERT_NONE ? 30 : s_notice ? 30 : 200));
    }
}

// ---- sound ----

static void alert_sound(float vol)
{
    audio_play(988, 1480, 220, vol);
    audio_play(659, 988, 380, vol);
}

void platform_test_sound(void)
{
    alert_sound(settings_alert_volume(true) / 100.0f * 0.6f);
}

void platform_alert(alert_level_t level, const char *page_title, const char *why)
{
    ESP_LOGW(TAG, "ALERT %s %s: %s",
             level == ALERT_CRIT ? "CRIT" : level == ALERT_WARN ? "WARN" : level == ALERT_NOTICE ? "NOTICE" : "NONE",
             page_title, why);
    if (level == ALERT_NOTICE) {
        // A passing event: one soft chime, a light tap, one sweep round the ring.
        s_notice = true;
        s_notice_since = model_now_ms();
        haptic(HAPTIC_SHARP_TICK);
        int vol = settings_alert_volume(false);
        if (vol > 0) {
            audio_play(1319, 1976, 140, vol / 100.0f * 0.22f);
            audio_play(1760, 2637, 260, vol / 100.0f * 0.18f);
        }
        return;
    }
    s_alert = level;
    s_alert_since = model_now_ms();
    if (level != ALERT_CRIT) return;
    haptic(HAPTIC_BUZZ);
    int vol = settings_alert_volume(false);
    if (vol > 0) alert_sound(vol / 100.0f * 0.6f);
}

void platform_init(void)
{
    s_haptic_q = xQueueCreate(8, 1);
    xTaskCreatePinnedToCoreWithCaps(haptic_task, "haptic", 3072, NULL, 4, NULL, 0, MALLOC_CAP_SPIRAM);
    xTaskCreatePinnedToCoreWithCaps(leds_task, "leds", 3072, NULL, 3, NULL, 0, MALLOC_CAP_SPIRAM);
}

void platform_early_init(void) { s_model_mux = xSemaphoreCreateMutex(); }
