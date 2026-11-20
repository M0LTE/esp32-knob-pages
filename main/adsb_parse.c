#include "adsb_parse.h"

#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"

#define KM_PER_DEG 111.32f
#define DEG2RAD_F(d) ((d) * 3.14159265f / 180.0f)

void adsb_box(float lat, float lon, float km, float box[4])
{
    float dlat = km / KM_PER_DEG;
    float c = cosf(DEG2RAD_F(lat));
    float dlon = km / (KM_PER_DEG * (c > 0.01f ? c : 0.01f));
    box[0] = lat - dlat;
    box[1] = lat + dlat;
    box[2] = lon - dlon;
    box[3] = lon + dlon;
}

float adsb_dist_km(float lat1, float lon1, float lat2, float lon2)
{
    float x = (lon2 - lon1) * cosf(DEG2RAD_F((lat1 + lat2) / 2));
    float y = lat2 - lat1;
    return sqrtf(x * x + y * y) * KM_PER_DEG;
}

static void str(const cJSON *o, const char *k, char *dst, size_t n)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, k);
    dst[0] = 0;
    if (!cJSON_IsString(v) || !v->valuestring) return;
    const char *s = v->valuestring;
    while (*s == ' ') s++;
    size_t len = strlen(s);
    while (len && s[len - 1] == ' ') len--;
    if (len >= n) {
        len = n - 1;
        while (len > 0 && ((unsigned char)s[len] & 0xC0) == 0x80) len--;  // whole UTF-8 characters
    }
    memcpy(dst, s, len);
    dst[len] = 0;
}

static float num(const cJSON *o, const char *k)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, k);
    return cJSON_IsNumber(v) ? (float)v->valuedouble : NAN;
}

int adsb_parse(const char *json, aircraft_t *out, int max, uint32_t now_ms)
{
    cJSON *root = cJSON_Parse(json);
    if (!root) return -1;
    const cJSON *ac = cJSON_GetObjectItemCaseSensitive(root, "ac");
    if (!cJSON_IsArray(ac)) {
        // An empty sky can come back without the array.
        bool ok = cJSON_IsObject(root);
        cJSON_Delete(root);
        return ok ? 0 : -1;
    }
    int n = 0;
    const cJSON *a;
    cJSON_ArrayForEach(a, ac) {
        if (n >= max) break;
        float lat = num(a, "lat"), lon = num(a, "lon");
        if (isnan(lat) || isnan(lon)) continue;
        aircraft_t *o = &out[n];
        memset(o, 0, sizeof *o);
        str(a, "hex", o->hex, sizeof o->hex);
        if (!o->hex[0]) continue;
        for (char *c = o->hex; *c; c++) *c = (char)tolower((unsigned char)*c);
        str(a, "flight", o->flight, sizeof o->flight);
        str(a, "r", o->reg, sizeof o->reg);
        str(a, "t", o->type, sizeof o->type);
        str(a, "desc", o->desc, sizeof o->desc);
        str(a, "ownOp", o->op, sizeof o->op);
        str(a, "category", o->cat, sizeof o->cat);
        o->lat = lat;
        o->lon = lon;
        const cJSON *alt = cJSON_GetObjectItemCaseSensitive(a, "alt_baro");
        if (cJSON_IsNumber(alt)) o->alt_ft = (float)alt->valuedouble;
        else if (cJSON_IsString(alt) && !strcmp(alt->valuestring, "ground")) o->alt_ft = 0;
        else o->alt_ft = NAN;
        o->gs_kt = num(a, "gs");
        o->track = num(a, "track");
        o->rate_fpm = num(a, "baro_rate");
        if (isnan(o->rate_fpm)) o->rate_fpm = num(a, "geom_rate");
        o->seen_ms = now_ms;
        n++;
    }
    cJSON_Delete(root);
    return n;
}

static float s_home_lat, s_home_lon;

static int by_distance(const void *a, const void *b)
{
    const aircraft_t *x = a, *y = b;
    float dx = adsb_dist_km(s_home_lat, s_home_lon, x->lat, x->lon);
    float dy = adsb_dist_km(s_home_lat, s_home_lon, y->lat, y->lon);
    return dx < dy ? -1 : dx > dy;
}

