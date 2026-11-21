#include "platform.h"

#include <math.h>
#include <string.h>
#include <time.h>

#include "audio.h"
#include "driver/rtc_io.h"
#include "esp_sleep.h"
#include "esp_wifi.h"
#include "ota.h"
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

static volatile bool s_dark;

static void leds_task(void *arg)
{
    for (;;) {
        if (s_dark) {
            vTaskDelay(pdMS_TO_TICKS(200));
            continue;
        }
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

// ---- brightness, restart, sleep (the long-press system menu) ----

static int s_bright = 100;
static esp_timer_handle_t s_bright_save;


static void bright_save_cb(void *arg) { settings_save_brightness(s_bright); }

void platform_set_brightness(int pct)
{
    if (pct < 5) pct = 5;
    if (pct > 100) pct = 100;
    s_bright = pct;
    board_backlight(pct);
    // Stored once the bezel has been still for a few seconds, not per click.
    if (!s_bright_save) {
        const esp_timer_create_args_t t = {.callback = bright_save_cb, .name = "bright_save"};
        esp_timer_create(&t, &s_bright_save);
    }
    if (s_bright_save) {
        esp_timer_stop(s_bright_save);
        esp_timer_start_once(s_bright_save, 3 * 1000 * 1000);
    }
}

int platform_get_brightness(void) { return s_bright; }

void platform_brightness_init(int pct)
{
    s_bright = pct < 5 ? 5 : pct > 100 ? 100 : pct;
}

void platform_restart(void)
{
    ESP_LOGI(TAG, "restart asked for");
    audio_stream_stop();
    if (s_bright_save && esp_timer_is_active(s_bright_save)) {
        esp_timer_stop(s_bright_save);
        settings_save_brightness(s_bright);
    }
    vTaskDelay(pdMS_TO_TICKS(100));  // let the audio fade
    esp_restart();
}

#define WAKE_PINS ((1ULL << PIN_TP_INT) | (1ULL << PIN_KNOB_A) | (1ULL << PIN_KNOB_B))

void platform_sleep(void)
{
    if (!ota_ready_for_sleep()) {
        ESP_LOGW(TAG, "not sleeping: a new image is still on trial");
        return;
    }
    ESP_LOGI(TAG, "going to sleep: touch the screen or turn the bezel to wake");
    if (s_bright_save && esp_timer_is_active(s_bright_save)) {
        esp_timer_stop(s_bright_save);
        settings_save_brightness(s_bright);
    }
    audio_stream_stop();
    s_dark = true;
    for (int b = s_bright; b >= 0; b -= 5) {
        board_backlight(b);
        vTaskDelay(pdMS_TO_TICKS(12));
    }
    board_backlight(0);
    for (int i = 0; i < RGB_LED_COUNT; i++) leds_set(i, 0, 0, 0);
    leds_show();
    gpio_set_level(PIN_PA_CTRL, 0);
    board_panel_sleep();
    esp_wifi_stop();

    // Wait for the finger that pressed "sleep" to lift (and the bezel to be
    // still), or the touch line would wake it straight away.
    for (int quiet = 0, t = 0; quiet < 50 && t < 300; t++) {
        bool any_low = !gpio_get_level(PIN_TP_INT) || !gpio_get_level(PIN_KNOB_A) || !gpio_get_level(PIN_KNOB_B);
        quiet = any_low ? 0 : quiet + 1;
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    // Backlight held off; the wake lines pulled up (the bezel has its own 10k
    // pull-ups, the touch line gets the RTC one).
    gpio_set_level(PIN_LCD_BLK, 0);
    gpio_hold_en(PIN_LCD_BLK);
    gpio_deep_sleep_hold_en();
    rtc_gpio_pullup_en(PIN_TP_INT);
    rtc_gpio_pulldown_dis(PIN_TP_INT);
    esp_sleep_pd_config(ESP_PD_DOMAIN_RTC_PERIPH, ESP_PD_OPTION_ON);
    esp_sleep_enable_ext1_wakeup_io(WAKE_PINS, ESP_EXT1_WAKEUP_ANY_LOW);
#ifdef SLEEP_TEST_TIMER_S
    esp_sleep_enable_timer_wakeup((uint64_t)SLEEP_TEST_TIMER_S * 1000000);
#endif
    esp_deep_sleep_start();
}

void platform_log_wake(void)
{
    if (esp_reset_reason() != ESP_RST_DEEPSLEEP) return;
    esp_sleep_wakeup_cause_t c = esp_sleep_get_wakeup_cause();
    if (c == ESP_SLEEP_WAKEUP_EXT1) {
        uint64_t m = esp_sleep_get_ext1_wakeup_status();
        ESP_LOGI(TAG, "woke from sleep: %s", m & (1ULL << PIN_TP_INT) ? "touch" : "bezel");
    } else {
        ESP_LOGI(TAG, "woke from sleep (cause %d)", (int)c);
    }
}
