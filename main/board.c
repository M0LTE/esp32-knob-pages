// Board bring-up: QSPI display, touch, bezel, LED ring, backlight.
// Display, touch and bezel set-up come from the hello bring-up app.
#include "board.h"

#include <string.h>

#include "driver/ledc.h"
#include "driver/spi_master.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_st77916.h"
#include "esp_lcd_touch_cst816s.h"
#include "esp_log.h"
#include "esp_lvgl_port.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "led_strip.h"

static const char *TAG = "board";

#include "st77916_init.h"

// LVGL renders bands of BAND_LINES lines straight into two internal DMA
// buffers, already in the panel's byte order (RGB565_SWAPPED, so nothing is
// swapped per frame), and each band goes out on the QSPI bus while the next is
// drawn. The pages redraw the whole screen every frame; what keeps that cheap
// is in ui/theme.c (cached scenery, strip-clipped shapes). Measured choices:
// full frames in PSRAM spent about 24 ms a frame just moving memory (PSRAM
// fills at 35 MB/s here); 90-line bands beat 60 by 2 to 3 ms a frame but
// left no internal RAM to spare once Wi-Fi was up; 64 (was 72) since the
// memory pass, see docs/memory.md.
#ifndef BAND_LINES
#define BAND_LINES 64
#endif

static lv_display_t *s_disp;
static i2c_master_bus_handle_t s_i2c;
static led_strip_handle_t s_leds;
static knob_step_cb_t s_knob_cb;

lv_display_t *board_display(void) { return s_disp; }
i2c_master_bus_handle_t board_i2c(void) { return s_i2c; }
void board_set_knob_cb(knob_step_cb_t cb) { s_knob_cb = cb; }

void board_backlight(uint8_t percent)
{
    if (percent > 100) percent = 100;
    ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, (1023 * percent) / 100);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
}

static void backlight_init(void)
{
    ledc_timer_config_t t = {
        .speed_mode = LEDC_LOW_SPEED_MODE, .duty_resolution = LEDC_TIMER_10_BIT,
        .timer_num = LEDC_TIMER_0, .freq_hz = 20000, .clk_cfg = LEDC_AUTO_CLK,
    };
    ESP_ERROR_CHECK(ledc_timer_config(&t));
    ledc_channel_config_t c = {
        .gpio_num = PIN_LCD_BLK, .speed_mode = LEDC_LOW_SPEED_MODE, .channel = LEDC_CHANNEL_0,
        .timer_sel = LEDC_TIMER_0, .duty = 0,
    };
    ESP_ERROR_CHECK(ledc_channel_config(&c));
}

static void i2c_init(void)
{
    i2c_master_bus_config_t cfg = {
        .i2c_port = I2C_NUM_0, .sda_io_num = PIN_I2C_SDA, .scl_io_num = PIN_I2C_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT, .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = 1,
    };
    ESP_ERROR_CHECK(i2c_new_master_bus(&cfg, &s_i2c));
}

// Frame timing, from LVGL's display events (all on the LVGL task).
static int64_t s_render_t0, s_wait_t0, s_wait_acc;
static uint32_t s_frames;
static volatile uint32_t s_frames_total;
uint32_t board_frames_total(void) { return s_frames_total; }
static int64_t s_frame_us_sum, s_frame_us_max, s_wait_us_sum, s_perf_since;

static void perf_cb(lv_event_t *e)
{
    int64_t now = esp_timer_get_time();
    switch (lv_event_get_code(e)) {
    case LV_EVENT_RENDER_START:
        s_render_t0 = now;
        s_wait_acc = 0;
        break;
    case LV_EVENT_FLUSH_WAIT_START:
        s_wait_t0 = now;
        break;
    case LV_EVENT_FLUSH_WAIT_FINISH:
        s_wait_acc += now - s_wait_t0;
        break;
    case LV_EVENT_REFR_READY:
        if (s_render_t0) {
            int64_t d = now - s_render_t0;
            s_frames++;
            s_frames_total++;
            s_frame_us_sum += d;
            s_wait_us_sum += s_wait_acc;
            if (d > s_frame_us_max) s_frame_us_max = d;
            s_render_t0 = 0;
        }
        break;
    default:
        break;
    }
}

