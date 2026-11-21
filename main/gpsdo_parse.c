#include "gpsdo_parse.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "cJSON.h"

static float f(const cJSON *o, const char *k)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, k);
    return cJSON_IsNumber(v) ? (float)v->valuedouble : NAN;
}

static uint32_t u(const cJSON *o, const char *k)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, k);
    return cJSON_IsNumber(v) && v->valuedouble > 0 ? (uint32_t)(v->valuedouble > 4294967295.0 ? 4294967295.0 : v->valuedouble)
                                                    : 0;
}

static int32_t i32(const cJSON *o, const char *k)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, k);
    if (!cJSON_IsNumber(v)) return 0;
    double d = v->valuedouble;
    return d > 2147483647.0 ? 2147483647 : d < -2147483648.0 ? INT32_MIN : (int32_t)d;
}

static bool b(const cJSON *o, const char *k) { return cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(o, k)); }

static void s(const cJSON *o, const char *k, char *dst, size_t n)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, k);
    snprintf(dst, n, "%s", cJSON_IsString(v) && v->valuestring ? v->valuestring : "");
}

bool gpsdo_parse(const char *json, size_t len, model_t *m)
{
    cJSON *r = cJSON_ParseWithLength(json, len);
    if (!cJSON_IsObject(r)) {
        cJSON_Delete(r);
        return false;
    }
    s(r, "state", m->gps_state, sizeof m->gps_state);
    m->gps_locked = b(r, "locked");
    m->gps_ref_ok = b(r, "reference_ok");
    m->gps_pps = b(r, "pps_input");
    m->gps_dac_near_limit = b(r, "dac_near_limit");
    m->gps_locked_s = u(r, "locked_s");
    m->gps_lock_losses = u(r, "lock_losses");
    m->gps_missed_pps = u(r, "missed_pps_total");
    m->gps_since_missed_s = u(r, "since_missed_pps_s");
    m->gps_lock_counter = u(r, "lock_counter");
    m->gps_lock_target = u(r, "lock_target");
    m->gps_tic_ns = f(r, "tic_ns");
    m->gps_ocxo_c = f(r, "ocxo_c");
    m->gps_ambient_c = f(r, "ambient_c");
    m->gps_ppb_5m = f(r, "freq_error_ppb_5min");
    m->gps_ppb_30m = f(r, "freq_error_ppb_30min");
    m->gps_dac = i32(r, "dac");
    s(r, "fix", m->gps_fix, sizeof m->gps_fix);
    m->gps_sats_used = (int)i32(r, "sats_used");
    m->gps_sats_view = (int)i32(r, "sats_in_view");
    m->gps_hdop = f(r, "hdop");
    // Alarms, by name, joined with commas (as many as fit).
    m->gps_alarms[0] = 0;
    const cJSON *al = cJSON_GetObjectItemCaseSensitive(r, "alarms"), *a;
    size_t o = 0;
    cJSON_ArrayForEach(a, al) {
        const char *t = cJSON_IsString(a) ? a->valuestring : NULL;
        if (!t || !*t) continue;
        size_t tl = strlen(t), need = tl + (o ? 1 : 0);
        if (o + need >= sizeof m->gps_alarms) break;
        if (o) m->gps_alarms[o++] = ',';
        memcpy(m->gps_alarms + o, t, tl);
        o += tl;
        m->gps_alarms[o] = 0;
    }
    cJSON_Delete(r);
    return true;
}
