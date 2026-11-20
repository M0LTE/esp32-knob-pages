#include "energy_parse.h"

#include <ctype.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

static int64_t days_from_civil(int y, int m, int d)
{
    y -= m <= 2;
    int64_t era = (y >= 0 ? y : y - 399) / 400;
    int yoe = (int)(y - era * 400);
    int doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    int doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}

static int num(const char *s, size_t n, size_t *i, int digits)
{
    int v = 0;
    for (int k = 0; k < digits; k++, (*i)++) {
        if (*i >= n || !isdigit((unsigned char)s[*i])) return -1;
        v = v * 10 + (s[*i] - '0');
    }
    return v;
}

uint32_t ha_parse_time(const char *s, size_t n)
{
    size_t i = 0;
    int Y = num(s, n, &i, 4);
    if (Y < 0 || i >= n || s[i++] != '-') return 0;
    int M = num(s, n, &i, 2);
    if (M < 1 || i >= n || s[i++] != '-') return 0;
    int D = num(s, n, &i, 2);
    if (D < 1 || i >= n || (s[i] != 'T' && s[i] != ' ')) return 0;
    i++;
    int h = num(s, n, &i, 2);
    if (h < 0 || i >= n || s[i++] != ':') return 0;
    int mi = num(s, n, &i, 2);
    if (mi < 0) return 0;
    int se = 0;
    if (i < n && s[i] == ':') {
        i++;
        se = num(s, n, &i, 2);
        if (se < 0) return 0;
    }
    if (i < n && s[i] == '.')
        for (i++; i < n && isdigit((unsigned char)s[i]); i++) {}
    int off = 0;
    if (i < n && (s[i] == '+' || s[i] == '-')) {
        int sign = s[i++] == '-' ? -1 : 1;
        int oh = num(s, n, &i, 2);
        if (oh < 0) return 0;
        if (i < n && s[i] == ':') i++;
        int om = num(s, n, &i, 2);
        if (om < 0) om = 0;
        off = sign * (oh * 3600 + om * 60);
    }
    int64_t t = days_from_civil(Y, M, D) * 86400 + h * 3600 + mi * 60 + se - off;
    return t > 0 && t < 0xFFFFFFFFLL ? (uint32_t)t : 0;
}

// The end of the JSON value starting at p (string, number, object, array, literal).
static const char *skip_value(const char *p, const char *end)
{
    if (p >= end) return end;
    if (*p == '"') {
        for (p++; p < end && *p != '"'; p++)
            if (*p == '\\') p++;
        return p < end ? p + 1 : end;
    }
    if (*p == '{' || *p == '[') {
        int depth = 0;
        for (; p < end; p++) {
            if (*p == '"') {
                p = skip_value(p, end) - 1;
            } else if (*p == '{' || *p == '[') {
                depth++;
            } else if (*p == '}' || *p == ']') {
                if (--depth == 0) return p + 1;
            }
        }
        return end;
    }
    while (p < end && *p != ',' && *p != '}' && *p != ']') p++;
    return p;
}

// Top-level member of the entity object (depth 1), so attribute keys with the
// same name are not mistaken for it.
static const char *top_member(const char *json, const char *key)
{
    const char *p = json, *end = json + strlen(json);
    while (p < end && *p != '{') p++;
    if (p >= end) return NULL;
    p++;
    size_t kl = strlen(key);
    while (p < end) {
        while (p < end && (isspace((unsigned char)*p) || *p == ',')) p++;
        if (p >= end || *p != '"') return NULL;
        const char *k = p + 1;
        const char *ke = skip_value(p, end) - 1;  // closing quote
        p = ke + 1;
        while (p < end && isspace((unsigned char)*p)) p++;
        if (p >= end || *p != ':') return NULL;
        p++;
        while (p < end && isspace((unsigned char)*p)) p++;
        if ((size_t)(ke - k) == kl && !strncmp(k, key, kl)) return p;
        p = skip_value(p, end);
    }
    return NULL;
}

bool ha_state_str(const char *json, char *out, size_t outn)
{
    const char *v = top_member(json, "state");
    if (!v || *v != '"' || !outn) return false;
    size_t o = 0;
    for (v++; *v && *v != '"'; v++) {
        if (*v == '\\' && v[1]) v++;
        if (o + 1 < outn) out[o++] = *v;
    }
    out[o] = 0;
    return true;
}

