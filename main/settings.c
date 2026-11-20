#include "settings.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "cJSON.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_random.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "nvs.h"
#include "nvs_flash.h"

static const char *TAG = "settings";
#define NS "desk"

const char *const energy_keys[EN_COUNT] = {
    "pv_w", "load_w", "grid_import_w", "grid_export_w", "batt_w", "soc_pct", "plan", "rates", "export_rate",
    "status", "pv_today", "import_today", "cost_today",
};

static settings_t *s_cur_p;  // in PSRAM: the settings are a few KB
#define s_cur (*s_cur_p)
static SemaphoreHandle_t s_mux;
static volatile uint32_t s_gen;
static char s_ap_pass[12];

#define CPY(dst, src) strlcpy((dst), (src), sizeof(dst))

// Compiled-in defaults: generic, nothing about any particular network. A
// build can lay its own over these (see settings_init and the README).
void settings_defaults(settings_t *s)
{
    memset(s, 0, sizeof *s);
    CPY(s->hostname, "desk-knob");
    static const set_page_t pages[] = {{"hosts", "Network"}, {"station", "Station"}, {"modem", "Modem"},
                                       {"energy", "Energy"},  {"rooms", "Rooms"},     {"sky", "Sky"}};
    s->n_pages = sizeof pages / sizeof pages[0];
    memcpy(s->pages, pages, sizeof pages);
    // Hosts, KISS, metrics, Home Assistant entities, rooms and home are left
    // empty: each source stays quiet until it is given something to talk to.
    s->kiss_port = 8105;
    s->energy_poll_s = 10;
    s->adsb_enabled = true;
    s->home_lat = s->home_lon = NAN;  // no default location
    s->adsb_radius_km = 25;
    s->sound = true;
    s->volume = 40;
    s->quiet = true;
    CPY(s->quiet_from, "22:00");
    CPY(s->quiet_to, "07:00");
}

static void jstr(cJSON *o, const char *k, char *dst, size_t n)
{
    cJSON *v = cJSON_GetObjectItemCaseSensitive(o, k);
    if (cJSON_IsString(v)) snprintf(dst, n, "%s", v->valuestring);
}

static void jint(cJSON *o, const char *k, int *dst)
{
    cJSON *v = cJSON_GetObjectItemCaseSensitive(o, k);
    if (cJSON_IsNumber(v)) *dst = v->valueint;
}

static void jbool(cJSON *o, const char *k, bool *dst)
{
    cJSON *v = cJSON_GetObjectItemCaseSensitive(o, k);
    if (cJSON_IsBool(v)) *dst = cJSON_IsTrue(v);
}