void adsb_merge(aircraft_t *cur, int *n_cur, const aircraft_t *fresh, int n_fresh, float home_lat, float home_lon,
                uint32_t now_ms, aircraft_t *scratch)
{
    int n = 0;
    // Everything from this poll, carrying over trail and route.
    for (int i = 0; i < n_fresh; i++) {
        aircraft_t *o = &scratch[n++];
        *o = fresh[i];
        const aircraft_t *old = NULL;
        for (int j = 0; j < *n_cur; j++)
            if (!strcmp(cur[j].hex, o->hex)) {
                old = &cur[j];
                break;
            }
        if (old) {
            memcpy(o->trail_lat, old->trail_lat, sizeof o->trail_lat);
            memcpy(o->trail_lon, old->trail_lon, sizeof o->trail_lon);
            o->trail_n = old->trail_n;
            if (!strcmp(o->flight, old->flight)) {
                memcpy(o->orig, old->orig, sizeof o->orig);
                memcpy(o->dest, old->dest, sizeof o->dest);
                memcpy(o->orig_city, old->orig_city, sizeof o->orig_city);
                memcpy(o->dest_city, old->dest_city, sizeof o->dest_city);
            }
        }
        bool moved = !o->trail_n || o->trail_lat[o->trail_n - 1] != o->lat || o->trail_lon[o->trail_n - 1] != o->lon;
        if (moved) {
            if (o->trail_n == AC_TRAIL) {
                memmove(o->trail_lat, o->trail_lat + 1, sizeof(float) * (AC_TRAIL - 1));
                memmove(o->trail_lon, o->trail_lon + 1, sizeof(float) * (AC_TRAIL - 1));
                o->trail_n--;
            }
            o->trail_lat[o->trail_n] = o->lat;
            o->trail_lon[o->trail_n] = o->lon;
            o->trail_n++;
        }
    }
    // Ones missing from this poll, until they have been gone a minute.
    for (int j = 0; j < *n_cur; j++) {
        bool in = false;
        for (int i = 0; i < n_fresh && !in; i++) in = !strcmp(cur[j].hex, fresh[i].hex);
        if (!in && now_ms - cur[j].seen_ms < ADSB_FORGET_MS) scratch[n++] = cur[j];
    }
    s_home_lat = home_lat;
    s_home_lon = home_lon;
    qsort(scratch, n, sizeof *scratch, by_distance);
    if (n > MODEL_MAX_AC) n = MODEL_MAX_AC;
    memcpy(cur, scratch, sizeof *cur * n);
    *n_cur = n;
}

static void city(const cJSON *ap, char *code, size_t cn, char *c, size_t n)
{
    str(ap, "icao_code", code, cn);
    str(ap, "municipality", c, n);
}

int adsbdb_parse(const char *json, int status, adsb_route_t *r)
{
    memset(r, 0, sizeof *r);
    if (status == 404) return 0;
    if (status != 200) return -1;
    cJSON *root = cJSON_Parse(json);
    if (!root) return -1;
    int res = -1;
    const cJSON *resp = cJSON_GetObjectItemCaseSensitive(root, "response");
    if (cJSON_IsString(resp)) {
        res = 0;  // "unknown callsign" and friends
    } else if (cJSON_IsObject(resp)) {
        const cJSON *fr = cJSON_GetObjectItemCaseSensitive(resp, "flightroute");
        const cJSON *o = cJSON_GetObjectItemCaseSensitive(fr, "origin");
        const cJSON *d = cJSON_GetObjectItemCaseSensitive(fr, "destination");
        if (cJSON_IsObject(o) && cJSON_IsObject(d)) {
            city(o, r->orig, sizeof r->orig, r->orig_city, sizeof r->orig_city);
            city(d, r->dest, sizeof r->dest, r->dest_city, sizeof r->dest_city);
            res = r->orig[0] || r->dest[0] ? 1 : 0;
        } else {
            res = 0;
        }
    }
    cJSON_Delete(root);
    return res;
}

bool adsb_callsign_key(const char *flight, char *out, int n)
{
    int o = 0;
    for (const char *c = flight; *c && o < n - 1; c++)
        if (isalnum((unsigned char)*c)) out[o++] = (char)toupper((unsigned char)*c);
    out[o] = 0;
    return o >= 3;
}
