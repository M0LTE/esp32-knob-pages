// Aircraft near home from adsb.oarc.uk's readsb API every 3 s, with routes
// from api.adsbdb.com by callsign (cached, at most one lookup a second, for
// aircraft inside the radius, nearest first). Does nothing until a home
// location is set. Requests carry only a generic user agent; the home
// location goes out only as the query box.
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "adsb_parse.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/task.h"
#include "model.h"
#include "net.h"
#include "ota.h"
#include "platform.h"
#include "settings.h"
#include "sources.h"

static const char *TAG = "adsb";

#define POLL_MS 3000
#define ROUTE_GAP_MS 1000
#define ROUTE_TTL_MS (6u * 3600 * 1000)
#define ROUTE_CACHE 256
#define BUF_MAX (256 * 1024)
#define MAX_FRESH 200
#define UA "desk-knob/0.1"

volatile uint32_t g_adsb_ok, g_adsb_fail, g_adsb_routes;
volatile int g_adsb_state;  // 0 off/not set, 1 failing, 2 up

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

// A kept-alive HTTPS client per host.
typedef struct {
    esp_http_client_handle_t h;
} conn_t;

static int get(conn_t *c, const char *url, body_t *b)
{
    b->n = 0;
    b->overflow = false;
    b->p[0] = 0;
    if (!c->h) {
        esp_http_client_config_t cfg = {
            .url = url,
            .timeout_ms = 8000,
            .event_handler = on_http,
            .user_data = b,
            .buffer_size = 4096,
            .user_agent = UA,
            .keep_alive_enable = true,
            .crt_bundle_attach = esp_crt_bundle_attach,
        };
        c->h = esp_http_client_init(&cfg);
        if (!c->h) return -1;
    } else {
        esp_http_client_set_url(c->h, url);
    }
    esp_http_client_set_user_data(c->h, b);
    esp_err_t err = esp_http_client_perform(c->h);
    int st = esp_http_client_get_status_code(c->h);
    if (err != ESP_OK || b->overflow) {
        esp_http_client_cleanup(c->h);
        c->h = NULL;
        return -1;
    }
    return st;
}

typedef struct {
    char key[10];            // callsign, upper case
    uint32_t at;             // model clock when looked up; 0 = empty slot
    adsb_route_t r;          // empty codes = no route
} route_entry_t;

static route_entry_t *s_cache;
static int s_cache_next;

static route_entry_t *cache_find(const char *key, uint32_t now)
{
    for (int i = 0; i < ROUTE_CACHE; i++)
        if (s_cache[i].at && !strcmp(s_cache[i].key, key)) return now - s_cache[i].at < ROUTE_TTL_MS ? &s_cache[i] : NULL;
    return NULL;
}

static void cache_put(const char *key, const adsb_route_t *r, uint32_t now)
{
    route_entry_t *e = NULL;
    for (int i = 0; i < ROUTE_CACHE && !e; i++)
        if (s_cache[i].at && !strcmp(s_cache[i].key, key)) e = &s_cache[i];
    if (!e) {
        e = &s_cache[s_cache_next];  // oldest first round the ring
        s_cache_next = (s_cache_next + 1) % ROUTE_CACHE;
    }
    snprintf(e->key, sizeof e->key, "%s", key);
    e->r = *r;
    e->at = now ? now : 1;
}

static void apply_route(aircraft_t *a, const adsb_route_t *r)
{
    memcpy(a->orig, r->orig, sizeof a->orig);
    memcpy(a->dest, r->dest, sizeof a->dest);
    memcpy(a->orig_city, r->orig_city, sizeof a->orig_city);
    memcpy(a->dest_city, r->dest_city, sizeof a->dest_city);
}

static void set_state(bool up, float lat, float lon, float radius)
{
    model_lock();
    g_model.adsb_up = up;
    g_model.home_lat = lat;
    g_model.home_lon = lon;
    g_model.adsb_radius_km = radius;
    model_unlock();
}

