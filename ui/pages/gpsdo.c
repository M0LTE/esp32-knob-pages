// GPSDO page: a GPS-disciplined oscillator's health, from its JSON status on
// MQTT. The outer ring is the last hour of phase error as a clock face, "now"
// at 12 o'clock and the past running anticlockwise: the trace sits on the zero
// circle when all is well, drifts off it as the phase wanders, and turns red
// where lock was lost. Inside: the lock state and how long it has held, the
// live phase error on a centre-zero scale, satellites as a row of dots (lit
// when used), and the oscillator's vital numbers.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../model.h"
#include "../page.h"
#include "../theme.h"

#define HIST 720              // samples kept (one per message; ~5 s apart = 1 h)
#define SPAN_MS 3600000u
#define R_ZERO 146            // the zero-phase circle
#define NS_PX 0.4f            // pixels per ns off the zero circle
#define NS_CLAMP 45.0f

typedef struct {
    uint32_t t_ms;
    float tic;
    uint8_t locked;
} gsample_t;

typedef struct {
    lv_obj_t *canvas;
    uint32_t now, shown_ms;
    bool was_visible;
    uint32_t seen_seq;
    gsample_t hist[HIST];
    int head, n;
    float tic_disp;
    uint32_t missed_prev, missed_at_ms;  // when missed PPS last went up
    // snapshot
    bool up, any;
    char name[20], state[16], fix[8], alarms[64];
    bool locked, ref_ok, pps, dac_near;
    uint32_t locked_s, lock_losses, missed, since_missed, lock_counter, lock_target;
    float tic, ocxo, amb, ppb5, ppb30, hdop;
    int32_t dac;
    int used, view;
} gps_t;

static lv_color_t state_colour(const gps_t *s)
{
    if (!s->any) return C_DIM;
    if (s->locked && s->ref_ok && s->pps) return C_OK;
    if (!strcmp(s->state, "HOLDOVER") || !strcmp(s->state, "WARMUP")) return C_WARN;
    return C_CRIT;
}

static void fmt_dur(char *b, int n, uint32_t s)
{
    if (s >= 86400) snprintf(b, n, "%ud %02uh %02um", (unsigned)(s / 86400), (unsigned)(s / 3600 % 24), (unsigned)(s / 60 % 60));
    else if (s >= 3600) snprintf(b, n, "%uh %02um", (unsigned)(s / 3600), (unsigned)(s / 60 % 60));
    else snprintf(b, n, "%um %02us", (unsigned)(s / 60), (unsigned)(s % 60));
}

static void draw_ring(lv_layer_t *l, gps_t *s)
{
    // Graticule: the zero circle, faint +/-25 ns circles, a tick every 10 minutes.
    th_ring(l, SCR_CX, SCR_CY, R_ZERO, 1, C_FAINT, LV_OPA_COVER);
    for (int i = 0; i < 120; i++) {
        float a = i * 3.0f;
        th_dot(l, th_px(R_ZERO + 25 * NS_PX * 1.6f, a), th_py(R_ZERO + 25 * NS_PX * 1.6f, a), 1, C_HAIR, LV_OPA_COVER);
        th_dot(l, th_px(R_ZERO - 25 * NS_PX * 1.6f, a), th_py(R_ZERO - 25 * NS_PX * 1.6f, a), 1, C_HAIR, LV_OPA_COVER);
    }
    for (int m = 5; m < 60; m += 5) {
        float a = -360.0f * m / 60;
        bool major = m % 15 == 0;
        th_line(l, th_px(R_ZERO + 22, a), th_py(R_ZERO + 22, a), th_px(R_ZERO + (major ? 29 : 26), a),
                th_py(R_ZERO + (major ? 29 : 26), a), 1, major ? C_DIM : C_FAINT, LV_OPA_COVER);
    }
    // Labels on the sides only, clear of the page dots at the bottom.
    th_text(l, &font_mono_11, (int)th_px(R_ZERO - 22, 270), (int)th_py(R_ZERO - 22, 270) - 7, 40,
            LV_TEXT_ALIGN_CENTER, C_DIM, LV_OPA_COVER, 0, "-15m");
    th_text(l, &font_mono_11, (int)th_px(R_ZERO - 22, 90), (int)th_py(R_ZERO - 22, 90) - 7, 40,
            LV_TEXT_ALIGN_CENTER, C_DIM, LV_OPA_COVER, 0, "-45m");
    // The trace, oldest first.
    float px = 0, py = 0;
    bool have = false;
    for (int k = s->n - 1; k >= 0; k--) {
        const gsample_t *g = &s->hist[(s->head - 1 - k + HIST) % HIST];
        uint32_t age = s->now - g->t_ms;
        if (age > SPAN_MS) {
            have = false;
            continue;
        }
        float a = -360.0f * age / SPAN_MS;
        float v = isnan(g->tic) ? 0 : fminf(NS_CLAMP, fmaxf(-NS_CLAMP, g->tic));
        float r = R_ZERO + v * NS_PX * 1.6f;
        float x = th_px(r, a), y = th_py(r, a);
        lv_color_t c = g->locked ? C_INK : C_CRIT;
        lv_opa_t o = (lv_opa_t)(110 + 145 * (1 - (float)age / SPAN_MS));
        if (have) th_line(l, px, py, x, y, g->locked ? 1 : 2, c, o);
        px = x;
        py = y;
        have = true;
    }
    // Now marker.
    th_line(l, SCR_CX, SCR_CY - R_ZERO - 14, SCR_CX, SCR_CY - R_ZERO + 14, 2, C_ACCENT, 230);
}

