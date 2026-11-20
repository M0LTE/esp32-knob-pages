// Bench build only (idf.py -DDESK_BENCH=1 build): runs the pages without a
// network, on made-up data, so display speed can be measured on the desk.
#ifdef DESK_BENCH
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "esp_random.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "model.h"
#include "platform.h"

static float frand(void) { return (esp_random() >> 8) / 16777216.0f; }

static void bench_task(void *arg)
{
    static const char *calls[] = {"G4ABC", "M0XYZ", "G7DEF", "M7GHI-1", "2E0JKL", "MB7XYZ", "GB7ABC", "G0ABC"};
    model_lock();
    g_model.n_hosts = 10;
    for (int i = 0; i < 10; i++) {
        snprintf(g_model.hosts[i].name, sizeof g_model.hosts[i].name, "host%d", i);
        snprintf(g_model.hosts[i].addr, sizeof g_model.hosts[i].addr, "192.0.2.%d", i + 1);
    }
    g_model.kiss_up = true;
    g_model.modem_up = true;
    g_model.n_mstations = 8;
    for (int i = 0; i < 8; i++) {
        mstation_t *m = &g_model.mstations[i];
        snprintf(m->call, sizeof m->call, "%s", calls[i]);
        snprintf(m->mode, sizeof m->mode, "bpsk300-il2pc");
        m->subch = 2;
        m->frames = 10 + i * 30;
        m->snr_last = m->snr_mean = 8 + i;
        m->offset_last = i - 4;
    }
    g_model.rt_ratio = 1.0f;
    model_unlock();
    for (int k = 0;; k++) {
        uint32_t now = model_now_ms();
        model_lock();
        int i = k % 10;
        model_host_sample(&g_model.hosts[i], i == 7 && (k / 200) % 2 ? -1 : 0.4f + i * 1.5f * frand(), now);
        if (frand() < 0.1f) {
            kiss_frame_t f = {.t_ms = now, .wall = model_wall(), .tx = frand() < 0.3f, .subch = 2};
            f.len = (uint16_t)(17 + 200 * frand());
            snprintf(f.src, sizeof f.src, "%s", f.tx ? "GB7XYZ" : calls[(int)(frand() * 8)]);
            snprintf(f.dst, sizeof f.dst, "%s", f.tx ? calls[(int)(frand() * 8)] : "GB7XYZ");
            model_kiss_push(&f);
            mstation_t *m = &g_model.mstations[(int)(frand() * 8)];
            m->frames++;
            m->heard_ms = now;
        }
        g_model.busy_pct = 30 + 10 * sinf(now / 5000.0f);
        g_model.modem_poll_ms = now;
        model_unlock();
        vTaskDelay(pdMS_TO_TICKS(200));
    }
}

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"

// Memory speeds that bound a frame: PSRAM copies and fills of one screen.
static void mem_bench(void)
{
    const size_t n = 360 * 360 * 2;
    uint8_t *a = heap_caps_malloc(n, MALLOC_CAP_SPIRAM), *b = heap_caps_malloc(n, MALLOC_CAP_SPIRAM);
    uint8_t *in = heap_caps_malloc(14400, MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
    if (!a || !b || !in) return;
    int64_t t0 = esp_timer_get_time();
    for (int i = 0; i < 4; i++) memcpy(b, a, n);
    int64_t t1 = esp_timer_get_time();
    for (int i = 0; i < 4; i++) memset(a, i, n);
    int64_t t2 = esp_timer_get_time();
    for (int i = 0; i < 4; i++)
        for (size_t o = 0; o < n; o += 14400) memcpy(in, a + o, 14400);
    int64_t t3 = esp_timer_get_time();
    ESP_LOGI("bench", "one screen: psram->psram copy %.2f ms, psram fill %.2f ms, psram->internal copy %.2f ms",
             (t1 - t0) / 4000.0, (t2 - t1) / 4000.0, (t3 - t2) / 4000.0);
    free(a);
    free(b);
    free(in);
}

void bench_start(void) {
    mem_bench(); xTaskCreatePinnedToCore(bench_task, "bench", 4096, NULL, 3, NULL, 0); }
#endif
