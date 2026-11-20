#include "rooms_parse.h"

#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "cJSON.h"

static int hhmm(const char *s)
{
    int h, m;
    if (sscanf(s, "%d:%d", &h, &m) != 2 || h < 0 || h > 23 || m < 0 || m > 59) return -1;
    return h * 60 + m;
}

int rooms_cfg_parse(const char *text, room_cfg_t *out, int max)
{
    int n = 0;
    const char *p = text;
    while (*p && n < max) {
        const char *eol = strpbrk(p, "\r\n");
        size_t len = eol ? (size_t)(eol - p) : strlen(p);
        char line[256];
        if (len >= sizeof line) len = sizeof line - 1;
        memcpy(line, p, len);
        line[len] = 0;
        p = eol ? eol + 1 : p + len;

        char *tok[5];
        int k = 0;
        for (char *t = strtok(line, " \t"); t && k < 5; t = strtok(NULL, " \t")) tok[k++] = t;
        if (k != 3 && k != 5) continue;
        room_cfg_t *r = &out[n];
        memset(r, 0, sizeof *r);
        r->night_from = r->night_to = -1;
        snprintf(r->id, sizeof r->id, "%s", tok[0]);
        snprintf(r->name, sizeof r->name, "%s", tok[1]);
        for (char *c = r->name; *c; c++)
            if (*c == '_') *c = ' ';
        snprintf(r->helper, sizeof r->helper, "%s", tok[2]);
        if (k == 5) {
            char *dash = strchr(tok[4], '-');
            if (!dash) continue;
            *dash = 0;
            int a = hhmm(tok[4]), b = hhmm(dash + 1);
            if (a < 0 || b < 0 || a == b) continue;
            snprintf(r->night, sizeof r->night, "%s", tok[3]);
            r->night_from = (int16_t)a;
            r->night_to = (int16_t)b;
        }
        n++;
    }
    return n;
}

void rooms_cfg_line(const room_cfg_t *r, char *out, int n)
{
    char name[20];
    snprintf(name, sizeof name, "%s", r->name);
    for (char *c = name; *c; c++)
        if (*c == ' ') *c = '_';
    int o = snprintf(out, n, "%s %s %s", r->id, name, r->helper);
    if (r->night[0] && r->night_from >= 0 && o < n)
        snprintf(out + o, n - o, " %s %02d:%02d-%02d:%02d", r->night, r->night_from / 60, r->night_from % 60,
                 r->night_to / 60, r->night_to % 60);
}

const char *rooms_helper_at(const room_cfg_t *r, int m)
{
    if (!r->night[0] || r->night_from < 0) return r->helper;
    int a = r->night_from, b = r->night_to;
    bool in = a < b ? (m >= a && m < b) : (m >= a || m < b);
    return in ? r->night : r->helper;
}

static float num_or_nan(const cJSON *o, const char *key)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, key);
    return cJSON_IsNumber(v) ? (float)v->valuedouble : NAN;
}

static void copy_str(char *dst, size_t n, const cJSON *v)
{
    if (cJSON_IsString(v) && v->valuestring) {
        // Truncate on a character boundary (the text is UTF-8).
        size_t len = strlen(v->valuestring);
        if (len >= n) {
            len = n - 1;
            while (len > 0 && ((unsigned char)v->valuestring[len] & 0xC0) == 0x80) len--;
        }
        memcpy(dst, v->valuestring, len);
        dst[len] = 0;
    } else {
        dst[0] = 0;
    }
}

int rooms_parse_state(const char *json, const room_cfg_t *cfg, int n, room_t *out)
{
    for (int i = 0; i < n; i++) {
        room_t *r = &out[i];
        float tmin = r->t_min, tmax = r->t_max, tstep = r->t_step;
        memset(r, 0, sizeof *r);
        snprintf(r->id, sizeof r->id, "%s", cfg[i].id);
        snprintf(r->name, sizeof r->name, "%s", cfg[i].name);
        r->target = r->temp = r->humidity = r->ac_setpoint = NAN;
        r->valves = -1;
        r->t_min = tmin, r->t_max = tmax, r->t_step = tstep;
    }
    cJSON *root = cJSON_Parse(json);
    if (!root) return -1;
    const cJSON *dec = cJSON_GetObjectItemCaseSensitive(root, "decisions");
    if (!cJSON_IsArray(dec)) {
        cJSON_Delete(root);
        return -1;
    }
    int found = 0;
    const cJSON *d;
    cJSON_ArrayForEach(d, dec) {
        const cJSON *zid = cJSON_GetObjectItemCaseSensitive(d, "zone_id");
        if (!cJSON_IsString(zid)) continue;
        int i;
        for (i = 0; i < n; i++)
            if (!strcmp(cfg[i].id, zid->valuestring)) break;
        if (i == n) continue;  // heat_plant and anything not configured
        room_t *r = &out[i];
        found++;
        copy_str(r->status, sizeof r->status, cJSON_GetObjectItemCaseSensitive(d, "status"));
        r->target = num_or_nan(d, "target");
        r->temp = num_or_nan(d, "pv");
        r->humidity = num_or_nan(d, "humidity");
        const cJSON *cur = cJSON_GetObjectItemCaseSensitive(d, "current");
        if (cJSON_IsObject(cur)) {
            copy_str(r->ac_mode, sizeof r->ac_mode, cJSON_GetObjectItemCaseSensitive(cur, "mode"));
            r->ac_setpoint = num_or_nan(cur, "setpoint");
        }
        if (!r->ac_mode[0] || !strcmp(r->ac_mode, "off")) r->ac_setpoint = NAN;
        const cJSON *h = cJSON_GetObjectItemCaseSensitive(d, "heating");
        if (cJSON_IsObject(h)) {
            r->valves = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(h, "valves_open")) ? 1 : 0;
            r->heat_demand = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(h, "demanding"));
        }
        const cJSON *why = cJSON_GetObjectItemCaseSensitive(d, "why");
        if (cJSON_IsArray(why)) copy_str(r->why, sizeof r->why, cJSON_GetArrayItem(why, 0));
    }
    cJSON_Delete(root);
    return found;
}
