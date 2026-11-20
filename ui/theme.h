// Visual language for the desk display: black glass, hairline geometry,
// warm-white ink, one amber accent, status colours used only for status.
#pragma once

#include <math.h>
#include "lvgl.h"

#define SCR_W 360
#define SCR_H 360
// Pages draw relative to the screen centre; during a slide th_ox shifts that
// centre with the page's object, so content moves with the transition. It is
// set only inside a page's draw callback (see TH_PAGE_DRAW) and is 0 otherwise.
extern int th_ox;
#define SCR_CX (180 + th_ox)
#define SCR_CY 180

// Palette
#define C_BG     lv_color_hex(0x000000)
#define C_INK    lv_color_hex(0xEDE8DF)  // primary text, warm white
#define C_SUB    lv_color_hex(0x9A968E)  // secondary text
#define C_DIM    lv_color_hex(0x55524D)  // labels, scale numbers
#define C_FAINT  lv_color_hex(0x2A2826)  // rings, ticks
#define C_HAIR   lv_color_hex(0x161514)  // lowest-contrast structure
#define C_ACCENT lv_color_hex(0xFF9F1C)  // amber: selection, sweep, our own transmissions
#define C_OK     lv_color_hex(0x46D5A5)
#define C_WARN   lv_color_hex(0xF5D547)
#define C_CRIT   lv_color_hex(0xFF4D4D)
#define C_HEAT   lv_color_hex(0xFF7A3D)  // climate: heating
#define C_COOL   lv_color_hex(0x4CC2FF)  // climate: cooling

LV_FONT_DECLARE(font_mono_11);
LV_FONT_DECLARE(font_mono_13);
LV_FONT_DECLARE(font_mono_16);
LV_FONT_DECLARE(font_mono_22);
LV_FONT_DECLARE(font_mono_34);
LV_FONT_DECLARE(font_mono_52);
LV_FONT_DECLARE(font_sans_15);
LV_FONT_DECLARE(font_sans_20);
LV_FONT_DECLARE(font_sans_28);

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif
#define DEG2RAD(d) ((d) * (float)M_PI / 180.0f)

// Small drawing helpers used by every page's draw callback.
void th_line(lv_layer_t *l, float x1, float y1, float x2, float y2, int w, lv_color_t c, lv_opa_t opa);
void th_arc(lv_layer_t *l, int cx, int cy, int r, int w, float a0, float a1, lv_color_t c, lv_opa_t opa);
void th_tri(lv_layer_t *l, float x0, float y0, float x1, float y1, float x2, float y2, lv_color_t c, lv_opa_t opa);
// The same, for a triangle partly under an opaque disc of radius disc_r that
// will be drawn over it later: the part hidden by the disc is skipped.
void th_tri_behind(lv_layer_t *l, float x0, float y0, float x1, float y1, float x2, float y2, lv_color_t c,
                   lv_opa_t opa, int disc_cx, int disc_cy, int disc_r);
void th_dot(lv_layer_t *l, float x, float y, int r, lv_color_t c, lv_opa_t opa);
void th_ring(lv_layer_t *l, float x, float y, int r, int w, lv_color_t c, lv_opa_t opa);
void th_text(lv_layer_t *l, const lv_font_t *f, int x, int y, int w, lv_text_align_t align, lv_color_t c,
             lv_opa_t opa, int spacing, const char *txt);
// Point on a circle; angle in degrees, 0 = 12 o'clock, clockwise.
static inline float th_px(float r, float deg) { return SCR_CX + r * sinf(DEG2RAD(deg)); }
static inline float th_py(float r, float deg) { return SCR_CY - r * cosf(DEG2RAD(deg)); }

// Scenery cache: the parts of a page that do not move, drawn once into an
// offscreen screen-sized buffer and copied in as the first thing each
// frame. Pages redraw everything every frame, and on the device a straight
// copy is far cheaper than redrawing anti-aliased rings, ticks and labels.
// Only draw into it what would otherwise be drawn first, straight onto the
// black background, so the result is pixel for pixel the same.
#define TH_SC_STRIP 24
#define TH_SC_STRIPS ((SCR_H + TH_SC_STRIP - 1) / TH_SC_STRIP)
typedef struct {
    lv_draw_buf_t *buf;
    // Per band of TH_SC_STRIP rows, up to two column spans that are not all
    // black (x1 > x2 = none); only those are copied, onto the black screen.
    int16_t span[TH_SC_STRIPS][2][2];
} th_scenery_t;
// Start (re)drawing the scenery: returns a layer cleared to C_BG, or NULL if
// out of memory (then draw everything live). Finish with th_scenery_end().
lv_layer_t *th_scenery_begin(th_scenery_t *sc);
void th_scenery_end(th_scenery_t *sc, lv_layer_t *layer);
// Copy it into the page's layer (clipped to obj, as the page's own drawing is). False if there is none.
bool th_scenery_blit(lv_layer_t *l, const th_scenery_t *sc, lv_obj_t *obj);

// A big ring drawn every frame at varying opacity, cached as an image (see
// theme.c). Prepare it outside rendering (in a timer or tick, not a draw
// callback); th_ring_cached() falls back to th_ring() if it does not match.
typedef struct {
    lv_draw_buf_t *buf;
    int r, w;
    uint32_t c;
} th_ringcache_t;
void th_ring_prepare(th_ringcache_t *rc, int r, int w, lv_color_t c);
void th_ring_cached(lv_layer_t *l, const th_ringcache_t *rc, int x, int y, int r, int w, lv_color_t c, lv_opa_t opa);

// For deciding when scenery must be redrawn: a hash of the integer values the
// draw calls actually use. LVGL rounds coordinates to whole pixels, so hashing
// the rounded values catches every visible change and ignores sub-pixel creep.
static inline uint32_t th_hash(uint32_t h, int32_t v) { return (h ^ (uint32_t)v) * 16777619u; }
uint32_t th_hash_point(uint32_t h, float x, float y);  // as lv_draw_line rounds it
#define TH_HASH_SEED 2166136261u

// Wraps a page's draw body so its drawing follows the page's object during a slide.
#define TH_PAGE_DRAW(name, body_fn, obj_expr)                         \
    static void name(lv_event_t *e)                                    \
    {                                                                  \
        lv_area_t _c;                                                  \
        lv_obj_get_coords((obj_expr), &_c);                            \
        th_ox = _c.x1;                                                 \
        body_fn(e);                                                    \
        th_ox = 0;                                                     \
    }

// Eases a displayed value towards a target, frame-rate independent.
static inline float th_ease(float cur, float target, float dt_ms, float tau_ms)
{
    float k = 1.0f - expf(-dt_ms / tau_ms);
    return cur + (target - cur) * k;
}
