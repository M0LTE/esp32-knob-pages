// 2.4 GHz scope: the knob's own Wi-Fi radio turned into a receiver. Entered
// from the system menu (the pager hides the other pages and holds alerts; the
// device stops Wi-Fi and every network source while it runs). Top: the tuned
// centre and span, then a live spectrum trace; a ruler marks the Wi-Fi
// channels (the non-overlapping 1, 6 and 11 as brackets, their edges carried
// faintly through the trace and the waterfall), the 2.4 GHz ISM band edges
// and the 13 cm amateur band; below, a waterfall. The bezel tunes, a tap
// changes the span; a tag at the foot says when the USB port has been
// switched off from the system menu. The 100 and 60 MHz spans are swept by the device: the
// trace is refreshed along the sweep as each part of it is captured, and the
// waterfall gets a row for every complete sweep (about 6 a second at 100 MHz).
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../model.h"
#include "../page.h"
#include "../theme.h"

#define SPEC_TOP 70
#define SPEC_BOT 124
#define RULER_Y 126   // channel ticks
#define NUM_Y 132     // channel numbers
#define BRACKET_Y 148 // 1, 6, 11
#define BRACKET13_Y 154
#define BAND_Y 162    // 13 cm amateur band
#define WF_TOP2 168
#define IMG_H2 (SCR_H - WF_TOP2)
#define F_MIN 2200.0f
#define F_MAX 2700.0f

// A tap steps through them in order; the scope opens on 40 MHz.
static const float spans[] = {100, 60, 40, 20, 10, 5};
#define SPAN_FIRST 2
#define WIDE_C 2442.0f  // 2392 to 2492 MHz at 100 MHz: all of 2.4 GHz Wi-Fi

typedef struct {
    lv_obj_t *canvas;
    uint32_t now;
    lv_draw_buf_t *img;
    int head;
    uint32_t seen_row;
    bool have_rows;
    float floor_b, top_b;
    float spec[WF_W];
    uint16_t lut[256];
    bool swapped;
    // asked / tuned
    float want_c, want_span;
    int span_i;
    bool active, usb_off;
    float c, span, rps;
    char status[32];
} scope_t;

static uint16_t rgb565(int r, int g, int b, bool swap)
{
    uint16_t v = (uint16_t)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
    return swap ? (uint16_t)((v >> 8) | (v << 8)) : v;
}

static void build_lut(scope_t *s)
{
    // Black, deep teal, cyan, warm white: a different ramp from the packet
    // waterfall, so the two are never confused.
    static const uint32_t st[] = {0x000000, 0x041A22, 0x0B4B5C, 0x1F9CB0, 0x8FE3E8, 0xFFF4D6};
    const int n = 6;
    for (int i = 0; i < 256; i++) {
        float x = i / 255.0f * (n - 1);
        int a = (int)x;
        if (a >= n - 1) a = n - 2;
        float f = x - a;
        uint32_t c0 = st[a], c1 = st[a + 1];
        s->lut[i] = rgb565((int)(((c0 >> 16) & 255) * (1 - f) + ((c1 >> 16) & 255) * f),
                           (int)(((c0 >> 8) & 255) * (1 - f) + ((c1 >> 8) & 255) * f),
                           (int)((c0 & 255) * (1 - f) + (c1 & 255) * f), s->swapped);
    }
}

static float f_to_x(const scope_t *s, float mhz) { return SCR_CX - 180 + (mhz - (s->c - s->span / 2)) / s->span * WF_W; }

static bool clip(lv_area_t *o, const lv_area_t *a, const lv_area_t *b)
{
    o->x1 = LV_MAX(a->x1, b->x1);
    o->y1 = LV_MAX(a->y1, b->y1);
    o->x2 = LV_MIN(a->x2, b->x2);
    o->y2 = LV_MIN(a->y2, b->y2);
    return o->x1 <= o->x2 && o->y1 <= o->y2;
}