void board_disp_perf(disp_perf_t *o)
{
    lvgl_port_lock(0);
    int64_t now = esp_timer_get_time();
    float secs = (now - s_perf_since) / 1e6f;
    o->frames = s_frames;
    o->fps = secs > 0 ? s_frames / secs : 0;
    o->frame_ms_avg = s_frames ? s_frame_us_sum / 1000.0f / s_frames : 0;
    o->frame_ms_max = s_frame_us_max / 1000.0f;
    o->wait_ms_avg = s_frames ? s_wait_us_sum / 1000.0f / s_frames : 0;
    s_frames = 0;
    s_frame_us_sum = s_frame_us_max = s_wait_us_sum = 0;
    s_perf_since = now;
    lvgl_port_unlock();
}

// One clock for LVGL and the data sources: esp_timer milliseconds.
static uint32_t tick_ms(void) { return (uint32_t)(esp_timer_get_time() / 1000); }

static esp_lcd_panel_handle_t s_panel;
static esp_lcd_panel_io_handle_t s_io;

// For the scope: held while a band of the picture is on the bus (the bus
// clock runs only then), or by the scope while it wants the bus still. Only
// in use once the scope has asked; the pages never touch it (taking it for
// every band cost them 1.4 fps, measured).
static SemaphoreHandle_t s_bus;
static volatile bool s_bus_gate, s_bus_band;

bool board_lcd_quiet(bool on)
{
    if (!on) return xSemaphoreGive(s_bus) == pdTRUE;
    s_bus_gate = true;
    return xSemaphoreTake(s_bus, pdMS_TO_TICKS(20)) == pdTRUE;
}

static bool on_trans_done(esp_lcd_panel_io_handle_t io, esp_lcd_panel_io_event_data_t *d, void *ctx)
{
    BaseType_t woken = pdFALSE;
    if (s_bus_band) {
        s_bus_band = false;
        xSemaphoreGiveFromISR(s_bus, &woken);
    }
    lv_display_flush_ready(s_disp);
    return woken == pdTRUE;
}

static void flush_cb(lv_display_t *disp, const lv_area_t *area, uint8_t *px)
{
    if (s_bus_gate) {
        xSemaphoreTake(s_bus, portMAX_DELAY);
        s_bus_band = true;
    }
    esp_lcd_panel_draw_bitmap(s_panel, area->x1, area->y1, area->x2 + 1, area->y2 + 1, px);
}

static lv_indev_t *s_touch;
static lv_indev_read_cb_t s_touch_read;
static bool (*s_touch_filter)(bool pressed);

void board_set_touch_filter(bool (*filter)(bool pressed)) { s_touch_filter = filter; }

static void touch_read(lv_indev_t *indev, lv_indev_data_t *data)
{
    s_touch_read(indev, data);
    if (s_touch_filter && s_touch_filter(data->state == LV_INDEV_STATE_PRESSED)) data->state = LV_INDEV_STATE_RELEASED;
}

