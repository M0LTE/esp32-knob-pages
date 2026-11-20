// Polls pdn-soundmodem's Prometheus endpoint every 10 s and folds it into the
// modem fields of the model.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/idf_additions.h"
#include "esp_heap_caps.h"
#include "metrics_parse.h"
#include "model.h"
#include "net.h"
#include "platform.h"
#include "settings.h"
#include "sources.h"

static const char *TAG = "metrics";

#define POLL_MS 10000
#define BUF_MAX (256 * 1024)

volatile uint32_t g_metrics_ok, g_metrics_fail;

typedef struct {
    char *p;
    size_t n, cap;
    bool overflow;
} body_t;

static esp_err_t on_http(esp_http_client_event_t *e)
{
    body_t *b = e->user_data;
    if (e->event_id == HTTP_EVENT_ON_DATA && b) {
        if (b->n + e->data_len + 1 > b->cap) {
            size_t cap = b->cap * 2;
            while (cap < b->n + e->data_len + 1) cap *= 2;
            char *np = cap <= BUF_MAX ? heap_caps_realloc(b->p, cap, MALLOC_CAP_SPIRAM) : NULL;
            if (!np) {
                b->overflow = true;
                return ESP_OK;
            }
            b->p = np;
            b->cap = cap;
        }
        memcpy(b->p + b->n, e->data, e->data_len);
        b->n += e->data_len;
        b->p[b->n] = 0;
    }
    return ESP_OK;
}

static bool fetch(const char *url, body_t *b)
{
    b->n = 0;
    b->overflow = false;
    esp_http_client_config_t c = {
        .url = url,
        .timeout_ms = 5000,
        .event_handler = on_http,
        .user_data = b,
        .buffer_size = 2048,
        .disable_auto_redirect = false,
    };
    esp_http_client_handle_t h = esp_http_client_init(&c);
    if (!h) return false;
    esp_err_t err = esp_http_client_perform(h);
    int status = esp_http_client_get_status_code(h);
    esp_http_client_cleanup(h);
    if (err != ESP_OK || status != 200 || b->overflow) {
        ESP_LOGW(TAG, "GET %s failed: %s, status %d%s", url, esp_err_to_name(err), status, b->overflow ? ", too big" : "");
        return false;
    }
    return true;
}

static void set_down(const char *name)
{
    model_lock();
    snprintf(g_model.modem_name, sizeof g_model.modem_name, "%s", name);
    g_model.modem_up = false;
    model_unlock();
}

static void metrics_task(void *arg)
{
    body_t b = {.cap = 32 * 1024};
    b.p = heap_caps_malloc(b.cap, MALLOC_CAP_SPIRAM);
    metrics_state_t *st = heap_caps_calloc(1, sizeof *st, MALLOC_CAP_SPIRAM);
    prom_snap_t *snap = heap_caps_calloc(1, sizeof *snap, MALLOC_CAP_SPIRAM);
    char url[128] = "";
    settings_t *cur = heap_caps_malloc(sizeof *cur, MALLOC_CAP_SPIRAM);  // too big for the stack
#define s (*cur)
    TickType_t last = xTaskGetTickCount();
    for (;;) {
        settings_get(&s);
        if (strcmp(url, s.metrics_url)) {
            snprintf(url, sizeof url, "%s", s.metrics_url);
            metrics_state_init(st);  // a different modem: start the history again
        }
        if (!url[0]) {
            set_down(s.metrics_name);
        } else if (net_up()) {
            bool ok = fetch(url, &b) && prom_parse(b.p, b.n, snap) > 0;
            if (ok) {
                g_metrics_ok++;
                model_lock();
                snprintf(g_model.modem_name, sizeof g_model.modem_name, "%s", s.metrics_name);
                metrics_apply(st, snap, model_now_ms(), &g_model);
                g_model.modem_up = true;
                g_model.modem_poll_ms = model_now_ms();
                model_unlock();
            } else {
                g_metrics_fail++;
                set_down(s.metrics_name);
            }
        } else {
            set_down(s.metrics_name);
        }
        vTaskDelayUntil(&last, pdMS_TO_TICKS(POLL_MS));
    }
}

#undef s

void src_metrics_start(void)
{
    xTaskCreatePinnedToCoreWithCaps(metrics_task, "metrics", 8192, NULL, 4, NULL, 0, MALLOC_CAP_SPIRAM);
}