static void add_row(scope_t *s, const uint8_t *row)
{
    int hist[256] = {0}, acc = 0, q = -1, mx = 0;
    for (int i = 0; i < WF_W; i++) hist[row[i]]++;
    for (int i = 0; i < 256; i++) {
        acc += hist[i];
        if (q < 0 && acc >= WF_W / 3) q = i;
        if (hist[i]) mx = i;
    }
    float f = q - 4.0f, t = fmaxf(f + 50.0f, mx + 6.0f);
    if (!s->have_rows) {
        s->floor_b = f;
        s->top_b = t;
    }
    s->floor_b += (f - s->floor_b) * 0.05f;
    s->top_b += (t > s->top_b ? 0.2f : 0.01f) * (t - s->top_b);
    s->have_rows = true;
    s->head = (s->head - 1 + IMG_H2) % IMG_H2;
    uint16_t *px = (uint16_t *)(s->img->data + (size_t)s->head * s->img->header.stride);
    float span = fmaxf(16.0f, s->top_b - s->floor_b);
    for (int i = 0; i < WF_W; i++) {
        float v = (row[i] - s->floor_b) / span;
        v = v < 0 ? 0 : v > 1 ? 1 : v;
        px[i] = s->lut[(int)(v * 255)];
    }
}

static void blit(lv_layer_t *l, scope_t *s)
{
    lv_draw_image_dsc_t d;
    lv_draw_image_dsc_init(&d);
    d.src = s->img;
    const lv_area_t saved = l->_clip_area;
    int first = IMG_H2 - s->head;
    lv_area_t top = {th_ox, WF_TOP2, th_ox + WF_W - 1, WF_TOP2 + first - 1}, c;
    lv_area_t at = {th_ox, WF_TOP2 - s->head, th_ox + WF_W - 1, WF_TOP2 - s->head + IMG_H2 - 1};
    if (clip(&c, &top, &saved)) {
        l->_clip_area = c;
        lv_draw_image(l, &d, &at);
    }
    if (s->head > 0) {
        lv_area_t bot = {th_ox, WF_TOP2 + first, th_ox + WF_W - 1, SCR_H - 1};
        lv_area_t at2 = {th_ox, WF_TOP2 + first, th_ox + WF_W - 1, WF_TOP2 + first + IMG_H2 - 1};
        if (clip(&c, &bot, &saved)) {
            l->_clip_area = c;
            lv_draw_image(l, &d, &at2);
        }
    }
    l->_clip_area = saved;
}

static void fill(lv_layer_t *l, float x1, int y1, float x2, int y2, lv_color_t c, lv_opa_t opa)
{
    lv_draw_rect_dsc_t d;
    lv_draw_rect_dsc_init(&d);
    d.bg_color = c;
    d.bg_opa = opa;
    lv_area_t a = {(int32_t)x1, y1, (int32_t)x2, y2};
    lv_draw_rect(l, &d, &a);
}

#define C_TEAL lv_color_hex(0x1F9CB0)
#define C_TEAL_DEEP lv_color_hex(0x0B4B5C)
#define C_AMATEUR lv_color_hex(0xB68CFF)

static float ch_mhz(int ch) { return ch == 14 ? 2484 : 2412 + 5 * (ch - 1); }

// The non-overlapping 20 MHz channels: a faint band behind the trace each, and
// their edges as faint lines through the trace and the waterfall.
static void draw_channel_bands(lv_layer_t *l, scope_t *s)
{
    static const int main_ch[] = {1, 6, 11};
    float x0 = SCR_CX - 180, x1 = SCR_CX + 179;
    for (int i = 0; i < 3; i++) {
        float f = ch_mhz(main_ch[i]);
        float a = f_to_x(s, f - 10), b = f_to_x(s, f + 10);
        if (b < x0 || a > x1) continue;
        fill(l, fmaxf(a, x0), SPEC_TOP, fminf(b, x1), SPEC_BOT, C_TEAL, 22);
        for (int e = 0; e < 2; e++) {
            float x = e ? b : a;
            if (x < x0 || x > x1) continue;
            th_line(l, x, SPEC_TOP, x, BRACKET_Y, 1, C_TEAL, 70);
            th_line(l, x, WF_TOP2, x, SCR_H, 1, lv_color_hex(0x8FE3E8), 34);
        }
    }
}

