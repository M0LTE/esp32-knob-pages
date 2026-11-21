// Speaker out: PCM5100A I2S DAC and NS4150B amp, one 48 kHz mixer task for
// the alert chimes and received audio. The amp is on only while something
// plays.
#include "audio.h"

#include <math.h>
#include <string.h>

#include "board.h"
#include "driver/i2s_std.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "rx_audio.h"

#define OUT_RATE 48000
#define CHUNK 240                    // 5 ms per write
#define FADE_STEP (1.0f / (0.02f * OUT_RATE))  // 20 ms fades
#define RING_SAMPLES (48000 * 2)     // 2 s at the highest stream rate
#define TARGET_MS 300

static i2s_chan_handle_t s_tx;
static QueueHandle_t s_q;

typedef struct {
    float f1, f2, ms, vol;
} tone_t;

static rxa_t s_rx;
static volatile bool s_want;          // a stream is wanted (start called, stop not)
static volatile uint32_t s_rate_req;  // rate of the stream asked for
static volatile uint32_t s_gen_req;   // bumped by each start: the consumer resets
static volatile float s_vol_gain;
static volatile bool s_playing;
static volatile float s_dbfs = NAN;

void audio_stream_volume(int v)
{
    if (v <= 0) s_vol_gain = 0;
    else s_vol_gain = powf(10.0f, (v > 100 ? 0 : (v - 100) * 0.5f) / 20.0f);  // 100 = 0 dB, 1 = -49.5 dB
}

void audio_stream_start(uint32_t in_rate)
{
    s_rate_req = in_rate;
    s_gen_req++;
    s_dbfs = NAN;
    s_want = true;
}

void audio_stream_stop(void)
{
    s_want = false;
    s_dbfs = NAN;
}

void audio_stream_push(const uint8_t *le, uint32_t n)
{
    if (!s_want || !n) return;
    rxa_push_le(&s_rx, le, n);
    // Level for the meter: RMS of this block, smoothed.
    double acc = 0;
    for (uint32_t i = 0; i < n; i++) {
        int16_t v = (int16_t)(le[2 * i] | le[2 * i + 1] << 8);
        acc += (double)v * v;
    }
    float db = 10 * log10f((float)(acc / n) / (32768.0f * 32768.0f) + 1e-12f);
    s_dbfs = isnan(s_dbfs) ? db : s_dbfs + 0.3f * (db - s_dbfs);
}

bool audio_stream_playing(void) { return s_playing; }
float audio_stream_dbfs(void) { return s_want ? s_dbfs : NAN; }

void audio_stream_stats(uint32_t *fill_ms, uint32_t *underruns, uint32_t *trims, float *corr)
{
    uint32_t rate = s_rx.in_rate ? s_rx.in_rate : 48000;
    *fill_ms = rxa_fill(&s_rx) * 1000 / rate;
    *underruns = s_rx.underruns;
    *trims = s_rx.trims;
    *corr = s_rx.corr;
}

static void out_task(void *arg)
{
    static int16_t mono[CHUNK], buf[CHUNK * 2];
    tone_t t;
    bool tone = false, amp = false;
    int tone_n = 0, tone_total = 0;
    float p1 = 0, p2 = 0, fade = 0;
    uint32_t gen = 0;
    int idle_chunks = 0;
    for (;;) {
        bool want = s_want;
        if (want && gen != s_gen_req) {
            gen = s_gen_req;
            rxa_reset(&s_rx, s_rate_req, OUT_RATE, s_rate_req * TARGET_MS / 1000);
            fade = 0;
        }
        if (!tone && xQueueReceive(s_q, &t, 0) == pdTRUE) {
            tone = true;
            tone_n = 0;
            tone_total = (int)(OUT_RATE * t.ms / 1000);
            p1 = p2 = 0;
        }
        bool stream = want || fade > 0;
        if (!stream && !tone) {
            // Nothing to play: a little silence so the amp does not pop, then off.
            if (amp && ++idle_chunks > 8) {
                gpio_set_level(PIN_PA_CTRL, 0);
                amp = false;
            }
            if (!amp) {
                s_playing = false;
                if (xQueueReceive(s_q, &t, pdMS_TO_TICKS(20)) == pdTRUE) {
                    tone = true;
                    tone_n = 0;
                    tone_total = (int)(OUT_RATE * t.ms / 1000);
                    p1 = p2 = 0;
                }
                continue;
            }
        } else {
            idle_chunks = 0;
            if (!amp) {
                gpio_set_level(PIN_PA_CTRL, 1);
                amp = true;
            }
        }

        int real = 0;
        if (stream) real = rxa_pull(&s_rx, mono, CHUNK);
        else memset(mono, 0, sizeof mono);
        s_playing = want && s_rx.playing && real > 0;
        float vol = s_vol_gain, duck = tone ? 0.4f : 1.0f;
        for (int i = 0; i < CHUNK; i++) {
            // Fade in once audio is flowing, out when stopped or starved.
            float goal = want && s_rx.playing ? 1.0f : 0.0f;
            if (fade < goal) fade = fade + FADE_STEP > goal ? goal : fade + FADE_STEP;
            else if (fade > goal) fade = fade - FADE_STEP < goal ? goal : fade - FADE_STEP;
            float s = mono[i] * fade * vol * duck;
            if (tone) {
                if (tone_n < tone_total) {
                    float env = expf(-4.0f * tone_n / tone_total);
                    float att = tone_n < 64 ? tone_n / 64.0f : 1.0f;
                    s += (0.6f * sinf(p1) + 0.4f * sinf(p2)) * env * att * t.vol * 32767;
                    p1 += 2 * (float)M_PI * t.f1 / OUT_RATE;
                    p2 += 2 * (float)M_PI * t.f2 / OUT_RATE;
                    if (p1 > 2 * (float)M_PI) p1 -= 2 * (float)M_PI;
                    if (p2 > 2 * (float)M_PI) p2 -= 2 * (float)M_PI;
                    tone_n++;
                } else if (xQueueReceive(s_q, &t, 0) == pdTRUE) {
                    tone_n = 0;  // the next queued chime follows straight on
                    tone_total = (int)(OUT_RATE * t.ms / 1000);
                    p1 = p2 = 0;
                } else {
                    tone = false;
                }
            }
            if (s > 32767) s = 32767;
            if (s < -32768) s = -32768;
            buf[2 * i] = buf[2 * i + 1] = (int16_t)s;
        }
        size_t w;
        i2s_channel_write(s_tx, buf, sizeof buf, &w, portMAX_DELAY);
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

    int16_t *ring = heap_caps_malloc(RING_SAMPLES * sizeof(int16_t), MALLOC_CAP_SPIRAM);
    rxa_init(&s_rx, ring, RING_SAMPLES);
    audio_stream_volume(50);

    i2s_chan_config_t tx_chan = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_1, I2S_ROLE_MASTER);
    tx_chan.auto_clear = true;
    tx_chan.dma_desc_num = 4;
    tx_chan.dma_frame_num = CHUNK;
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
