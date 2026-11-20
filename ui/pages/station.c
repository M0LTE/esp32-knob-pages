// Station page: live traffic on a packet station, from a listen-only KISS tap.
// The outer band is a ten-minute clock: "now" is at 12 o'clock and every frame
// is a radial tick that drifts anticlockwise as it ages, white for frames heard
// and amber for frames the station sent; tick length follows frame size. The
// centre shows the newest frame and the few before it. Tap to change the
// band's span between 10 minutes and an hour.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../model.h"
#include "../page.h"
#include "../theme.h"

#define BAND_IN 122
#define BAND_OUT 142
#define SHOWN 3

typedef struct {
    lv_obj_t *canvas;
    uint32_t now;
    uint32_t span_ms;       // band covers this much time
    float span_disp;        // eased, so a change of span animates
    uint32_t seen_seq;
    uint32_t arrive_ms;     // when the newest frame arrived, for the entry animation
    uint32_t ripple_ms;
    uint8_t ripple_tx;
    bool up;
    char name[20];
    int n;
    kiss_frame_t f[KISS_FRAMES];  // newest first
    th_scenery_t scenery;   // draw_graticule(), for the inputs hashed in sc_key
    uint32_t sc_key;
} station_t;

static float age_angle(const station_t *s, uint32_t age_ms) { return -360.0f * age_ms / s->span_disp; }

static void fmt_age(char *b, int n, uint32_t ms)
{
    uint32_t sec = ms / 1000;
    if (sec < 60) snprintf(b, n, "%us", (unsigned)sec);
    else if (sec < 3600) snprintf(b, n, "%um", (unsigned)(sec / 60));
    else snprintf(b, n, "%uh", (unsigned)(sec / 3600));
}

// The band's rings and time scale: the page's scenery.
static void draw_graticule(lv_layer_t *l, station_t *s)
{
    th_ring(l, SCR_CX, SCR_CY, BAND_IN - 3, 1, C_FAINT, LV_OPA_COVER);
    th_ring(l, SCR_CX, SCR_CY, BAND_OUT + 3, 1, C_FAINT, LV_OPA_COVER);
    // Time graticule: one tick per minute (10 min span) or per 5 minutes (60 min).
    uint32_t step = s->span_ms <= 600000 ? 60000 : 300000;
    for (uint32_t t = step; t < (uint32_t)s->span_disp; t += step) {
        float a = age_angle(s, t);
        int major = (t % (step * 5)) == 0 && fabsf(fmodf(-a + 720.0f, 360.0f) - 180.0f) > 30;
        th_line(l, th_px(BAND_OUT + 4, a), th_py(BAND_OUT + 4, a), th_px(BAND_OUT + (major ? 12 : 8), a),
                th_py(BAND_OUT + (major ? 12 : 8), a), 1, major ? C_DIM : C_FAINT, LV_OPA_COVER);
        if (major) {
            char b[8];
            snprintf(b, sizeof b, "-%um", (unsigned)(t / 60000));
            float x = th_px(BAND_OUT + 22, a), y = th_py(BAND_OUT + 22, a);
            th_text(l, &font_mono_11, (int)x, (int)y - 7, 40, LV_TEXT_ALIGN_CENTER, C_DIM, LV_OPA_COVER, 0, b);
        }
    }
}

// Everything draw_graticule() takes from the state, as the draw calls round it.
static uint32_t graticule_key(const station_t *s)
{
    uint32_t h = TH_HASH_SEED;
    uint32_t step = s->span_ms <= 600000 ? 60000 : 300000;
    for (uint32_t t = step; t < (uint32_t)s->span_disp; t += step) {
        float a = age_angle(s, t);
        int major = (t % (step * 5)) == 0 && fabsf(fmodf(-a + 720.0f, 360.0f) - 180.0f) > 30;
        h = th_hash(h, major);
        h = th_hash_point(h, th_px(BAND_OUT + 4, a), th_py(BAND_OUT + 4, a));
        h = th_hash_point(h, th_px(BAND_OUT + (major ? 12 : 8), a), th_py(BAND_OUT + (major ? 12 : 8), a));
        if (major) h = th_hash(th_hash(h, (int)th_px(BAND_OUT + 22, a)), (int)th_py(BAND_OUT + 22, a));
        h = th_hash(h, (int32_t)t);
    }
    return h;
}