static void display_init(void)
{
    spi_bus_config_t bus = ST77916_PANEL_BUS_QSPI_CONFIG(PIN_LCD_SCK, PIN_LCD_D0, PIN_LCD_D1,
                                                          PIN_LCD_D2, PIN_LCD_D3,
                                                          LCD_H_RES * BAND_LINES * sizeof(uint16_t));
    ESP_ERROR_CHECK(spi_bus_initialize(SPI2_HOST, &bus, SPI_DMA_CH_AUTO));

    esp_lcd_panel_io_spi_config_t io_cfg = ST77916_PANEL_IO_QSPI_CONFIG(PIN_LCD_CS, NULL, NULL);
    io_cfg.pclk_hz = 40 * 1000 * 1000;
    io_cfg.trans_queue_depth = 4;
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)SPI2_HOST, &io_cfg, &s_io));

    st77916_vendor_config_t vendor = {
        .init_cmds = factory_lcd_init_cmds,
        .init_cmds_size = sizeof(factory_lcd_init_cmds) / sizeof(factory_lcd_init_cmds[0]),
        .flags.use_qspi_interface = 1,
    };
    esp_lcd_panel_dev_config_t panel_cfg = {
        .reset_gpio_num = PIN_LCD_RST,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = 16,
        .vendor_config = &vendor,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_st77916(s_io, &panel_cfg, &s_panel));
    ESP_ERROR_CHECK(esp_lcd_panel_reset(s_panel));
    ESP_ERROR_CHECK(esp_lcd_panel_init(s_panel));
    // The factory init table already sets the scan direction; no extra mirroring.

    const size_t fb_bytes = LCD_H_RES * BAND_LINES * sizeof(uint16_t);
    void *fb0 = heap_caps_aligned_alloc(64, fb_bytes, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    void *fb1 = heap_caps_aligned_alloc(64, fb_bytes, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    assert(fb0 && fb1);
    // Start black, before the backlight comes up.
    memset(fb0, 0, fb_bytes);
    for (int y = 0; y < LCD_V_RES; y += BAND_LINES)
        esp_lcd_panel_draw_bitmap(s_panel, 0, y, LCD_H_RES, LV_MIN(y + BAND_LINES, LCD_V_RES), fb0);
    vTaskDelay(pdMS_TO_TICKS(50));
    s_bus = xSemaphoreCreateBinary();
    xSemaphoreGive(s_bus);
    const esp_lcd_panel_io_callbacks_t cbs = {.on_color_trans_done = on_trans_done};
    esp_lcd_panel_io_register_event_callbacks(s_io, &cbs, NULL);
    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(s_panel, true));

    lvgl_port_cfg_t port_cfg = ESP_LVGL_PORT_INIT_CONFIG();
    port_cfg.task_priority = 5;
    port_cfg.task_stack = 10240;   // high-water mark leaves ~6 KB of 12 KB unused; see docs/memory.md
    port_cfg.task_affinity = 1;      // core 1; Wi-Fi and the sources live on core 0
    port_cfg.task_max_sleep_ms = 50;
    ESP_ERROR_CHECK(lvgl_port_init(&port_cfg));


    lvgl_port_lock(0);
    s_disp = lv_display_create(LCD_H_RES, LCD_V_RES);
    lv_display_set_color_format(s_disp, LV_COLOR_FORMAT_RGB565_SWAPPED);
    lv_display_set_buffers(s_disp, fb0, fb1, fb_bytes, LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(s_disp, flush_cb);
    lv_tick_set_cb(tick_ms);
    lv_display_add_event_cb(s_disp, perf_cb, LV_EVENT_RENDER_START, NULL);
    lv_display_add_event_cb(s_disp, perf_cb, LV_EVENT_FLUSH_WAIT_START, NULL);
    lv_display_add_event_cb(s_disp, perf_cb, LV_EVENT_FLUSH_WAIT_FINISH, NULL);
    lv_display_add_event_cb(s_disp, perf_cb, LV_EVENT_REFR_READY, NULL);
    s_perf_since = esp_timer_get_time();
    lvgl_port_unlock();

    esp_lcd_panel_io_handle_t tp_io;
    esp_lcd_panel_io_i2c_config_t tp_io_cfg = ESP_LCD_TOUCH_IO_I2C_CST816S_CONFIG();
    tp_io_cfg.scl_speed_hz = 400000;
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_i2c(s_i2c, &tp_io_cfg, &tp_io));
    esp_lcd_touch_config_t tp_cfg = {
        .x_max = LCD_H_RES, .y_max = LCD_V_RES,
        .rst_gpio_num = PIN_TP_RST, .int_gpio_num = PIN_TP_INT,
        .flags = {.swap_xy = 0, .mirror_x = 0, .mirror_y = 0},
    };
    esp_lcd_touch_handle_t tp;
    if (esp_lcd_touch_new_i2c_cst816s(tp_io, &tp_cfg, &tp) == ESP_OK) {
        lvgl_port_touch_cfg_t t = {.disp = s_disp, .handle = tp};
        s_touch = lvgl_port_add_touch(&t);
        if (s_touch) {
            // Read through a filter, so a touch can be swallowed (e.g. one that wakes the screen).
            lvgl_port_lock(0);
            s_touch_read = lv_indev_get_read_cb(s_touch);
            lv_indev_set_read_cb(s_touch, touch_read);
            lvgl_port_unlock();
        }
    } else {
        ESP_LOGW(TAG, "touch controller did not answer");
    }
}

