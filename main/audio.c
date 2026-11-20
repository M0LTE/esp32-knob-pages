// Speaker out: PCM5100A I2S DAC and NS4150B amp. From the hello bring-up app,
// without the microphone.
#include "audio.h"

#include <math.h>
#include <string.h>

#include "board.h"
#include "driver/i2s_std.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "freertos/idf_additions.h"
#include "esp_heap_caps.h"

#define OUT_RATE 32000

static i2s_chan_handle_t s_tx;
static QueueHandle_t s_q;

typedef struct {
    float f1, f2, ms, vol;
} tone_t;

static void out_task(void *arg)
{
    static int16_t buf[256 * 2];
    tone_t t;
    for (;;) {
        if (xQueueReceive(s_q, &t, portMAX_DELAY) != pdTRUE) continue;
        gpio_set_level(PIN_PA_CTRL, 1);
        int total = (int)(OUT_RATE * t.ms / 1000);
        float p1 = 0, p2 = 0;
        for (int n = 0; n < total;) {
            int chunk = total - n < 256 ? total - n : 256;
            for (int i = 0; i < chunk; i++, n++) {
                float env = expf(-4.0f * n / total);
                float att = n < 64 ? n / 64.0f : 1.0f;
                float s = 0.6f * sinf(p1) + 0.4f * sinf(p2);
                p1 += 2 * (float)M_PI * t.f1 / OUT_RATE;
                p2 += 2 * (float)M_PI * t.f2 / OUT_RATE;
                int16_t v = (int16_t)(s * env * att * t.vol * 32767);
                buf[2 * i] = buf[2 * i + 1] = v;
            }
            size_t w;
            i2s_channel_write(s_tx, buf, chunk * 4, &w, portMAX_DELAY);
        }
        // Flush with silence so the amp does not pop on mute.
        memset(buf, 0, sizeof(buf));
        size_t w;
        for (int i = 0; i < 8; i++) i2s_channel_write(s_tx, buf, sizeof(buf), &w, portMAX_DELAY);
        if (uxQueueMessagesWaiting(s_q) == 0) gpio_set_level(PIN_PA_CTRL, 0);
    }
}

void audio_play(float f1, float f2, float ms, float vol)
{
    if (!s_q) return;
    tone_t t = {f1, f2, ms, vol};
    xQueueSend(s_q, &t, 0);
}

void audio_init(void)
{
    gpio_config_t pa = {.pin_bit_mask = 1ULL << PIN_PA_CTRL, .mode = GPIO_MODE_OUTPUT};
    gpio_config(&pa);
    gpio_set_level(PIN_PA_CTRL, 0);

    i2s_chan_config_t tx_chan = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_1, I2S_ROLE_MASTER);
    tx_chan.auto_clear = true;
    ESP_ERROR_CHECK(i2s_new_channel(&tx_chan, &s_tx, NULL));
    i2s_std_config_t std = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(OUT_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED, .bclk = PIN_I2S_BCK, .ws = PIN_I2S_WS,
            .dout = PIN_I2S_DO, .din = I2S_GPIO_UNUSED,
        },
    };
    ESP_ERROR_CHECK(i2s_channel_init_std_mode(s_tx, &std));
    ESP_ERROR_CHECK(i2s_channel_enable(s_tx));
    s_q = xQueueCreate(8, sizeof(tone_t));
    xTaskCreatePinnedToCoreWithCaps(out_task, "spk", 4096, NULL, 6, NULL, 0, MALLOC_CAP_SPIRAM);
}
