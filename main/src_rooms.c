// Rooms from a climate controller's API (GET <url>/api/state, no auth,
// returning per-zone "decisions"; see test/brain_state_sample.json) every
// 10 s, and target changes asked for on the knob written to the room's target
// helper in Home Assistant (energy section's URL and token). The knob never
// drives an A/C or TRV directly: the controller reads the helper.
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
#include "rooms_parse.h"
#include "settings.h"
#include "sources.h"

static const char *TAG = "rooms";

#define POLL_MS 10000
#define LIMITS_MS (5 * 60 * 1000)
#define BUF_MAX (64 * 1024)

volatile uint32_t g_rooms_ok, g_rooms_fail;
volatile int g_rooms_n;

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

// One request. auth may be NULL; post_body NULL means GET. Returns the HTTP status (or -1).
static int request(const char *url, const char *auth, const char *post_body, body_t *b)
{
    b->n = 0;
    b->overflow = false;
    b->p[0] = 0;
    esp_http_client_config_t c = {
        .url = url,
        .timeout_ms = 5000,
        .event_handler = on_http,
        .user_data = b,
        .buffer_size = 2048,
        .method = post_body ? HTTP_METHOD_POST : HTTP_METHOD_GET,
    };
    esp_http_client_handle_t h = esp_http_client_init(&c);
    if (!h) return -1;
    if (auth) esp_http_client_set_header(h, "Authorization", auth);
    if (post_body) {
        esp_http_client_set_header(h, "Content-Type", "application/json");
        esp_http_client_set_post_field(h, post_body, strlen(post_body));
    }
    esp_err_t err = esp_http_client_perform(h);
    int status = esp_http_client_get_status_code(h);
    esp_http_client_cleanup(h);
    if (err != ESP_OK || b->overflow) return -1;
    return status;
}

static int minute_now(void)
{
    time_t t = time(NULL);
    struct tm lt;
    localtime_r(&t, &lt);
    return lt.tm_hour * 60 + lt.tm_min;
}

static bool clock_set(void) { return time(NULL) > 1700000000; }

typedef struct {
    float min, max, step;
} limits_t;

// limits for each room's day and night helper
static limits_t s_day[MODEL_MAX_ROOMS], s_night[MODEL_MAX_ROOMS];

static void read_limits(const settings_t *s, const char *auth, body_t *b)
{
    for (int i = 0; i < s->n_rooms; i++) {
        for (int k = 0; k < 2; k++) {
            const char *ent = k ? s->rooms[i].night : s->rooms[i].helper;
            limits_t *l = k ? &s_night[i] : &s_day[i];
            l->min = 16, l->max = 28, l->step = 0.5f;
            if (!ent[0]) continue;
            char url[200];
            snprintf(url, sizeof url, "%s/api/states/%s", s->energy_url, ent);
            int st = request(url, auth, NULL, b);
            if (st != 200) {
                ESP_LOGW(TAG, "limits of %s: status %d", ent, st);
                continue;
            }
            float mn = ha_attr_num(b->p, "min"), mx = ha_attr_num(b->p, "max"), sp = ha_attr_num(b->p, "step");
            if (!isnan(mn)) l->min = mn;
            if (!isnan(mx)) l->max = mx;
            if (!isnan(sp) && sp > 0) l->step = sp;
        }
    }
}

static const limits_t *limits_now(const settings_t *s, int i, int minute)
{
    return rooms_helper_at(&s->rooms[i], minute) == s->rooms[i].night ? &s_night[i] : &s_day[i];
}

static void handle_request(const settings_t *s, const char *auth, body_t *b)
{
    uint32_t seq;
    char room[16];
    float value;
    model_lock();
    seq = g_model.room_set.seq;
    bool pending = seq != g_model.room_set.done_seq;
    snprintf(room, sizeof room, "%s", g_model.room_set.room);
    value = g_model.room_set.value;
    model_unlock();
    if (!pending) return;

    int8_t result = -1;
    int i;
    for (i = 0; i < s->n_rooms; i++)
        if (!strcmp(s->rooms[i].id, room)) break;
    if (i == s->n_rooms) {
        ESP_LOGW(TAG, "set %s: no such room", room);
    } else if (!auth) {
        ESP_LOGW(TAG, "set %s: no Home Assistant token", room);
    } else if (!clock_set() && s->rooms[i].night[0]) {
        ESP_LOGW(TAG, "set %s: clock not set, cannot tell day from night", room);
    } else {
        int m = minute_now();
        const char *ent = rooms_helper_at(&s->rooms[i], m);
        const limits_t *l = limits_now(s, i, m);
        float v = roundf(value / l->step) * l->step;
        if (v < l->min) v = l->min;
        if (v > l->max) v = l->max;
        char url[200], body[160];
        snprintf(url, sizeof url, "%s/api/services/input_number/set_value", s->energy_url);
        snprintf(body, sizeof body, "{\"entity_id\":\"%s\",\"value\":%.2f}", ent, v);
        int st = request(url, auth, body, b);
        result = st == 200 ? 1 : -1;
        ESP_LOGI(TAG, "set %s to %.2f: %s (status %d)", ent, v, result > 0 ? "done" : "failed", st);
    }
    model_lock();
    g_model.room_set.result = result;
    g_model.room_set.done_seq = seq;
    model_unlock();
}