static void draw_sats(lv_layer_t *l, gps_t *s)
{
    // A row of dots along an arc near the top: one per satellite in view, lit when used.
    int n = s->view > 24 ? 24 : s->view;
    const float pitch = 3.4f, r = 126;
    float start = -pitch * (n - 1) / 2;
    for (int i = 0; i < n; i++) {
        float a = start + i * pitch;
        bool used = i < s->used;
        th_dot(l, th_px(r, a), th_py(r, a), used ? 3 : 2, used ? C_OK : C_FAINT, LV_OPA_COVER);
    }
    char b[16];
    snprintf(b, sizeof b, "%d/%d", s->used, s->view);
    th_text(l, &font_mono_11, SCR_CX, 62, 60, LV_TEXT_ALIGN_CENTER, C_DIM, LV_OPA_COVER, 1, b);
}

static void draw_centre(lv_layer_t *l, gps_t *s)
{
    char b[48];
    lv_color_t c = state_colour(s);
    th_text(l, &font_sans_28, SCR_CX, 94, 240, LV_TEXT_ALIGN_CENTER, c, LV_OPA_COVER, 3, s->state[0] ? s->state : "--");
    if (s->locked) {
        char d[24];
        fmt_dur(d, sizeof d, s->locked_s);
        if (isnan(s->hdop)) snprintf(b, sizeof b, "%s  %s", d, s->fix);
        else snprintf(b, sizeof b, "%s  %s %.1f", d, s->fix, s->hdop);
        th_text(l, &font_mono_13, SCR_CX, 128, 240, LV_TEXT_ALIGN_CENTER, C_SUB, LV_OPA_COVER, 0, b);
    } else if (s->lock_target) {
        snprintf(b, sizeof b, "lock test %u / %u", (unsigned)s->lock_counter, (unsigned)s->lock_target);
        th_text(l, &font_mono_13, SCR_CX, 128, 220, LV_TEXT_ALIGN_CENTER, C_WARN, LV_OPA_COVER, 0, b);
    }
    // Phase error on a centre-zero scale.
    const int y = 168, half = 92;
    th_line(l, SCR_CX - half, y, SCR_CX + half, y, 1, C_FAINT, LV_OPA_COVER);
    for (int v = -40; v <= 40; v += 10) {
        float x = SCR_CX + v * half / 45.0f;
        int h = v == 0 ? 7 : v % 20 == 0 ? 5 : 3;
        th_line(l, x, y - h, x, y + h, 1, v == 0 ? C_SUB : C_FAINT, LV_OPA_COVER);
    }
    if (!isnan(s->tic_disp)) {
        float v = fminf(NS_CLAMP, fmaxf(-NS_CLAMP, s->tic_disp));
        float x = SCR_CX + v * half / 45.0f;
        th_line(l, x, y - 11, x, y + 11, 3, C_ACCENT, LV_OPA_COVER);
        snprintf(b, sizeof b, "%+.0f ns", s->tic);
        th_text(l, &font_mono_16, SCR_CX, y + 14, 120, LV_TEXT_ALIGN_CENTER, C_INK, LV_OPA_COVER, 0, b);
    }
    th_text(l, &font_mono_11, SCR_CX - half - 4, y - 7, 40, LV_TEXT_ALIGN_RIGHT, C_DIM, LV_OPA_COVER, 0, "-");
    th_text(l, &font_mono_11, SCR_CX + half + 4, y - 7, 40, LV_TEXT_ALIGN_LEFT, C_DIM, LV_OPA_COVER, 0, "+");
}