static void adsb_task(void *arg)
{
    body_t b = {.cap = 32 * 1024};
    b.p = heap_caps_malloc(b.cap, MALLOC_CAP_SPIRAM);
    settings_t *s = heap_caps_malloc(sizeof *s, MALLOC_CAP_SPIRAM);
    aircraft_t *cur = heap_caps_calloc(MODEL_MAX_AC, sizeof(aircraft_t), MALLOC_CAP_SPIRAM);
    aircraft_t *fresh = heap_caps_calloc(MAX_FRESH, sizeof(aircraft_t), MALLOC_CAP_SPIRAM);
    aircraft_t *scratch = heap_caps_calloc(MODEL_MAX_AC + MAX_FRESH, sizeof(aircraft_t), MALLOC_CAP_SPIRAM);
    s_cache = heap_caps_calloc(ROUTE_CACHE, sizeof(route_entry_t), MALLOC_CAP_SPIRAM);
    conn_t feed = {0}, routes = {0};
    int n_cur = 0, last_state = -1;
    uint32_t last_poll = 0, last_route = 0, gen = 0xFFFFFFFF;
    bool first = true;
    for (;;) {
        uint32_t now = model_now_ms();
        if (settings_gen() != gen) {
            gen = settings_gen();
            settings_get(s);
            n_cur = 0;  // home or radius may have changed
        }
#ifdef ADSB_TEST_HOME
        // Bench check only (-DADSB_TEST_HOME=1): a made-up point near Birmingham airport.
        if (isnan(s->home_lat)) s->home_lat = 52.45f, s->home_lon = -1.75f;
#endif
        bool home = !isnan(s->home_lat) && !isnan(s->home_lon);
        int state;
        if (!s->adsb_enabled || !home) {
            state = 0;
            set_state(false, NAN, NAN, s->adsb_radius_km);
            model_lock();
            g_model.n_ac = 0;
            model_unlock();
        } else if (!net_up() || g_ota_active) {
            state = 1;
            set_state(false, s->home_lat, s->home_lon, s->adsb_radius_km);
        } else {
            state = g_adsb_state ? g_adsb_state : 1;
            if (first || now - last_poll >= POLL_MS) {
                first = false;
                last_poll = now;
                float box[4];
                adsb_box(s->home_lat, s->home_lon, s->adsb_radius_km, box);
                char url[160];
                snprintf(url, sizeof url, "https://adsb.oarc.uk/re-api/?box=%.4f,%.4f,%.4f,%.4f&jv2", box[0], box[1],
                         box[2], box[3]);
                int st = get(&feed, url, &b);
                int n = st == 200 ? adsb_parse(b.p, fresh, MAX_FRESH, now) : -1;
                if (n >= 0) {
                    adsb_merge(cur, &n_cur, fresh, n, s->home_lat, s->home_lon, now, scratch);
                    for (int i = 0; i < n_cur; i++) {
                        char key[10];
                        route_entry_t *e;
                        if (adsb_callsign_key(cur[i].flight, key, sizeof key) && (e = cache_find(key, now)))
                            apply_route(&cur[i], &e->r);
                    }
                    model_lock();
                    memcpy(g_model.ac, cur, sizeof(aircraft_t) * n_cur);
                    g_model.n_ac = n_cur;
                    g_model.adsb_poll_ms = now;
                    model_unlock();
                    set_state(true, s->home_lat, s->home_lon, s->adsb_radius_km);
                    g_adsb_ok++;
                    state = 2;
                } else {
                    ESP_LOGW(TAG, "feed: status %d", st);
                    set_state(false, s->home_lat, s->home_lon, s->adsb_radius_km);
                    g_adsb_fail++;
                    state = 1;
                }
            }
            // One route lookup a second at most: the nearest aircraft inside the
            // radius whose callsign is not cached.
            if (state == 2 && now - last_route >= ROUTE_GAP_MS) {
                for (int i = 0; i < n_cur; i++) {
                    char key[10];
                    if (adsb_dist_km(s->home_lat, s->home_lon, cur[i].lat, cur[i].lon) > s->adsb_radius_km) continue;
                    if (!adsb_callsign_key(cur[i].flight, key, sizeof key) || cache_find(key, now)) continue;
                    last_route = now;
                    char url[96];
                    snprintf(url, sizeof url, "https://api.adsbdb.com/v0/callsign/%s", key);
                    int st = get(&routes, url, &b);
                    adsb_route_t r;
                    int res = adsbdb_parse(b.p, st, &r);
                    if (res >= 0) {
                        cache_put(key, &r, now);
                        g_adsb_routes++;
                    } else {
                        ESP_LOGW(TAG, "route %s: status %d", key, st);
                    }
                    break;
                }
            }
        }
        if (state != last_state) {
            static const char *names[] = {"off (no home location set)", "failing", "up"};
            ESP_LOGI(TAG, "%s", names[state]);
            last_state = state;
        }
        g_adsb_state = state;
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

void src_adsb_start(void)
{
    // TLS needs a deeper stack than plain HTTP.
    xTaskCreatePinnedToCoreWithCaps(adsb_task, "adsb", 12288, NULL, 4, NULL, 0, MALLOC_CAP_SPIRAM);
}