bool settings_from_json(const char *json, settings_t *s)
{
    cJSON *r = cJSON_Parse(json);
    if (!r) return false;
    cJSON *o;
    if ((o = cJSON_GetObjectItem(r, "wifi"))) {
        jstr(o, "ssid", s->ssid, sizeof s->ssid);
        jstr(o, "pass", s->pass, sizeof s->pass);
    }
    jstr(r, "hostname", s->hostname, sizeof s->hostname);
    jstr(r, "admin_pass", s->admin_pass, sizeof s->admin_pass);
    if (cJSON_IsArray(o = cJSON_GetObjectItem(r, "pages"))) {
        s->n_pages = 0;
        cJSON *e;
        cJSON_ArrayForEach(e, o) {
            if (s->n_pages >= SET_MAX_PAGES) break;
            set_page_t *p = &s->pages[s->n_pages];
            memset(p, 0, sizeof *p);
            jstr(e, "type", p->type, sizeof p->type);
            jstr(e, "title", p->title, sizeof p->title);
            if (p->type[0]) s->n_pages++;
        }
    }
    if (cJSON_IsArray(o = cJSON_GetObjectItem(r, "hosts"))) {
        s->n_hosts = 0;
        cJSON *e;
        cJSON_ArrayForEach(e, o) {
            if (s->n_hosts >= SET_MAX_HOSTS) break;
            set_host_t *h = &s->hosts[s->n_hosts];
            memset(h, 0, sizeof *h);
            jstr(e, "name", h->name, sizeof h->name);
            jstr(e, "addr", h->addr, sizeof h->addr);
            if (h->addr[0]) {
                if (!h->name[0]) CPY(h->name, h->addr);
                s->n_hosts++;
            }
        }
    }
    if ((o = cJSON_GetObjectItem(r, "kiss"))) {
        jstr(o, "host", s->kiss_host, sizeof s->kiss_host);
        jint(o, "port", &s->kiss_port);
        jstr(o, "name", s->kiss_name, sizeof s->kiss_name);
    }
    if ((o = cJSON_GetObjectItem(r, "metrics"))) {
        jstr(o, "url", s->metrics_url, sizeof s->metrics_url);
        jstr(o, "name", s->metrics_name, sizeof s->metrics_name);
    }
    if ((o = cJSON_GetObjectItem(r, "energy"))) {
        jstr(o, "url", s->energy_url, sizeof s->energy_url);
        jstr(o, "token", s->energy_token, sizeof s->energy_token);
        jint(o, "poll_s", &s->energy_poll_s);
        cJSON *e = cJSON_GetObjectItem(o, "entities");
        for (int i = 0; e && i < EN_COUNT; i++) jstr(e, energy_keys[i], s->energy_ent[i], sizeof s->energy_ent[i]);
    }
    if ((o = cJSON_GetObjectItem(r, "rooms"))) {
        jstr(o, "brain_url", s->brain_url, sizeof s->brain_url);
        cJSON *l = cJSON_GetObjectItem(o, "list");
        if (cJSON_IsArray(l)) {
            s->n_rooms = 0;
            cJSON *e;
            cJSON_ArrayForEach(e, l) {
                if (s->n_rooms < MODEL_MAX_ROOMS && cJSON_IsString(e))
                    s->n_rooms += rooms_cfg_parse(e->valuestring, &s->rooms[s->n_rooms], 1);
            }
        }
    }
    if ((o = cJSON_GetObjectItem(r, "adsb"))) {
        jbool(o, "enabled", &s->adsb_enabled);
        cJSON *v;
        if (cJSON_IsNumber(v = cJSON_GetObjectItem(o, "home_lat"))) s->home_lat = (float)v->valuedouble;
        if (cJSON_IsNumber(v = cJSON_GetObjectItem(o, "home_lon"))) s->home_lon = (float)v->valuedouble;
        if (cJSON_IsNumber(v = cJSON_GetObjectItem(o, "radius_km"))) s->adsb_radius_km = (float)v->valuedouble;
    }
    if ((o = cJSON_GetObjectItem(r, "alerts"))) {
        jbool(o, "sound", &s->sound);
        jint(o, "volume", &s->volume);
        jbool(o, "quiet", &s->quiet);
        jstr(o, "quiet_from", s->quiet_from, sizeof s->quiet_from);
        jstr(o, "quiet_to", s->quiet_to, sizeof s->quiet_to);
    }
    if (!(s->adsb_radius_km >= 2 && s->adsb_radius_km <= 100)) s->adsb_radius_km = 25;
    if (!(s->home_lat >= -90 && s->home_lat <= 90) || !(s->home_lon >= -180 && s->home_lon <= 180))
        s->home_lat = s->home_lon = NAN;
    if (s->energy_poll_s < 5) s->energy_poll_s = 5;
    if (s->energy_poll_s > 3600) s->energy_poll_s = 3600;
    if (s->volume < 0) s->volume = 0;
    if (s->volume > 100) s->volume = 100;
    cJSON_Delete(r);
    return true;
}