static void stat(lv_layer_t *l, int x, int y, const char *label, const char *val, lv_color_t c)
{
    th_text(l, &font_mono_11, x, y, 90, LV_TEXT_ALIGN_CENTER, C_DIM, LV_OPA_COVER, 2, label);
    th_text(l, &font_mono_13, x, y + 13, 96, LV_TEXT_ALIGN_CENTER, c, LV_OPA_COVER, 0, val);
}

static void draw_stats(lv_layer_t *l, gps_t *s)
{
    char b[24];
    const int y1 = 210, y2 = 246, dx = 70;
    snprintf(b, sizeof b, "%.1f\xC2\xB0", s->ocxo);
    stat(l, SCR_CX - dx, y1, "OCXO", b, C_INK);
    snprintf(b, sizeof b, "%ld", (long)s->dac);
    stat(l, SCR_CX, y1, "DAC", b, s->dac_near ? C_WARN : C_INK);
    snprintf(b, sizeof b, "%+.3f", s->ppb5);
    stat(l, SCR_CX + dx, y1, "PPB 5M", b, fabsf(s->ppb5) > 1 ? C_WARN : C_INK);
    snprintf(b, sizeof b, "%.1f\xC2\xB0", s->amb);
    stat(l, SCR_CX - dx, y2, "AMBIENT", b, C_SUB);
    snprintf(b, sizeof b, "%u", (unsigned)s->missed);
    bool recent = s->missed_at_ms && s->now - s->missed_at_ms < 3600000u;
    stat(l, SCR_CX, y2, "MISSED", b, recent ? C_WARN : C_SUB);
    snprintf(b, sizeof b, "%+.3f", s->ppb30);
    stat(l, SCR_CX + dx, y2, "PPB 30M", b, C_SUB);
    if (s->alarms[0])
        th_text(l, &font_mono_11, SCR_CX, 284, 200, LV_TEXT_ALIGN_CENTER, C_WARN, LV_OPA_COVER, 1, s->alarms);
}

static void draw_body(lv_event_t *e)
{
    gps_t *s = lv_event_get_user_data(e);
    lv_layer_t *l = lv_event_get_layer(e);
    if (!s->any) {
        th_text(l, &font_mono_16, SCR_CX, SCR_CY - 30, 260, LV_TEXT_ALIGN_CENTER, C_SUB, LV_OPA_COVER, 2, "GPSDO");
        th_text(l, &font_mono_11, SCR_CX, SCR_CY - 4, 240, LV_TEXT_ALIGN_CENTER, C_DIM, LV_OPA_COVER, 1,
                "waiting for its status;");
        th_text(l, &font_mono_11, SCR_CX, SCR_CY + 12, 240, LV_TEXT_ALIGN_CENTER, C_DIM, LV_OPA_COVER, 1,
                "set MQTT on the settings page");
        return;
    }
    draw_ring(l, s);
    draw_sats(l, s);
    draw_centre(l, s);
    draw_stats(l, s);
    uint32_t age = s->now - s->shown_ms;
    if (age < 3000)
        th_text(l, &font_sans_15, SCR_CX, 10, 200, LV_TEXT_ALIGN_CENTER, C_SUB, LV_OPA_COVER, 4, s->name);
    if (!s->up) th_text(l, &font_mono_11, SCR_CX, 300, 120, LV_TEXT_ALIGN_CENTER, C_CRIT, LV_OPA_COVER, 2, "STALE");
}

