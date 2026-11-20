// Home energy from Home Assistant's REST API: inverter power and energy
// sensors and Predbat's plan and rates (entity ids from the settings), polled
// every few seconds into the model. Entities left empty stay unknown.
// Without a token it stays quietly "not configured". The token is never
// logged.
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "energy_parse.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/task.h"
#include "model.h"
#include "net.h"
#include "platform.h"
#include "settings.h"
#include "sources.h"

static const char *TAG = "energy";

#define BUF_MAX (256 * 1024)
#define MAX_POINTS 1200   // Predbat's 5-minute plan is about 600 entries

volatile uint32_t g_energy_ok, g_energy_fail;
volatile int g_energy_state;  // 0 not configured, 1 failing, 2 up

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

// One persistent client per poll round (keep-alive to HA).
static esp_http_client_handle_t s_client;
static char s_auth[320];

static bool get_entity(const settings_t *s, const char *entity, body_t *b)
{
    char url[200];
    snprintf(url, sizeof url, "%s/api/states/%s", s->energy_url, entity);
    b->n = 0;
    b->overflow = false;
    b->p[0] = 0;
    if (!s_client) {
        esp_http_client_config_t c = {
            .url = url,
            .timeout_ms = 5000,
            .event_handler = on_http,
            .user_data = b,
            .buffer_size = 2048,
            .keep_alive_enable = true,
        };
        s_client = esp_http_client_init(&c);
        if (!s_client) return false;
    } else {
        esp_http_client_set_url(s_client, url);
    }
    esp_http_client_set_header(s_client, "Authorization", s_auth);
    esp_err_t err = esp_http_client_perform(s_client);
    int status = esp_http_client_get_status_code(s_client);
    if (err != ESP_OK || status != 200 || b->overflow) {
        // Say which entity and what came back, never the token.
        ESP_LOGW(TAG, "GET %s: %s, status %d%s", entity, esp_err_to_name(err), status, b->overflow ? ", too big" : "");
        esp_http_client_cleanup(s_client);
        s_client = NULL;
        return false;
    }
    return true;
}

typedef struct {
    float v[EN_COUNT];
    char status[24];
    float soc_kwh, soc_max_kwh;
    float rate_next;
    uint32_t rate_next_wall, start;
    float plan_soc[ENERGY_SLOTS], plan_rate[ENERGY_SLOTS];
} snap_t;

static bool poll_once(const settings_t *s, body_t *b, snap_t *o, ha_point_t *pts)
{
    time_t now = time(NULL);
    if (now < 1700000000) return false;  // no clock yet: cannot place the plan
    o->start = (uint32_t)(now - now % 900);
    for (int i = 0; i < EN_COUNT; i++) {
        if (!s->energy_ent[i][0]) {
            // Not configured: that reading stays unknown.
            o->v[i] = NAN;
            if (i == EN_STATUS) o->status[0] = 0;
            if (i == EN_PLAN) {
                o->soc_kwh = o->soc_max_kwh = NAN;
                for (int k = 0; k < ENERGY_SLOTS; k++) o->plan_soc[k] = NAN;
            }
            if (i == EN_RATES) {
                o->rate_next = NAN;
                o->rate_next_wall = 0;
                for (int k = 0; k < ENERGY_SLOTS; k++) o->plan_rate[k] = NAN;
            }
            continue;
        }
        if (!get_entity(s, s->energy_ent[i], b)) return false;
        o->v[i] = ha_state_num(b->p);
        if (i == EN_STATUS) {
            if (!ha_state_str(b->p, o->status, sizeof o->status)) o->status[0] = 0;
        } else if (i == EN_PLAN) {
            o->soc_kwh = ha_attr_num(b->p, "soc_now");
            o->soc_max_kwh = ha_attr_num(b->p, "soc_max");
            int n = ha_results(b->p, pts, MAX_POINTS);
            ha_resample(pts, n, o->start, 900, ENERGY_SLOTS, o->plan_soc);
        } else if (i == EN_RATES) {
            int n = ha_results(b->p, pts, MAX_POINTS);
            ha_resample(pts, n, o->start, 900, ENERGY_SLOTS, o->plan_rate);
            o->rate_next = NAN;
            o->rate_next_wall = 0;
            ha_next_change(pts, n, (uint32_t)now, o->v[EN_RATES], &o->rate_next, &o->rate_next_wall);
        }
    }
    return true;
}

