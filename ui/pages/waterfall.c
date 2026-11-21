// Waterfall page: a packet station's receive passband, live, from its station
// page feed. The waterfall fills the glass and scrolls down, newest at the top,
// on an amber-on-black ramp with automatic levels; our own transmissions are
// drawn on a cool ramp so they never read as a strong station. Above it, the
// live spectrum trace, the audio scale and a bar under each modem's band.
// Every decoded frame is tagged on its burst with the callsign and SNR, and
// the tag scrolls down with it. Modem names show for a few seconds on arrival.
// Tap to listen: the station's receive audio plays on the knob, and the bezel
// becomes the volume until you tap again or leave the page.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../model.h"
#include "../page.h"
#include "../theme.h"

#define WF_TOP 96                  // y of the newest row
#define IMG_H (SCR_H - WF_TOP)     // rows kept on screen
#define SPEC_TOP 44
#define SPEC_BOT 86
#define BAR_Y 90
#define INFO_MS 6000

typedef struct {
    lv_obj_t *canvas;
    uint32_t now, shown_ms;
    bool was_visible;
    lv_draw_buf_t *img;            // IMG_H rows, written as a ring
    int head;                      // image row holding the newest data
    uint32_t seen_row;             // last model row folded in
    bool have_rows;
    float floor_b, top_b;          // auto levels, on the byte scale
    float spec[WF_W];              // smoothed trace, bytes
    uint16_t lut[256], lut_tx[256];
    bool swapped;
    // snapshot
    bool up, configured;
    char name[20];
    double dial;
    bool lsb;
    float lo, hi, rows_per_s;
    wf_band_t bands[WF_MAX_BANDS];
    int n_bands;
    uint32_t row_seq;
    wf_tag_t tags[WF_TAGS];
    uint32_t tag_seq;
    uint32_t info_ms;
    bool listen, audio_on;
    int volume;
    uint32_t vol_ms;               // last volume change, for the arc
    float audio_db;
} wf_t;

static const uint32_t band_rgb[] = {0x4CC2FF, 0xB68CFF, 0xE8C27A, 0x6FE3C1, 0xFF8FA3, 0x9AD0FF, 0xD7E36F, 0xFFB86B};

static uint16_t rgb565(int r, int g, int b, bool swap)
{
    uint16_t v = (uint16_t)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
    return swap ? (uint16_t)((v >> 8) | (v << 8)) : v;
}

// Piecewise-linear ramp through a few stops.
static void build_lut(uint16_t *lut, const uint32_t *stops, int n, bool swap)
{
    for (int i = 0; i < 256; i++) {
        float x = i / 255.0f * (n - 1);
        int a = (int)x;
        if (a >= n - 1) a = n - 2;
        float f = x - a;
        uint32_t c0 = stops[a], c1 = stops[a + 1];
        int r = (int)(((c0 >> 16) & 255) * (1 - f) + ((c1 >> 16) & 255) * f);
        int g = (int)(((c0 >> 8) & 255) * (1 - f) + ((c1 >> 8) & 255) * f);
        int b = (int)((c0 & 255) * (1 - f) + (c1 & 255) * f);
        lut[i] = rgb565(r, g, b, swap);
    }
}

static bool clip(lv_area_t *out, const lv_area_t *a, const lv_area_t *b)
{
    out->x1 = LV_MAX(a->x1, b->x1);
    out->y1 = LV_MAX(a->y1, b->y1);
    out->x2 = LV_MIN(a->x2, b->x2);
    out->y2 = LV_MIN(a->y2, b->y2);
    return out->x1 <= out->x2 && out->y1 <= out->y2;
}

static float hz_to_x(const wf_t *s, float hz) { return (hz - s->lo) / (s->hi - s->lo) * WF_W; }