TH_PAGE_DRAW(draw_cb, draw_body, (lv_obj_t *)lv_event_get_current_target(e))

static void create(page_t *p, lv_obj_t *root)
{
    gps_t *s = calloc(1, sizeof *s);
    p->state = s;
    s->tic_disp = NAN;
    s->canvas = lv_obj_create(root);
    lv_obj_remove_style_all(s->canvas);
    lv_obj_set_size(s->canvas, SCR_W, SCR_H);
    lv_obj_remove_flag(s->canvas, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s->canvas, draw_cb, LV_EVENT_DRAW_MAIN, s);
}

static void destroy(page_t *p) { free(p->state); }

static void tick(page_t *p, uint32_t now, uint32_t dt, bool visible)
{
    gps_t *s = p->state;
    s->now = now;
    if (visible && !s->was_visible) s->shown_ms = now;
    s->was_visible = visible;
    model_lock();
    uint32_t seq = g_model.gps_seq;
    s->up = g_model.gps_up;
    snprintf(s->name, sizeof s->name, "%s", g_model.gps_name[0] ? g_model.gps_name : "GPSDO");
    snprintf(s->state, sizeof s->state, "%s", g_model.gps_state);
    snprintf(s->fix, sizeof s->fix, "%s", g_model.gps_fix[0] ? g_model.gps_fix : "--");
    snprintf(s->alarms, sizeof s->alarms, "%s", g_model.gps_alarms);
    s->locked = g_model.gps_locked;
    s->ref_ok = g_model.gps_ref_ok;
    s->pps = g_model.gps_pps;
    s->dac_near = g_model.gps_dac_near_limit;
    s->locked_s = g_model.gps_locked_s;
    s->lock_losses = g_model.gps_lock_losses;
    s->missed = g_model.gps_missed_pps;
    s->since_missed = g_model.gps_since_missed_s;
    s->lock_counter = g_model.gps_lock_counter;
    s->lock_target = g_model.gps_lock_target;
    s->tic = g_model.gps_tic_ns;
    s->ocxo = g_model.gps_ocxo_c;
    s->amb = g_model.gps_ambient_c;
    s->ppb5 = g_model.gps_ppb_5m;
    s->ppb30 = g_model.gps_ppb_30m;
    s->hdop = g_model.gps_hdop;
    s->dac = g_model.gps_dac;
    s->used = g_model.gps_sats_used;
    s->view = g_model.gps_sats_view;
    model_unlock();
    if (seq && seq != s->seen_seq) {
        s->any = true;
        s->seen_seq = seq;
        s->hist[s->head] = (gsample_t){now, s->tic, (uint8_t)(s->locked && s->ref_ok)};
        s->head = (s->head + 1) % HIST;
        if (s->n < HIST) s->n++;
        if (s->missed > s->missed_prev && s->missed_prev) s->missed_at_ms = now;
        s->missed_prev = s->missed;
    }
    if (!isnan(s->tic)) s->tic_disp = isnan(s->tic_disp) ? s->tic : th_ease(s->tic_disp, s->tic, dt, 300);
    if (visible) lv_obj_invalidate(s->canvas);
}

static alert_level_t status(page_t *p, char *why, int len)
{
    gps_t *s = p->state;
    if (!s->any) return ALERT_NONE;
    if (!s->locked || !s->ref_ok || !s->pps) {
        snprintf(why, len, "%s %s", s->name, !s->pps ? "no PPS from GPS" : !s->locked ? "lost lock" : "reference not OK");
        return ALERT_CRIT;
    }
    if (!s->up) {
        snprintf(why, len, "%s status stale", s->name);
        return ALERT_WARN;
    }
    if (s->dac_near || s->alarms[0] || (s->missed_at_ms && s->now - s->missed_at_ms < 3600000u)) {
        snprintf(why, len, "%s %s", s->name, s->dac_near ? "DAC near its limit" : s->alarms[0] ? s->alarms : "missed PPS");
        return ALERT_WARN;
    }
    return ALERT_NONE;
}

const page_type_t page_gpsdo = {
    .type = "gpsdo", .label = "GPSDO", .create = create, .destroy = destroy,
    .tick = tick, .status = status,
};