static void set_down(void)
{
    model_lock();
    g_model.energy_up = false;
    model_unlock();
}

static void energy_task(void *arg)
{
    body_t b = {.cap = 32 * 1024};
    b.p = heap_caps_malloc(b.cap, MALLOC_CAP_SPIRAM);
    snap_t *o = heap_caps_calloc(1, sizeof *o, MALLOC_CAP_SPIRAM);
    ha_point_t *pts = heap_caps_malloc(sizeof(ha_point_t) * MAX_POINTS, MALLOC_CAP_SPIRAM);
    settings_t *s = heap_caps_malloc(sizeof *s, MALLOC_CAP_SPIRAM);
    int last_state = -1;
    for (;;) {
        settings_get(s);
        int state;
        bool any = false;
        for (int i = 0; i < EN_COUNT; i++) any |= s->energy_ent[i][0] != 0;
        if (!s->energy_token[0] || !s->energy_url[0] || !any) {
            state = 0;
            set_down();
        } else if (!net_up()) {
            state = 1;
            set_down();
        } else {
            snprintf(s_auth, sizeof s_auth, "Bearer %s", s->energy_token);
            bool ok = poll_once(s, &b, o, pts);
            if (ok) {
                g_energy_ok++;
                state = 2;
                model_lock();
                g_model.energy_up = true;
                g_model.energy_poll_ms = model_now_ms();
                g_model.pv_w = o->v[EN_PV];
                g_model.load_w = o->v[EN_LOAD];
                g_model.grid_w = o->v[EN_GRID_IN] - o->v[EN_GRID_OUT];  // NAN if either is
                // Battery power is taken as + when charging (as the Solax Modbus
                // integration's "battery power charge" reports it).
                g_model.batt_w = o->v[EN_BATT];
                g_model.soc_pct = o->v[EN_SOC_PCT];
                g_model.soc_kwh = o->soc_kwh;
                g_model.soc_max_kwh = o->soc_max_kwh;
                g_model.rate_p = o->v[EN_RATES];
                g_model.rate_next_p = o->rate_next;
                g_model.rate_next_wall = o->rate_next_wall;
                g_model.export_rate_p = o->v[EN_RATES_EXPORT];
                snprintf(g_model.plan_status, sizeof g_model.plan_status, "%s", o->status);
                g_model.pv_today_kwh = o->v[EN_PV_TODAY];
                g_model.import_today_kwh = o->v[EN_IMPORT_TODAY];
                g_model.cost_today_p = o->v[EN_COST_TODAY];
                g_model.plan_start_wall = o->start;
                memcpy(g_model.plan_soc_kwh, o->plan_soc, sizeof o->plan_soc);
                memcpy(g_model.plan_rate_p, o->plan_rate, sizeof o->plan_rate);
                model_unlock();
            } else {
                g_energy_fail++;
                state = 1;
                set_down();
            }
        }
        if (state != last_state) {
            static const char *names[] = {"not configured (needs URL, token and entities)", "failing", "up"};
            ESP_LOGI(TAG, "%s", names[state]);
            last_state = state;
        }
        g_energy_state = state;
        int wait = s->energy_poll_s > 0 ? s->energy_poll_s : 10;
        uint32_t gen = settings_gen();
        for (int i = 0; i < wait * 10 && settings_gen() == gen; i++) vTaskDelay(pdMS_TO_TICKS(100));
    }
}

void src_energy_start(void)
{
    xTaskCreatePinnedToCoreWithCaps(energy_task, "energy", 8192, NULL, 4, NULL, 0, MALLOC_CAP_SPIRAM);
}