// A dimension-line bracket from f0 to f1 MHz at row y, ends turned up. With a
// label, the line breaks around it, at the middle of the part on screen.
static void bracket(lv_layer_t *l, scope_t *s, float f0, float f1, int y, int up, lv_color_t c, const char *label)
{
    float x0 = SCR_CX - 176, x1 = SCR_CX + 175;
    float a = f_to_x(s, f0) + 1, b = f_to_x(s, f1) - 1;
    if (b < x0 || a > x1) return;
    float va = fmaxf(a, x0), vb = fminf(b, x1);
    if (a >= x0) th_line(l, a, y - up, a, y, 1, c, LV_OPA_COVER);
    if (b <= x1) th_line(l, b, y - up, b, y, 1, c, LV_OPA_COVER);
    float half = label ? 4 + 3.5f * strlen(label) : 0;
    if (!label || vb - va < 2 * half + 6) {
        th_line(l, va, y, vb, y, 1, c, LV_OPA_COVER);
        return;
    }
    float m = (va + vb) / 2;
    th_line(l, va, y, m - half, y, 1, c, LV_OPA_COVER);
    th_line(l, m + half, y, vb, y, 1, c, LV_OPA_COVER);
    th_text(l, &font_mono_11, (int)lrintf(m), y - 6, 24, LV_TEXT_ALIGN_CENTER, C_INK, LV_OPA_COVER, 0, label);
}

static void draw_ruler(lv_layer_t *l, scope_t *s)
{
    char b[8];
    float lo = s->c - s->span / 2, hi = s->c + s->span / 2;
    // Wide enough apart to number every channel? Otherwise only 1, 6, 11, 13.
    bool all = 5.0f / s->span * WF_W >= 26;
    // ISM band edges.
    const float ism[] = {2400.0f, 2483.5f};
    for (int i = 0; i < 2; i++)
        if (ism[i] > lo && ism[i] < hi)
            th_line(l, f_to_x(s, ism[i]), SPEC_TOP, f_to_x(s, ism[i]), BRACKET13_Y, 1, C_DIM, LV_OPA_COVER);
    // Channel centres 1-13 (2412 + 5(n-1)) and 14 (2484).
    for (int ch = 1; ch <= 14; ch++) {
        float f = ch_mhz(ch);
        if (f < lo || f > hi) continue;
        float x = f_to_x(s, f);
        bool main = ch == 1 || ch == 6 || ch == 11;
        th_line(l, x, RULER_Y, x, RULER_Y + (main ? 5 : 3), 1, main ? C_SUB : C_DIM, LV_OPA_COVER);
        // 1, 6 and 11 are numbered on their brackets.
        if (!main && (all || ch == 13)) {
            snprintf(b, sizeof b, "%d", ch);
            th_text(l, &font_mono_11, (int)x, NUM_Y, 20, LV_TEXT_ALIGN_CENTER, C_DIM, LV_OPA_COVER, 0, b);
        }
    }
    // 1, 6 and 11 as brackets 20 MHz wide; 13 below them, fainter, as it
    // overlaps 11.
    for (int ch = 1; ch <= 11; ch += 5) {
        snprintf(b, sizeof b, "%d", ch);
        bracket(l, s, ch_mhz(ch) - 10, ch_mhz(ch) + 10, BRACKET_Y, 4, C_TEAL, b);
    }
    bracket(l, s, ch_mhz(13) - 10, ch_mhz(13) + 10, BRACKET13_Y, 3, C_TEAL_DEEP, NULL);
    // 13 cm amateur band (UK allocation 2310 to 2450 MHz) as a thin bar,
    // labelled past its top end, or at its left when that end is off screen.
    float a0 = fmaxf(lo, 2310), a1 = fminf(hi, 2450);
    if (a1 > a0) {
        float xa = f_to_x(s, a0), xb = f_to_x(s, a1);
        if (xb + 44 < SCR_CX + 170) {
            th_text(l, &font_mono_11, (int)xb + 5, BAND_Y - 6, 40, LV_TEXT_ALIGN_LEFT, C_AMATEUR, LV_OPA_COVER, 0, "13cm");
        } else {
            float lx = fmaxf(xa, SCR_CX - 150);
            th_text(l, &font_mono_11, (int)lx, BAND_Y - 6, 40, LV_TEXT_ALIGN_LEFT, C_AMATEUR, LV_OPA_COVER, 0, "13cm");
            if (lx + 34 > xa) xa = lx + 34;
        }
        if (xb > xa) th_line(l, xa, BAND_Y, xb, BAND_Y, 2, C_AMATEUR, 200);
    }
}