// Fold one model row into the image, mapping bytes through the current levels.
static void add_row(wf_t *s, const uint8_t *row, bool tx)
{
    // Levels: floor at the row's quarter point, top a little above its peak; eased.
    int hist[256] = {0};
    for (int i = 0; i < WF_W; i++) hist[row[i]]++;
    int acc = 0, q = -1, mx = 0;
    for (int i = 0; i < 256; i++) {
        acc += hist[i];
        if (q < 0 && acc >= WF_W / 4) q = i;
        if (hist[i]) mx = i;
    }
    if (!tx) {
        float f = q - 6.0f, t = fmaxf(f + 60.0f, mx + 8.0f);
        if (!s->have_rows) {
            s->floor_b = f;
            s->top_b = t;
        }
        s->floor_b += (f - s->floor_b) * 0.05f;
        s->top_b += (t > s->top_b ? 0.2f : 0.01f) * (t - s->top_b);
    }
    s->have_rows = true;
    s->head = (s->head - 1 + IMG_H) % IMG_H;
    uint16_t *px = (uint16_t *)(s->img->data + (size_t)s->head * s->img->header.stride);
    const uint16_t *lut = tx ? s->lut_tx : s->lut;
    float span = fmaxf(20.0f, s->top_b - s->floor_b);
    for (int i = 0; i < WF_W; i++) {
        float v = (row[i] - s->floor_b) / span;
        v = v < 0 ? 0 : v > 1 ? 1 : v;
        px[i] = lut[(int)(v * 255)];
    }
}

static void blit(lv_layer_t *l, wf_t *s)
{
    lv_draw_image_dsc_t d;
    lv_draw_image_dsc_init(&d);
    d.src = s->img;
    const lv_area_t saved = l->_clip_area;
    // Image rows head..IMG_H-1 go first, from WF_TOP down; then rows 0..head-1.
    int first = IMG_H - s->head;
    lv_area_t top = {th_ox, WF_TOP, th_ox + WF_W - 1, WF_TOP + first - 1};
    lv_area_t at = {th_ox, WF_TOP - s->head, th_ox + WF_W - 1, WF_TOP - s->head + IMG_H - 1};
    lv_area_t c;
    if (clip(&c, &top, &saved)) {
        l->_clip_area = c;
        lv_draw_image(l, &d, &at);
    }
    if (s->head > 0) {
        lv_area_t bot = {th_ox, WF_TOP + first, th_ox + WF_W - 1, SCR_H - 1};
        lv_area_t at2 = {th_ox, WF_TOP + first, th_ox + WF_W - 1, WF_TOP + first + IMG_H - 1};
        if (clip(&c, &bot, &saved)) {
            l->_clip_area = c;
            lv_draw_image(l, &d, &at2);
        }
    }
    l->_clip_area = saved;
}

static void draw_top(lv_layer_t *l, wf_t *s)
{
    char b[32];
    // Audio scale, every kHz.
    for (int k = 1; k * 1000 < s->hi; k++) {
        if (k * 1000 < s->lo) continue;
        float x = SCR_CX - 180 + hz_to_x(s, k * 1000.0f);
        th_line(l, x, SPEC_TOP - 4, x, SPEC_BOT, 1, C_HAIR, LV_OPA_COVER);
        snprintf(b, sizeof b, "%dk", k);
        th_text(l, &font_mono_11, (int)x, SPEC_TOP - 18, 30, LV_TEXT_ALIGN_CENTER, C_DIM, LV_OPA_COVER, 0, b);
    }
    // Spectrum trace.
    float span = fmaxf(20.0f, s->top_b - s->floor_b);
    float px = -1, py = 0;
    for (int i = 0; i < WF_W; i += 2) {
        float v = (s->spec[i] - s->floor_b) / span;
        v = v < 0 ? 0 : v > 1 ? 1 : v;
        float x = SCR_CX - 180 + i, y = SPEC_BOT - v * (SPEC_BOT - SPEC_TOP);
        if (px >= 0) th_line(l, px, py, x, y, 1, C_ACCENT, 210);
        px = x;
        py = y;
    }
    // A bar under each modem's band.
    bool info = s->now - s->info_ms < INFO_MS;
    for (int i = 0; i < s->n_bands; i++) {
        const wf_band_t *bd = &s->bands[i];
        float x0 = SCR_CX - 180 + hz_to_x(s, bd->lo_hz), x1 = SCR_CX - 180 + hz_to_x(s, bd->hi_hz);
        lv_color_t c = lv_color_hex(band_rgb[bd->sub % 8]);
        th_line(l, x0, BAR_Y, x1, BAR_Y, 3, c, LV_OPA_COVER);
        if (info) {
            th_text(l, &font_mono_11, (int)((x0 + x1) / 2), WF_TOP + 4, 120, LV_TEXT_ALIGN_CENTER, c, LV_OPA_COVER, 0,
                    bd->name);
        }
    }
    // Header: the station, then its dial; while listening, the audio state.
    uint32_t age = s->now - s->shown_ms;
    if (s->listen) {
        const char *st = s->audio_on ? "LISTENING" : "STARTING AUDIO";
        th_text(l, &font_mono_11, SCR_CX, 8, 200, LV_TEXT_ALIGN_CENTER, C_ACCENT, LV_OPA_COVER, 3, st);
    } else if (age < 3000 && !info) {
        th_text(l, &font_sans_15, SCR_CX, 6, 200, LV_TEXT_ALIGN_CENTER, C_SUB, LV_OPA_COVER, 4, s->name);
    } else if (s->dial > 0) {
        snprintf(b, sizeof b, "%.2f kHz %s", s->dial / 1000.0, s->lsb ? "LSB" : "USB");
        th_text(l, &font_mono_11, SCR_CX, 8, 200, LV_TEXT_ALIGN_CENTER, C_DIM, LV_OPA_COVER, 1, b);
    }
}