static float to_num(const char *s)
{
    if (!s || !*s) return NAN;
    char *e;
    float f = strtof(s, &e);
    if (e == s) return NAN;
    while (*e == ' ') e++;
    if (*e) return NAN;
    return f;
}

float ha_state_num(const char *json)
{
    char b[40];
    if (!ha_state_str(json, b, sizeof b)) return NAN;
    if (!strcmp(b, "unavailable") || !strcmp(b, "unknown") || !strcmp(b, "None")) return NAN;
    return to_num(b);
}

static const char *attributes(const char *json, const char **end)
{
    const char *a = top_member(json, "attributes");
    if (!a || *a != '{') return NULL;
    *end = skip_value(a, json + strlen(json));
    return a;
}

// A member of the object starting at obj (depth 1 only).
static const char *obj_member(const char *obj, const char *end, const char *key)
{
    const char *p = obj + 1;
    size_t kl = strlen(key);
    while (p < end) {
        while (p < end && (isspace((unsigned char)*p) || *p == ',')) p++;
        if (p >= end || *p != '"') return NULL;
        const char *k = p + 1;
        const char *ke = skip_value(p, end) - 1;
        p = ke + 1;
        while (p < end && isspace((unsigned char)*p)) p++;
        if (p >= end || *p != ':') return NULL;
        p++;
        while (p < end && isspace((unsigned char)*p)) p++;
        if ((size_t)(ke - k) == kl && !strncmp(k, key, kl)) return p;
        p = skip_value(p, end);
    }
    return NULL;
}

float ha_attr_num(const char *json, const char *key)
{
    const char *end, *a = attributes(json, &end);
    if (!a) return NAN;
    const char *v = obj_member(a, end, key);
    if (!v) return NAN;
    if (*v == '"') v++;  // a number given as a string
    char *e;
    float f = strtof(v, &e);
    return e == v ? NAN : f;
}

static int cmp_pt(const void *a, const void *b)
{
    const ha_point_t *x = a, *y = b;
    return x->t < y->t ? -1 : x->t > y->t;
}

int ha_results(const char *json, ha_point_t *pts, int max)
{
    const char *end, *a = attributes(json, &end);
    if (!a) return 0;
    const char *r = obj_member(a, end, "results");
    if (!r || *r != '{') return 0;
    const char *rend = skip_value(r, end);
    const char *p = r + 1;
    int n = 0;
    bool sorted = true;
    while (p < rend && n < max) {
        while (p < rend && (isspace((unsigned char)*p) || *p == ',')) p++;
        if (p >= rend || *p != '"') break;
        const char *k = p + 1;
        const char *ke = skip_value(p, rend) - 1;
        p = ke + 1;
        while (p < rend && isspace((unsigned char)*p)) p++;
        if (p >= rend || *p != ':') break;
        p++;
        while (p < rend && isspace((unsigned char)*p)) p++;
        const char *v = p;
        p = skip_value(p, rend);
        uint32_t t = ha_parse_time(k, ke - k);
        char *e;
        float f = strtof(v, &e);
        if (!t || e == v) continue;
        if (n && t < pts[n - 1].t) sorted = false;
        pts[n].t = t;
        pts[n].v = f;
        n++;
    }
    if (!sorted) qsort(pts, n, sizeof *pts, cmp_pt);
    return n;
}

void ha_resample(const ha_point_t *pts, int n, uint32_t start, uint32_t step, int slots, float *out)
{
    int j = -1;  // last point at or before the slot
    for (int i = 0; i < slots; i++) {
        uint32_t t = start + (uint32_t)i * step;
        while (j + 1 < n && pts[j + 1].t <= t) j++;
        out[i] = j >= 0 ? pts[j].v : NAN;
    }
}

bool ha_next_change(const ha_point_t *pts, int n, uint32_t now, float cur, float *val, uint32_t *at)
{
    for (int i = 0; i < n; i++) {
        if (pts[i].t <= now) continue;
        if (isnan(cur) || fabsf(pts[i].v - cur) > 0.005f) {
            *val = pts[i].v;
            *at = pts[i].t;
            return true;
        }
    }
    return false;
}