static void draw_body(lv_event_t *e)
{
    scope_t *s = lv_event_get_user_data(e);
    lv_layer_t *l = lv_event_get_layer(e);
    char b[40];
    if (s->img && s->have_rows) blit(l, s);
    draw_channel_bands(l, s);
    // Trace.
    if (s->have_rows) {
        float span = fmaxf(16.0f, s->top_b - s->floor_b), px = -1, py = 0;
        for (int i = 0; i < WF_W; i += 2) {
            float v = (s->spec[i] - s->floor_b) / span;
            v = v < 0 ? 0 : v > 1 ? 1 : v;
            float x = SCR_CX - 180 + i, y = SPEC_BOT - v * (SPEC_BOT - SPEC_TOP);
            if (px >= 0) th_line(l, px, py, x, y, 1, lv_color_hex(0x8FE3E8), 220);
            px = x;
            py = y;
        }
    }
    draw_ruler(l, s);
    th_line(l, SCR_CX - 180, WF_TOP2 - 1, SCR_CX + 180, WF_TOP2 - 1, 1, C_FAINT, LV_OPA_COVER);
    // Tuning.
    float c = s->active ? s->c : s->want_c, span = s->active ? s->span : s->want_span;
    snprintf(b, sizeof b, "%.1f", c);
    th_text(l, &font_mono_22, SCR_CX - 14, 18, 160, LV_TEXT_ALIGN_CENTER, C_INK, LV_OPA_COVER, 0, b);
    th_text(l, &font_mono_11, SCR_CX + 52, 26, 40, LV_TEXT_ALIGN_LEFT, C_SUB, LV_OPA_COVER, 0, "MHz");
    snprintf(b, sizeof b, "SPAN %.0f MHz", span);
    th_text(l, &font_mono_11, SCR_CX, 48, 160, LV_TEXT_ALIGN_CENTER, C_DIM, LV_OPA_COVER, 2, b);
    // USB switched off from the system menu: a small tag at the foot.
    if (s->usb_off) {
        lv_draw_rect_dsc_t d;
        lv_draw_rect_dsc_init(&d);
        d.bg_color = C_BG;
        d.bg_opa = 215;
        d.radius = 9;
        d.border_color = C_ACCENT;
        d.border_width = 1;
        d.border_opa = 150;
        lv_area_t a = {SCR_CX - 40, 328, SCR_CX + 40, 346};
        lv_draw_rect(l, &d, &a);
        th_text(l, &font_mono_11, SCR_CX, 331, 80, LV_TEXT_ALIGN_CENTER, C_ACCENT, LV_OPA_COVER, 1, "USB OFF");
    }
    if (!s->active) {
        th_text(l, &font_mono_13, SCR_CX, 240, 260, LV_TEXT_ALIGN_CENTER, C_SUB, LV_OPA_COVER, 2,
                s->status[0] ? s->status : "STARTING RECEIVER");
        th_text(l, &font_mono_11, SCR_CX, 262, 260, LV_TEXT_ALIGN_CENTER, C_DIM, LV_OPA_COVER, 0,
                "Wi-Fi is off while the scope runs");
    }
}