static void draw_band(lv_layer_t *l, station_t *s)
{
    // Frames.
    for (int i = s->n - 1; i >= 0; i--) {
        const kiss_frame_t *f = &s->f[i];
        uint32_t age = s->now - f->t_ms;
        if (age >= (uint32_t)s->span_disp) continue;
        float a = age_angle(s, age);
        float len = 6 + 14 * fminf(1.0f, log10f(f->len + 1) / 2.6f);
        float r0 = BAND_IN, r1 = BAND_IN + len;
        lv_opa_t o = (lv_opa_t)(255 - 150 * age / s->span_disp);
        th_line(l, th_px(r0, a), th_py(r0, a), th_px(r1, a), th_py(r1, a), f->tx ? 3 : 2,
                f->tx ? C_ACCENT : C_INK, o);
    }
    // "Now" marker and the arrival ripple.
    th_line(l, SCR_CX, SCR_CY - BAND_OUT - 2, SCR_CX, SCR_CY - BAND_IN + 2, 2, C_ACCENT, 220);
    uint32_t since = s->now - s->ripple_ms;
    if (since < 1200) {
        float k = since / 1200.0f;
        lv_color_t c = s->ripple_tx ? C_ACCENT : C_INK;
        th_arc(l, SCR_CX, SCR_CY, (int)(BAND_OUT + 4 + 20 * k), 2, -40 * (1 - k * 0.5f), 40 * (1 - k * 0.5f), c,
               (lv_opa_t)(180 * (1 - k)));
    }
}

static void draw_centre(lv_layer_t *l, station_t *s)
{
    char b[64], age[8];
    if (!s->up) {
        th_text(l, &font_mono_22, SCR_CX, SCR_CY - 30, 220, LV_TEXT_ALIGN_CENTER, C_CRIT, LV_OPA_COVER, 3, "NO LINK");
        th_text(l, &font_mono_11, SCR_CX, SCR_CY + 2, 200, LV_TEXT_ALIGN_CENTER, C_DIM, LV_OPA_COVER, 1,
                "KISS port not answering");
        return;
    }
    if (!s->n) {
        th_text(l, &font_mono_13, SCR_CX, SCR_CY - 8, 200, LV_TEXT_ALIGN_CENTER, C_DIM, LV_OPA_COVER, 2, "LISTENING");
        return;
    }
    const kiss_frame_t *f = &s->f[0];
    uint32_t since = s->now - s->arrive_ms;
    float k = since < 450 ? since / 450.0f : 1.0f;
    float e = 1 - (1 - k) * (1 - k) * (1 - k);
    int dy = (int)(14 * (1 - e));
    lv_opa_t o = (lv_opa_t)(255 * e);

    snprintf(b, sizeof b, "%s  CH%u  %uB", f->tx ? "TX" : "RX", f->subch, f->len);
    th_text(l, &font_mono_11, SCR_CX, 92 + dy, 200, LV_TEXT_ALIGN_CENTER, f->tx ? C_ACCENT : C_DIM, o, 2, b);
    th_text(l, &font_sans_28, SCR_CX, 106 + dy, 220, LV_TEXT_ALIGN_CENTER, C_INK, o, 1, f->src);
    snprintf(b, sizeof b, "\xE2\x86\x92 %s", f->dst);
    th_text(l, &font_mono_16, SCR_CX, 142 + dy, 220, LV_TEXT_ALIGN_CENTER, C_SUB, o, 0, b);
    fmt_age(age, sizeof age, s->now - f->t_ms);
    snprintf(b, sizeof b, "%s ago", age);
    th_text(l, &font_mono_13, SCR_CX, 166 + dy, 200, LV_TEXT_ALIGN_CENTER, C_DIM, o, 0, b);

    // Older frames.
    th_line(l, SCR_CX - 70, 192, SCR_CX + 70, 192, 1, C_FAINT, LV_OPA_COVER);
    for (int i = 1; i < SHOWN && i < s->n; i++) {
        const kiss_frame_t *g = &s->f[i];
        fmt_age(age, sizeof age, s->now - g->t_ms);
        snprintf(b, sizeof b, "%-9s%-8s%4s", g->src, g->dst, age);
        int y = 198 + (i - 1) * 16 + dy;
        th_text(l, &font_mono_11, SCR_CX, y, 200, LV_TEXT_ALIGN_CENTER, g->tx ? C_ACCENT : C_SUB,
                (lv_opa_t)(230 - 55 * (i - 1)), 0, b);
    }

    // Rates over the last ten minutes.
    int rx = 0, tx = 0;
    for (int i = 0; i < s->n; i++) {
        if (s->now - s->f[i].t_ms > 600000) break;
        if (s->f[i].tx) tx++; else rx++;
    }
    snprintf(b, sizeof b, "%d", rx + tx);
    th_text(l, &font_mono_16, SCR_CX - 4, 240, 60, LV_TEXT_ALIGN_RIGHT, C_INK, LV_OPA_COVER, 0, b);
    th_text(l, &font_mono_11, SCR_CX + 2, 244, 80, LV_TEXT_ALIGN_LEFT, C_DIM, LV_OPA_COVER, 1, "/10min");
    snprintf(b, sizeof b, "RX %d  TX %d", rx, tx);
    th_text(l, &font_mono_11, SCR_CX, 262, 120, LV_TEXT_ALIGN_CENTER, C_DIM, LV_OPA_COVER, 1, b);
}