static void draw_tags(lv_layer_t *l, wf_t *s)
{
    char b[32];
    int n = s->tag_seq < WF_TAGS ? (int)s->tag_seq : WF_TAGS;
    for (int k = 0; k < n; k++) {
        const wf_tag_t *t = &s->tags[(s->tag_seq - 1 - k) % WF_TAGS];
        int32_t age_start = (int32_t)(s->row_seq - t->row);  // rows since the burst began
        int32_t age_end = age_start - t->rows;
        if (age_end > IMG_H || age_start < 0) continue;
        const wf_band_t *bd = NULL;
        for (int i = 0; i < s->n_bands; i++)
            if (s->bands[i].sub == t->sub) bd = &s->bands[i];
        if (!bd) continue;
        float x0 = SCR_CX - 180 + hz_to_x(s, bd->lo_hz);
        float y_new = WF_TOP + (age_end < 0 ? 0 : age_end), y_old = WF_TOP + age_start;
        if (y_old > SCR_H) y_old = SCR_H;
        lv_color_t c = t->tx ? C_COOL : C_INK;
        th_line(l, x0 - 3, y_new, x0 - 3, y_old, 2, c, 220);
        th_line(l, x0 - 3, y_new, x0 + 2, y_new, 2, c, 220);
        if (isnan(t->snr_db)) snprintf(b, sizeof b, "%s", t->tx ? "TX" : t->from);
        else snprintf(b, sizeof b, "%s %.0fdB", t->from, t->snr_db);
        if (t->tx) snprintf(b, sizeof b, "TX %s", t->to);
        if (k >= 8) continue;  // brackets on all, labels on the newest few
        // Label to the left of the band if it fits inside the glass at that height,
        // otherwise just right of the band.
        float x1 = SCR_CX - 180 + hz_to_x(s, bd->hi_hz);
        lv_draw_rect_dsc_t bg;
        lv_draw_rect_dsc_init(&bg);
        bg.bg_color = C_BG;
        bg.bg_opa = 170;
        bg.radius = 3;
        int w = (int)strlen(b) * 7 + 6;
        float dy = y_new + 6 - SCR_CY, hw = sqrtf(fmaxf(0, 175.0f * 175.0f - dy * dy));
        bool left = x0 - 6 - w >= SCR_CX - hw;
        int x = left ? (int)x0 - 6 - w : (int)x1 + 4;
        if (!left && x + w > SCR_CX + hw) x = (int)x0 + 4;  // neither side fits: over the band
        lv_area_t a = {x, (int)y_new - 1, x + w, (int)y_new + 13};
        lv_draw_rect(l, &bg, &a);
        th_text(l, &font_mono_11, x + 3, (int)y_new, w, LV_TEXT_ALIGN_LEFT, t->tx ? C_COOL : C_INK, LV_OPA_COVER, 0, b);
    }
}