static void rooms_task(void *arg)
{
    body_t b = {.cap = 16 * 1024};
    b.p = heap_caps_malloc(b.cap, MALLOC_CAP_SPIRAM);
    settings_t *s = heap_caps_malloc(sizeof *s, MALLOC_CAP_SPIRAM);
    room_t *rooms = heap_caps_calloc(MODEL_MAX_ROOMS, sizeof(room_t), MALLOC_CAP_SPIRAM);
    static char auth[320];
    uint32_t last_poll = 0, last_limits = 0, gen = 0xFFFFFFFF;
    bool limits_read = false, first = true;
    for (;;) {
        uint32_t now = model_now_ms();
        if (settings_gen() != gen) {
            gen = settings_gen();
            settings_get(s);
            limits_read = false;
            for (int i = 0; i < MODEL_MAX_ROOMS; i++) s_day[i] = s_night[i] = (limits_t){16, 28, 0.5f};
        }
        const char *a = NULL;
        if (s->energy_token[0] && s->energy_url[0]) {
            snprintf(auth, sizeof auth, "Bearer %s", s->energy_token);
            a = auth;
        }
        if (!s->brain_url[0] || !s->n_rooms) {
            // Not configured: quiet.
            handle_request(s, a, &b);
            model_lock();
            g_model.climate_up = false;
            g_model.n_rooms = 0;
            model_unlock();
        } else if (net_up()) {
            handle_request(s, a, &b);
            if (a && (!limits_read || now - last_limits > LIMITS_MS)) {
                read_limits(s, a, &b);
                limits_read = true;
                last_limits = now;
            }
            if (first || now - last_poll >= POLL_MS) {
                first = false;
                last_poll = now;
                char url[160];
                snprintf(url, sizeof url, "%s/api/state", s->brain_url);
                int st = request(url, NULL, NULL, &b);
                int n = s->n_rooms;
                model_lock();
                memcpy(rooms, g_model.rooms, sizeof(room_t) * MODEL_MAX_ROOMS);  // keeps t_* across polls
                model_unlock();
                int found = st == 200 ? rooms_parse_state(b.p, s->rooms, n, rooms) : -1;
                int m = minute_now();
                for (int i = 0; i < n; i++) {
                    const limits_t *l = limits_now(s, i, m);
                    rooms[i].t_min = l->min, rooms[i].t_max = l->max, rooms[i].t_step = l->step;
                }
                model_lock();
                if (found >= 0) {
                    memcpy(g_model.rooms, rooms, sizeof(room_t) * MODEL_MAX_ROOMS);
                    g_model.n_rooms = n;
                    g_model.climate_up = true;
                    g_model.climate_poll_ms = model_now_ms();
                } else {
                    g_model.climate_up = false;
                }
                model_unlock();
                if (found >= 0) {
                    if (!g_rooms_ok) ESP_LOGI(TAG, "%d of %d rooms from the climate controller", found, n);
                    g_rooms_ok++;
                    g_rooms_n = found;
                } else {
                    if (!g_rooms_fail) ESP_LOGW(TAG, "GET %s: status %d", url, st);
                    g_rooms_fail++;
                }
            }
        } else {
            model_lock();
            g_model.climate_up = false;
            model_unlock();
        }
        vTaskDelay(pdMS_TO_TICKS(100));  // a knob request goes out within 0.1 s
    }
}

void src_rooms_start(void)
{
    xTaskCreatePinnedToCoreWithCaps(rooms_task, "rooms", 8192, NULL, 4, NULL, 0, MALLOC_CAP_SPIRAM);
}