TH_PAGE_DRAW(draw_cb, draw_body, (lv_obj_t *)lv_event_get_current_target(e))

static void create(page_t *p, lv_obj_t *root)
{
    scope_t *s = calloc(1, sizeof *s);
    p->state = s;
    s->want_c = 2437.0f;  // Wi-Fi channel 6
    s->span_i = SPAN_FIRST;
    s->want_span = spans[SPAN_FIRST];
    s->canvas = lv_obj_create(root);
    lv_obj_remove_style_all(s->canvas);
    lv_obj_set_size(s->canvas, SCR_W, SCR_H);
    lv_obj_remove_flag(s->canvas, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s->canvas, draw_cb, LV_EVENT_DRAW_MAIN, s);
    lv_color_format_t cf = lv_display_get_color_format(lv_display_get_default());
    s->swapped = cf == LV_COLOR_FORMAT_RGB565_SWAPPED;
    build_lut(s);
}

static void destroy(page_t *p)
{
    scope_t *s = p->state;
    if (s->img) lv_draw_buf_destroy(s->img);
    free(s);
}

static void tick(page_t *p, uint32_t now, uint32_t dt, bool visible)
{
    scope_t *s = p->state;
    s->now = now;
    if (!visible) {
        s->have_rows = false;  // a fresh picture next time
        return;
    }
    if (!s->img) {
        s->img = lv_draw_buf_create(WF_W, IMG_H2, lv_display_get_color_format(lv_display_get_default()), 0);
        if (s->img) lv_draw_buf_clear(s->img, NULL);
    }
    uint8_t rows[8][WF_W], spec[WF_W];
    int nr = 0;
    model_lock();
    g_model.sc_center_mhz_want = s->want_c;
    g_model.sc_span_mhz_want = s->want_span;
    s->active = g_model.sc_active;
    s->usb_off = g_model.sc_usb_off;
    s->c = g_model.sc_center_mhz;
    s->span = g_model.sc_span_mhz > 0 ? g_model.sc_span_mhz : s->want_span;
    s->rps = g_model.sc_rows_per_s;
    snprintf(s->status, sizeof s->status, "%s", g_model.sc_status);
    memcpy(spec, g_model.sc_spec, sizeof spec);
    uint32_t seq = g_model.sc_row_seq, from = s->seen_row;
    if (seq - from > 8) from = seq - 8;
    for (uint32_t r = from; r < seq; r++, nr++) memcpy(rows[nr], g_model.sc_rows[r % WF_ROWS], WF_W);
    model_unlock();
    s->seen_row = seq;
    if (s->img && s->active)
        for (int i = 0; i < nr; i++) add_row(s, rows[i]);
    for (int i = 0; i < WF_W; i++) s->spec[i] = th_ease(s->spec[i], spec[i], dt, 80);
    lv_obj_invalidate(s->canvas);
}

static void tap(page_t *p, int x, int y)
{
    scope_t *s = p->state;
    s->span_i = (s->span_i + 1) % (int)(sizeof spans / sizeof spans[0]);
    s->want_span = spans[s->span_i];
    if (s->span_i == 0) s->want_c = WIDE_C;  // zooming out: frame the whole Wi-Fi band
    float half = s->want_span / 2;
    if (s->want_c < F_MIN + half) s->want_c = F_MIN + half;
    if (s->want_c > F_MAX - half) s->want_c = F_MAX - half;
}

static bool knob(page_t *p, int delta)
{
    scope_t *s = p->state;
    float step = s->want_span / 8;
    s->want_c += delta * step;
    float half = s->want_span / 2;
    if (s->want_c < F_MIN + half) s->want_c = F_MIN + half;
    if (s->want_c > F_MAX - half) s->want_c = F_MAX - half;
    return true;
}

const page_type_t page_scope = {
    .type = "scope", .label = "Scope", .create = create, .destroy = destroy, .tick = tick, .tap = tap, .knob = knob,
};