static void draw_body(lv_event_t *e)
{
    wf_t *s = lv_event_get_user_data(e);
    lv_layer_t *l = lv_event_get_layer(e);
    if (!s->configured) {
        th_text(l, &font_mono_16, SCR_CX, SCR_CY - 30, 260, LV_TEXT_ALIGN_CENTER, C_SUB, LV_OPA_COVER, 2, "WATERFALL");
        th_text(l, &font_mono_11, SCR_CX, SCR_CY - 4, 240, LV_TEXT_ALIGN_CENTER, C_DIM, LV_OPA_COVER, 1,
                "set the station page address");
        th_text(l, &font_mono_11, SCR_CX, SCR_CY + 12, 240, LV_TEXT_ALIGN_CENTER, C_DIM, LV_OPA_COVER, 1,
                "on the settings page");
        return;
    }
    if (s->img && s->have_rows) blit(l, s);
    draw_top(l, s);
    draw_tags(l, s);
    th_line(l, SCR_CX - 180, WF_TOP - 1, SCR_CX + 180, WF_TOP - 1, 1, C_FAINT, LV_OPA_COVER);
    if (s->listen) {
        // Audio level: a thin bar round the right of the rim, live.
        float lv = isnan(s->audio_db) ? 0 : fminf(1, fmaxf(0, (s->audio_db + 60) / 60));
        th_arc(l, SCR_CX, SCR_CY, 172, 3, 40, 140, C_FAINT, LV_OPA_COVER);
        if (lv > 0.01f) th_arc(l, SCR_CX, SCR_CY, 172, 3, 140 - 100 * lv, 140, C_ACCENT, LV_OPA_COVER);
        // Volume: round the left of the rim, bright while it is being turned.
        uint32_t since = s->now - s->vol_ms;
        lv_opa_t o = since < 1500 ? LV_OPA_COVER : since < 2500 ? (lv_opa_t)(255 - 160 * (since - 1500) / 1000) : 95;
        th_arc(l, SCR_CX, SCR_CY, 172, 3, 220, 320, C_FAINT, LV_OPA_COVER);
        if (s->volume > 0) th_arc(l, SCR_CX, SCR_CY, 172, 3, 220, 220 + s->volume, C_INK, o);
        if (since < 2500) {
            char v[12];
            snprintf(v, sizeof v, "VOL %d", s->volume);
            lv_draw_rect_dsc_t bg;
            lv_draw_rect_dsc_init(&bg);
            bg.bg_color = C_BG;
            bg.bg_opa = (lv_opa_t)(o * 220 / 255);
            bg.radius = 6;
            lv_area_t a = {SCR_CX - 134, SCR_CY - 12, SCR_CX - 54, SCR_CY + 12};
            lv_draw_rect(l, &bg, &a);
            th_text(l, &font_mono_13, SCR_CX - 128, SCR_CY - 8, 80, LV_TEXT_ALIGN_LEFT, C_INK, o, 1, v);
        }
    }
    if (!s->up)
        th_text(l, &font_mono_11, SCR_CX, SCR_CY, 160, LV_TEXT_ALIGN_CENTER, C_CRIT, LV_OPA_COVER, 2,
                s->have_rows ? "STALE" : "CONNECTING");
}

TH_PAGE_DRAW(draw_cb, draw_body, (lv_obj_t *)lv_event_get_current_target(e))

static void create(page_t *p, lv_obj_t *root)
{
    wf_t *s = calloc(1, sizeof *s);
    p->state = s;
    s->volume = 40;
    s->canvas = lv_obj_create(root);
    lv_obj_remove_style_all(s->canvas);
    lv_obj_set_size(s->canvas, SCR_W, SCR_H);
    lv_obj_remove_flag(s->canvas, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s->canvas, draw_cb, LV_EVENT_DRAW_MAIN, s);
    // Same pixel format as the display, so the copy each frame is a plain memcpy.
    lv_color_format_t cf = lv_display_get_color_format(lv_display_get_default());
    s->swapped = cf == LV_COLOR_FORMAT_RGB565_SWAPPED;
    s->img = lv_draw_buf_create(WF_W, IMG_H, cf, 0);
    if (s->img) lv_draw_buf_clear(s->img, NULL);
    static const uint32_t rx[] = {0x000000, 0x0B0F24, 0x2A1446, 0x7A2448, 0xD9601E, 0xFF9F1C, 0xFFE2A8};
    static const uint32_t tx[] = {0x000000, 0x06182A, 0x0E3A5C, 0x2A7FB0, 0x9AD8FF};
    build_lut(s->lut, rx, 7, s->swapped);
    build_lut(s->lut_tx, tx, 5, s->swapped);
}