char *settings_to_json(const settings_t *s, bool with_secrets)
{
    cJSON *r = cJSON_CreateObject();
    cJSON *w = cJSON_AddObjectToObject(r, "wifi");
    cJSON_AddStringToObject(w, "ssid", s->ssid);
    if (with_secrets) cJSON_AddStringToObject(w, "pass", s->pass);
    cJSON_AddStringToObject(r, "hostname", s->hostname);
    if (with_secrets) cJSON_AddStringToObject(r, "admin_pass", s->admin_pass);
    cJSON *a = cJSON_AddArrayToObject(r, "pages");
    for (int i = 0; i < s->n_pages; i++) {
        cJSON *e = cJSON_CreateObject();
        cJSON_AddStringToObject(e, "type", s->pages[i].type);
        cJSON_AddStringToObject(e, "title", s->pages[i].title);
        cJSON_AddItemToArray(a, e);
    }
    a = cJSON_AddArrayToObject(r, "hosts");
    for (int i = 0; i < s->n_hosts; i++) {
        cJSON *e = cJSON_CreateObject();
        cJSON_AddStringToObject(e, "name", s->hosts[i].name);
        cJSON_AddStringToObject(e, "addr", s->hosts[i].addr);
        cJSON_AddItemToArray(a, e);
    }
    cJSON *k = cJSON_AddObjectToObject(r, "kiss");
    cJSON_AddStringToObject(k, "host", s->kiss_host);
    cJSON_AddNumberToObject(k, "port", s->kiss_port);
    cJSON_AddStringToObject(k, "name", s->kiss_name);
    cJSON *m = cJSON_AddObjectToObject(r, "metrics");
    cJSON_AddStringToObject(m, "url", s->metrics_url);
    cJSON_AddStringToObject(m, "name", s->metrics_name);
    cJSON *en = cJSON_AddObjectToObject(r, "energy");
    cJSON_AddStringToObject(en, "url", s->energy_url);
    if (with_secrets) cJSON_AddStringToObject(en, "token", s->energy_token);
    else cJSON_AddBoolToObject(en, "token_set", s->energy_token[0] != 0);
    cJSON_AddNumberToObject(en, "poll_s", s->energy_poll_s);
    cJSON *ee = cJSON_AddObjectToObject(en, "entities");
    for (int i = 0; i < EN_COUNT; i++) cJSON_AddStringToObject(ee, energy_keys[i], s->energy_ent[i]);
    cJSON *ro = cJSON_AddObjectToObject(r, "rooms");
    cJSON_AddStringToObject(ro, "brain_url", s->brain_url);
    cJSON *rl = cJSON_AddArrayToObject(ro, "list");
    for (int i = 0; i < s->n_rooms; i++) {
        char line[200];
        rooms_cfg_line(&s->rooms[i], line, sizeof line);
        cJSON_AddItemToArray(rl, cJSON_CreateString(line));
    }
    cJSON *ad = cJSON_AddObjectToObject(r, "adsb");
    cJSON_AddBoolToObject(ad, "enabled", s->adsb_enabled);
    // Home is private: stored, but not shown in settings.json.
    if (with_secrets && !isnan(s->home_lat) && !isnan(s->home_lon)) {
        cJSON_AddNumberToObject(ad, "home_lat", s->home_lat);
        cJSON_AddNumberToObject(ad, "home_lon", s->home_lon);
    } else if (!with_secrets) {
        cJSON_AddBoolToObject(ad, "home_set", !isnan(s->home_lat) && !isnan(s->home_lon));
    }
    cJSON_AddNumberToObject(ad, "radius_km", s->adsb_radius_km);
    cJSON *al = cJSON_AddObjectToObject(r, "alerts");
    cJSON_AddBoolToObject(al, "sound", s->sound);
    cJSON_AddNumberToObject(al, "volume", s->volume);
    cJSON_AddBoolToObject(al, "quiet", s->quiet);
    cJSON_AddStringToObject(al, "quiet_from", s->quiet_from);
    cJSON_AddStringToObject(al, "quiet_to", s->quiet_to);
    char *out = cJSON_PrintUnformatted(r);
    cJSON_Delete(r);
    return out;
}

void settings_get(settings_t *out)
{
    xSemaphoreTake(s_mux, portMAX_DELAY);
    *out = s_cur;
    xSemaphoreGive(s_mux);
}

uint32_t settings_gen(void) { return s_gen; }
const char *settings_ap_pass(void) { return s_ap_pass; }

bool settings_save(const settings_t *s)
{
    char *json = settings_to_json(s, true);
    if (!json) return false;
    nvs_handle_t h;
    esp_err_t err = nvs_open(NS, NVS_READWRITE, &h);
    if (err == ESP_OK) {
        err = nvs_set_str(h, "cfg", json);
        if (err == ESP_OK) err = nvs_commit(h);
        nvs_close(h);
    }
    free(json);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "save failed: %s", esp_err_to_name(err));
        return false;
    }
    xSemaphoreTake(s_mux, portMAX_DELAY);
    s_cur = *s;
    s_gen++;
    xSemaphoreGive(s_mux);
    ESP_LOGI(TAG, "saved");
    return true;
}