static void draw_body(lv_event_t *e)
{
    station_t *s = lv_event_get_user_data(e);
    lv_layer_t *l = lv_event_get_layer(e);
    if (!th_scenery_blit(l, &s->scenery, s->canvas)) draw_graticule(l, s);
    draw_band(l, s);
    draw_centre(l, s);
    th_text(l, &font_sans_15, SCR_CX - 6, 12, 120, LV_TEXT_ALIGN_CENTER, C_SUB, LV_OPA_COVER, 4, s->name);
    th_dot(l, SCR_CX + 42, 21, 3, s->up ? C_OK : C_CRIT, LV_OPA_COVER);
}

static void update_scenery(station_t *s)
{
    uint32_t key = graticule_key(s);
    if (s->scenery.buf && key == s->sc_key) return;
    lv_layer_t *l = th_scenery_begin(&s->scenery);
    if (!l) return;
    draw_graticule(l, s);
    th_scenery_end(&s->scenery, l);
    s->sc_key = key;
}

static void draw_cb(lv_event_t *e);

static void create(page_t *p, lv_obj_t *root)
{
    station_t *s = calloc(1, sizeof *s);
    p->state = s;
    s->span_ms = 600000;
    s->span_disp = s->span_ms;
    s->canvas = lv_obj_create(root);
    lv_obj_remove_style_all(s->canvas);
    lv_obj_set_size(s->canvas, SCR_W, SCR_H);
    lv_obj_remove_flag(s->canvas, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s->canvas, draw_cb, LV_EVENT_DRAW_MAIN, s);
}

TH_PAGE_DRAW(draw_cb, draw_body, (lv_obj_t *)lv_event_get_current_target(e))

static void destroy(page_t *p)
{
    station_t *s = p->state;
    if (s->scenery.buf) lv_draw_buf_destroy(s->scenery.buf);
    free(s);
}

static void tick(page_t *p, uint32_t now, uint32_t dt, bool visible)
{
    station_t *s = p->state;
    s->now = now;
    model_lock();
    s->up = g_model.kiss_up;
    snprintf(s->name, sizeof s->name, "%s", g_model.kiss_name[0] ? g_model.kiss_name : "STATION");
    s->n = g_model.frames_n;
    for (int i = 0; i < s->n; i++) s->f[i] = *model_kiss_get(i);
    uint32_t seq = g_model.frames_seq;
    model_unlock();
    if (seq != s->seen_seq) {
        if (s->seen_seq) {
            s->arrive_ms = now;
            s->ripple_ms = now;
            s->ripple_tx = s->n ? s->f[0].tx : 0;
        }
        s->seen_seq = seq;
    }
    s->span_disp = th_ease(s->span_disp, s->span_ms, dt, 180);
    if (visible) {
        update_scenery(s);
        lv_obj_invalidate(s->canvas);
    }
}

static void tap(page_t *p, int x, int y)
{
    station_t *s = p->state;
    s->span_ms = s->span_ms == 600000 ? 3600000 : 600000;
    update_scenery(s);  // taps land between tick and draw
}

static alert_level_t status(page_t *p, char *why, int len)
{
    station_t *s = p->state;
    if (!s->up) {
        snprintf(why, len, "%s KISS link down", s->name);
        return ALERT_CRIT;
    }
    return ALERT_NONE;
}

const page_type_t page_station = {
    .type = "station", .label = "Station", .create = create, .destroy = destroy,
    .tick = tick, .tap = tap, .status = status,
};