static void destroy(page_t *p)
{
    wf_t *s = p->state;
    if (s->img) lv_draw_buf_destroy(s->img);
    free(s);
}

static void tick(page_t *p, uint32_t now, uint32_t dt, bool visible)
{
    wf_t *s = p->state;
    s->now = now;
    if (visible && !s->was_visible) {
        s->shown_ms = now;
        s->info_ms = now;
    }
    if (!visible && s->listen) s->listen = false;  // leaving the page stops the audio
    s->was_visible = visible;
    uint8_t rows[8][WF_W], txf[8];
    int nr = 0;
    uint8_t spec[WF_W];
    model_lock();
    s->up = g_model.wf_up;
    s->configured = g_model.wf_hi_hz > g_model.wf_lo_hz || g_model.wf_name[0];
    snprintf(s->name, sizeof s->name, "%s", g_model.wf_name[0] ? g_model.wf_name : "WATERFALL");
    s->dial = g_model.wf_dial_hz;
    s->lsb = g_model.wf_lsb;
    s->lo = g_model.wf_lo_hz;
    s->hi = g_model.wf_hi_hz > g_model.wf_lo_hz ? g_model.wf_hi_hz : g_model.wf_lo_hz + 3000;
    s->rows_per_s = g_model.wf_rows_per_s;
    s->n_bands = g_model.wf_n_bands;
    memcpy(s->bands, g_model.wf_bands, sizeof(wf_band_t) * s->n_bands);
    memcpy(s->tags, g_model.wf_tags, sizeof s->tags);
    s->tag_seq = g_model.wf_tag_seq;
    memcpy(spec, g_model.wf_spec, sizeof spec);
    uint32_t seq = g_model.wf_row_seq;
    g_model.wf_listen = s->listen;
    g_model.wf_volume = (uint8_t)s->volume;
    s->audio_on = g_model.wf_audio_on;
    s->audio_db = g_model.wf_audio_dbfs;
    // Take the rows we have not folded in yet (if we fell far behind, the newest).
    uint32_t from = s->seen_row;
    if (seq - from > WF_ROWS) from = seq - WF_ROWS;
    if (seq - from > 8) from = seq - 8;
    for (uint32_t r = from; r < seq; r++, nr++) {
        memcpy(rows[nr], g_model.wf_rows[r % WF_ROWS], WF_W);
        txf[nr] = g_model.wf_row_tx[r % WF_ROWS];
    }
    model_unlock();
    s->seen_row = seq;
    s->row_seq = seq;
    if (s->img)
        for (int i = 0; i < nr; i++) add_row(s, rows[i], txf[i]);
    for (int i = 0; i < WF_W; i++) s->spec[i] = th_ease(s->spec[i], spec[i], dt, 120);
    if (visible) lv_obj_invalidate(s->canvas);
}

static void tap(page_t *p, int x, int y)
{
    wf_t *s = p->state;
    if (!s->configured) return;
    s->listen = !s->listen;
    if (s->listen) s->vol_ms = s->now;  // show the volume as listening starts
}

static bool knob(page_t *p, int delta)
{
    wf_t *s = p->state;
    if (!s->listen) return false;
    s->volume = s->volume + delta * 5;
    if (s->volume < 0) s->volume = 0;
    if (s->volume > 100) s->volume = 100;
    s->vol_ms = s->now;
    return true;
}

static alert_level_t status(page_t *p, char *why, int len)
{
    wf_t *s = p->state;
    if (s->configured && s->have_rows && !s->up) {
        snprintf(why, len, "%s waterfall feed lost", s->name);
        return ALERT_WARN;
    }
    return ALERT_NONE;
}

const page_type_t page_waterfall = {
    .type = "waterfall", .label = "Waterfall", .create = create, .destroy = destroy,
    .tick = tick, .tap = tap, .knob = knob, .status = status,
};