// The bezel is not a quadrature encoder: each detent pulses one line low,
// GPIO2 for one direction and GPIO1 for the other. Poll both, debounce, and
// count a step when a line returns high after being low for a few samples.
#define KNOB_POLL_US 3000
#define KNOB_DEBOUNCE 2

typedef struct {
    gpio_num_t pin;
    int dir;
    uint8_t low_count;
} knob_line_t;

static knob_line_t s_lines[2] = {{PIN_KNOB_A, -1, 0}, {PIN_KNOB_B, +1, 0}};  // GPIO2 pulses on anticlockwise

static void knob_poll(void *arg)
{
    for (int i = 0; i < 2; i++) {
        knob_line_t *l = &s_lines[i];
        if (gpio_get_level(l->pin) == 0) {
            if (l->low_count < 255) l->low_count++;
        } else {
            if (l->low_count >= KNOB_DEBOUNCE && s_knob_cb) s_knob_cb(l->dir);
            l->low_count = 0;
        }
    }
}

static void knob_init(void)
{
    gpio_config_t io = {
        .pin_bit_mask = (1ULL << PIN_KNOB_A) | (1ULL << PIN_KNOB_B),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&io));
    const esp_timer_create_args_t t = {.callback = knob_poll, .name = "knob"};
    esp_timer_handle_t h;
    ESP_ERROR_CHECK(esp_timer_create(&t, &h));
    ESP_ERROR_CHECK(esp_timer_start_periodic(h, KNOB_POLL_US));
}

static void leds_init(void)
{
    led_strip_config_t strip = {
        .strip_gpio_num = PIN_RGB_DATA,
        .max_leds = RGB_LED_COUNT,
        .led_model = LED_MODEL_WS2812,
        .color_component_format = LED_STRIP_COLOR_COMPONENT_FMT_GRB,
    };
    led_strip_rmt_config_t rmt = {
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = 10 * 1000 * 1000,
    };
    ESP_ERROR_CHECK(led_strip_new_rmt_device(&strip, &rmt, &s_leds));
    led_strip_clear(s_leds);
}

void leds_set(int i, uint8_t r, uint8_t g, uint8_t b)
{
    if (s_leds && i >= 0 && i < RGB_LED_COUNT) led_strip_set_pixel(s_leds, i, r, g, b);
}

void leds_show(void)
{
    if (s_leds) led_strip_refresh(s_leds);
}

void board_init(void)
{
    gpio_hold_dis(PIN_LCD_BLK);  // held low through deep sleep
    backlight_init();
    i2c_init();
    display_init();
    knob_init();
    leds_init();
    ESP_LOGI(TAG, "board up, display: 2 x %d-line bands in internal RAM", BAND_LINES);
}

void board_panel_on(bool on)
{
    if (!s_io) return;
    esp_lcd_panel_io_tx_param(s_io, (int)(0x02u << 24 | (on ? 0x29u : 0x28u) << 8), NULL, 0);
}

void board_panel_sleep(void)
{
    if (!s_io) return;
    // Display off, then sleep in, as QSPI write-command transfers.
    esp_lcd_panel_io_tx_param(s_io, (int)(0x02u << 24 | 0x28u << 8), NULL, 0);
    vTaskDelay(pdMS_TO_TICKS(20));
    esp_lcd_panel_io_tx_param(s_io, (int)(0x02u << 24 | 0x10u << 8), NULL, 0);
    vTaskDelay(pdMS_TO_TICKS(120));
}