static void make_ap_pass(nvs_handle_t h)
{
    size_t n = sizeof s_ap_pass;
    if (nvs_get_str(h, "appass", s_ap_pass, &n) == ESP_OK && strlen(s_ap_pass) >= 8) return;
    // Easy to read off a small screen and type on a phone: no 0/o, 1/l/i.
    static const char alpha[] = "abcdefghjkmnpqrstuvwxyz23456789";
    for (int i = 0; i < 8; i++) s_ap_pass[i] = alpha[esp_random() % (sizeof alpha - 1)];
    s_ap_pass[8] = 0;
    nvs_set_str(h, "appass", s_ap_pass);
    nvs_commit(h);
}

#ifdef DESK_HAS_DEFAULTS
// The build's own defaults (idf.py -DDESK_DEFAULTS=file.json), embedded.
extern const char desk_defaults_start[] asm("_binary_desk_defaults_json_start");
#endif

// Lays the build's defaults over the compiled ones, with the same parser as
// the stored settings. Secrets never come from a build: Wi-Fi password, admin
// password, Home Assistant token and home location are set on the device.
static void apply_build_defaults(settings_t *s)
{
#ifdef DESK_HAS_DEFAULTS
    settings_t *keep = malloc(sizeof *keep);
    if (!keep) return;
    *keep = *s;
    if (!settings_from_json(desk_defaults_start, s)) {
        ESP_LOGW(TAG, "build defaults unreadable, ignored");
        *s = *keep;
        free(keep);
        return;
    }
    bool stripped = strcmp(s->pass, keep->pass) || strcmp(s->admin_pass, keep->admin_pass) ||
                    strcmp(s->energy_token, keep->energy_token) || !isnan(s->home_lat) || !isnan(s->home_lon);
    memcpy(s->pass, keep->pass, sizeof s->pass);
    memcpy(s->admin_pass, keep->admin_pass, sizeof s->admin_pass);
    memcpy(s->energy_token, keep->energy_token, sizeof s->energy_token);
    s->home_lat = keep->home_lat;
    s->home_lon = keep->home_lon;
    free(keep);
    if (stripped) ESP_LOGW(TAG, "build defaults carried secrets or a home location: ignored those");
    ESP_LOGI(TAG, "build defaults applied");
#else
    (void)s;
#endif
}

void settings_init(void)
{
    s_mux = xSemaphoreCreateMutex();
    s_cur_p = heap_caps_calloc(1, sizeof *s_cur_p, MALLOC_CAP_SPIRAM);
    assert(s_cur_p);
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS needs erasing (%s)", esp_err_to_name(err));
        nvs_flash_erase();
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    settings_defaults(&s_cur);
    apply_build_defaults(&s_cur);
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READWRITE, &h) == ESP_OK) {
        size_t n = 0;
        if (nvs_get_str(h, "cfg", NULL, &n) == ESP_OK && n > 1) {
            char *buf = malloc(n);
            if (buf && nvs_get_str(h, "cfg", buf, &n) == ESP_OK) {
                if (!settings_from_json(buf, &s_cur)) ESP_LOGW(TAG, "stored settings unreadable, using defaults");
            }
            free(buf);
        } else {
            ESP_LOGI(TAG, "no stored settings, using defaults");
        }
        make_ap_pass(h);
        nvs_close(h);
    }
}

static int hm(const char *s)
{
    int h, m;
    if (sscanf(s, "%d:%d", &h, &m) != 2) return -1;
    return h * 60 + m;
}

bool settings_sound_allowed(const settings_t *s)
{
    if (!s->sound || s->volume <= 0) return false;
    if (!s->quiet) return true;
    time_t t = time(NULL);
    struct tm lt;
    localtime_r(&t, &lt);
    if (lt.tm_year < 120) return true;  // clock not set yet
    int now = lt.tm_hour * 60 + lt.tm_min, a = hm(s->quiet_from), b = hm(s->quiet_to);
    if (a < 0 || b < 0 || a == b) return true;
    bool in = a < b ? (now >= a && now < b) : (now >= a || now < b);
    return !in;
}

int settings_alert_volume(bool any_time)
{
    xSemaphoreTake(s_mux, portMAX_DELAY);
    int v = s_cur.volume;
    bool ok = any_time || settings_sound_allowed(&s_cur);
    xSemaphoreGive(s_mux);
    if (any_time && v <= 0) v = 40;
    return ok ? v : 0;
}
