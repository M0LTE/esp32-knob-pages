#include "theme.h"

#include <string.h>

int th_ox;

// ---- Drawing only where the shape is ----
// LVGL's software renderer works through the whole bounding box of a line,
// arc or triangle, row by row, building coverage masks even where the shape
// is absent. For long thin shapes (a sweep wedge, a long diagonal, a short arc
// on a big circle) that box is mostly empty. These helpers draw the same shape
// several times, each time clipped to one horizontal strip narrowed to where
// the shape's outline actually is in that strip. Every pixel the shape touches
// lies in exactly one strip and gets exactly the coverage it would have had,
// so the output is identical; only the empty work is skipped. Shapes entirely
// outside the area being rendered (one band of the screen) are not queued.

#define STRIP_MIN_AREA 900   // smaller shapes get one clip, their box

typedef void (*draw_fn_t)(lv_layer_t *l, const void *dsc);

static bool intersect(lv_area_t *out, const lv_area_t *a, const lv_area_t *b)
{
    out->x1 = LV_MAX(a->x1, b->x1);
    out->y1 = LV_MAX(a->y1, b->y1);
    out->x2 = LV_MIN(a->x2, b->x2);
    out->y2 = LV_MIN(a->y2, b->y2);
    return out->x1 <= out->x2 && out->y1 <= out->y2;
}

static bool outside(const lv_layer_t *l, const lv_area_t *a)
{
    lv_area_t c;
    return !intersect(&c, a, &l->_clip_area);
}

// Shape outline as a closed polygon (xs, ys, n points); margin covers
// anti-aliasing and rounding.
// A disc that will be painted over opaquely later: pixels well inside it
// need not be drawn now.
typedef struct {
    int cx, cy, r;
} hole_t;

static void draw_stripped_x(lv_layer_t *l, const float *xs, const float *ys, int n, int margin, int strip_h,
                            int32_t xlo, int32_t xhi, const hole_t *hole, draw_fn_t fn, const void *dsc);

static void draw_stripped(lv_layer_t *l, const float *xs, const float *ys, int n, int margin, int strip_h,
                          draw_fn_t fn, const void *dsc)
{
    draw_stripped_x(l, xs, ys, n, margin, strip_h, INT32_MIN, INT32_MAX, NULL, fn, dsc);
}

// As above, with every clip also held to columns xlo..xhi, and trimmed where
// it runs into the hole from one end.
static void draw_stripped_x(lv_layer_t *l, const float *xs, const float *ys, int n, int margin, int strip_h,
                            int32_t xlo, int32_t xhi, const hole_t *hole, draw_fn_t fn, const void *dsc)
{
    float fx0 = xs[0], fx1 = xs[0], fy0 = ys[0], fy1 = ys[0];
    for (int i = 1; i < n; i++) {
        fx0 = fminf(fx0, xs[i]), fx1 = fmaxf(fx1, xs[i]);
        fy0 = fminf(fy0, ys[i]), fy1 = fmaxf(fy1, ys[i]);
    }
    lv_area_t box = {LV_MAX((int32_t)floorf(fx0) - margin, xlo), (int32_t)floorf(fy0) - margin,
                     LV_MIN((int32_t)ceilf(fx1) + margin, xhi), (int32_t)ceilf(fy1) + margin};
    lv_area_t clip;
    if (!intersect(&clip, &box, &l->_clip_area)) return;
    const lv_area_t saved = l->_clip_area;
    if (lv_area_get_size(&box) < STRIP_MIN_AREA) {
        l->_clip_area = clip;
        fn(l, dsc);
        l->_clip_area = saved;
        return;
    }
    for (int32_t y0 = clip.y1; y0 <= clip.y2; y0 += strip_h) {
        int32_t y1 = y0 + strip_h - 1;
        if (y1 > clip.y2) y1 = clip.y2;
        // Outline points within the strip (widened by the margin): clip each edge to it.
        float lo = y0 - margin, hi = y1 + margin, xmin = 1e9f, xmax = -1e9f;
        for (int i = 0; i < n; i++) {
            float ax = xs[i], ay = ys[i], bx = xs[(i + 1) % n], by = ys[(i + 1) % n];
            if ((ay < lo && by < lo) || (ay > hi && by > hi)) continue;
            float t0 = 0, t1 = 1;
            if (ay != by) {
                float ta = (lo - ay) / (by - ay), tb = (hi - ay) / (by - ay);
                if (ta > tb) { float t = ta; ta = tb; tb = t; }
                t0 = fmaxf(0, ta), t1 = fminf(1, tb);
                if (t0 > t1) continue;
            }
            float xa = ax + (bx - ax) * t0, xb = ax + (bx - ax) * t1;
            xmin = fminf(xmin, fminf(xa, xb));
            xmax = fmaxf(xmax, fmaxf(xa, xb));
        }
        if (xmin > xmax) continue;
        lv_area_t strip = {LV_MAX((int32_t)floorf(xmin) - margin, xlo), y0, LV_MIN((int32_t)ceilf(xmax) + margin, xhi),
                           y1}, c;
        if (hole) {
            // Columns every row of this strip has well inside the hole.
            int32_t dy = LV_MAX(LV_ABS(y0 - hole->cy), LV_ABS(y1 - hole->cy)) + 1;
            if (dy < hole->r) {
                int32_t hw = (int32_t)sqrtf((float)(hole->r * hole->r - dy * dy)) - 1;
                int32_t h0 = hole->cx - hw, h1 = hole->cx + hw;
                if (h0 <= strip.x1 && h1 >= strip.x2) continue;      // all hidden
                if (h0 <= strip.x1 && h1 >= strip.x1) strip.x1 = h1 + 1;
                else if (h1 >= strip.x2 && h0 <= strip.x2) strip.x2 = h0 - 1;
            }
        }
        if (!intersect(&c, &strip, &saved)) continue;
        l->_clip_area = c;
        fn(l, dsc);
    }
    l->_clip_area = saved;
}

static void line_fn(lv_layer_t *l, const void *d) { lv_draw_line(l, d); }
static void arc_fn(lv_layer_t *l, const void *d) { lv_draw_arc(l, d); }
static void tri_fn(lv_layer_t *l, const void *d) { lv_draw_triangle(l, d); }

void th_line(lv_layer_t *l, float x1, float y1, float x2, float y2, int w, lv_color_t c, lv_opa_t opa)
{
    lv_draw_line_dsc_t d;
    lv_draw_line_dsc_init(&d);
    d.p1.x = x1;
    d.p1.y = y1;
    d.p2.x = x2;
    d.p2.y = y2;
    d.width = w;
    d.color = c;
    d.opa = opa;
    if (opa <= LV_OPA_MIN || w == 0) return;
    // Horizontal and vertical lines are cheap already: just skip them when out of view.
    int m = w + 2;
    float xs[2] = {x1, x2}, ys[2] = {y1, y2};
    if ((int32_t)x1 == (int32_t)x2 || (int32_t)y1 == (int32_t)y2) {
        lv_area_t a = {(int32_t)fminf(x1, x2) - m, (int32_t)fminf(y1, y2) - m, (int32_t)fmaxf(x1, x2) + m,
                       (int32_t)fmaxf(y1, y2) + m};
        if (!outside(l, &a)) lv_draw_line(l, &d);
        return;
    }
    draw_stripped(l, xs, ys, 2, m, 8, line_fn, &d);
}

// a0/a1 in our convention (0 = 12 o'clock, clockwise); LVGL's 0 is 3 o'clock.
// A ring (annulus) drawn as its left and right halves, in columns that do not
// overlap, each in strips: LVGL's ring and circle-border drawing otherwise
// works through every row of the whole square.
static void draw_ring_halves(lv_layer_t *l, float cx, float cy, float ro, float ri, int strip_h, draw_fn_t fn,
                             const void *dsc)
{
    float xs[2 * 46], ys[2 * 46];
    int32_t split = (int32_t)cx;
    for (int side = 0; side < 2; side++) {
        int n = 0;
        float b = side ? -90 : 90;  // LVGL angles, 0 = 3 o'clock: the left half is 90..270
        for (int k = 0; k <= 45; k++) {
            float a = DEG2RAD(b + 4.0f * k);
            xs[n] = cx + ro * cosf(a), ys[n++] = cy + ro * sinf(a);
        }
        for (int k = 45; k >= 0; k--) {
            float a = DEG2RAD(b + 4.0f * k);
            xs[n] = cx + ri * cosf(a), ys[n++] = cy + ri * sinf(a);
        }
        draw_stripped_x(l, xs, ys, n, 3, strip_h, side ? split : INT32_MIN, side ? INT32_MAX : split - 1, NULL, fn, dsc);
    }
}

void th_arc(lv_layer_t *l, int cx, int cy, int r, int w, float a0, float a1, lv_color_t c, lv_opa_t opa)
{
    if (opa <= LV_OPA_MIN || r <= 0) return;
    lv_draw_arc_dsc_t d;
    lv_draw_arc_dsc_init(&d);
    d.center.x = cx;
    d.center.y = cy;
    d.radius = r;
    d.width = w;
    // LVGL clamps negative angles to 0; bring both into 0..360 (a full turn stays 0..360).
    if (a1 - a0 >= 360) {
        d.start_angle = 0;
        d.end_angle = 360;
    } else {
        d.start_angle = fmodf(a0 - 90 + 720.0f, 360.0f);
        d.end_angle = fmodf(a1 - 90 + 720.0f, 360.0f);
    }
    d.color = c;
    d.opa = opa;
    if (w == 0 || d.start_angle == d.end_angle) return;
    lv_area_t box = {cx - r - 1, cy - r - 1, cx + r + 1, cy + r + 1};
    if (outside(l, &box)) return;
    // The angles exactly as LVGL's software renderer settles them: truncated,
    // brought under 360, then clamped to 0..359.
    int32_t s0 = (int32_t)d.start_angle, s1 = (int32_t)d.end_angle;
    while (s0 >= 360) s0 -= 360;
    while (s1 >= 360) s1 -= 360;
    s0 = s0 < 0 ? 0 : s0 > 359 ? 359 : s0;
    s1 = s1 < 0 ? 0 : s1 > 359 ? 359 : s1;
    bool ring = d.start_angle + 360 == d.end_angle || d.start_angle == d.end_angle + 360;
    if (ring || s0 == s1) {
        // A whole ring (LVGL also draws one when the clamping leaves s0 == s1).
        draw_ring_halves(l, cx, cy, r + 1.0f, fmaxf(0, r - w - 1.0f), 8, arc_fn, &d);
        return;
    }
    if (s1 < s0) s1 += 360;
    // Outline: the outer edge from s0 to s1, then the inner edge back, a degree
    // wider each side for the anti-aliased ends.
    enum { MAXP = 2 * 95 + 4 };
    float xs[MAXP], ys[MAXP];
    int span = s1 - s0 + 2, steps = span / 4 + 1, n = 0;
    s0 -= 1;
    float ro = r + 1.0f, ri = fmaxf(0, r - w - 1.0f);
    for (int k = 0; k <= steps; k++) {
        float a = DEG2RAD(s0 + (float)span * k / steps);
        xs[n] = cx + ro * cosf(a), ys[n++] = cy + ro * sinf(a);
    }
    for (int k = steps; k >= 0; k--) {
        float a = DEG2RAD(s0 + (float)span * k / steps);
        xs[n] = cx + ri * cosf(a), ys[n++] = cy + ri * sinf(a);
    }
    // Chords cut inside the circle by at most r * (1 - cos 2 deg), about 0.1 px at r = 180.
    draw_stripped(l, xs, ys, n, 3, 8, arc_fn, &d);
}

void th_dot(lv_layer_t *l, float x, float y, int r, lv_color_t c, lv_opa_t opa)
{
    if (opa <= LV_OPA_MIN) return;
    lv_draw_rect_dsc_t d;
    lv_draw_rect_dsc_init(&d);
    d.radius = LV_RADIUS_CIRCLE;
    d.bg_color = c;
    d.bg_opa = opa;
    lv_area_t a = {(int32_t)(x - r), (int32_t)(y - r), (int32_t)(x + r), (int32_t)(y + r)};
    if (outside(l, &a)) return;
    lv_draw_rect(l, &d, &a);
}

void th_ring(lv_layer_t *l, float x, float y, int r, int w, lv_color_t c, lv_opa_t opa)
{
    if (opa <= LV_OPA_MIN) return;
    lv_draw_rect_dsc_t d;
    lv_draw_rect_dsc_init(&d);
    d.radius = LV_RADIUS_CIRCLE;
    d.bg_opa = LV_OPA_TRANSP;
    d.border_color = c;
    d.border_width = w;
    d.border_opa = opa;
    lv_area_t a = {(int32_t)(x - r), (int32_t)(y - r), (int32_t)(x + r), (int32_t)(y + r)};
    if (outside(l, &a)) return;
    lv_draw_rect(l, &d, &a);
}

// Big rings drawn every frame: LVGL's circle border works row by row through
// the whole square with two radius masks, several milliseconds per ring per
// band. Instead the ring's coverage is drawn once into an A8 mask, and each
// frame that mask is blended in at the wanted colour and opacity, only in
// strips along the ring.
static void render_now(lv_layer_t *l)
{
    lv_display_t *d = lv_display_get_default();
    while (l->draw_task_head) {
        lv_draw_dispatch_wait_for_request();
        if (!lv_draw_dispatch_layer(d, l)) {
            lv_draw_wait_for_finish();
            lv_draw_dispatch_request();
        }
    }
}

void th_ring_prepare(th_ringcache_t *rc, int r, int w, lv_color_t c)
{
    uint32_t c32 = lv_color_to_u32(c);
    if (rc->buf && rc->r == r && rc->w == w && rc->c == c32) return;
    if (rc->buf) lv_draw_buf_destroy(rc->buf);
    rc->buf = NULL;
    // Draw it white on transparent ARGB8888; the alpha is the coverage.
    int n = 2 * r + 1;
    lv_draw_buf_t *argb = lv_draw_buf_create(n, n, LV_COLOR_FORMAT_ARGB8888, 0);
    lv_draw_buf_t *a8 = lv_draw_buf_create(n, n, LV_COLOR_FORMAT_A8, 0);
    if (!argb || !a8) {
        if (argb) lv_draw_buf_destroy(argb);
        if (a8) lv_draw_buf_destroy(a8);
        return;
    }
    lv_draw_buf_clear(argb, NULL);
    lv_layer_t l;
    lv_layer_init(&l);
    lv_area_t a = {0, 0, n - 1, n - 1};
    l.draw_buf = argb;
    l.color_format = LV_COLOR_FORMAT_ARGB8888;
    l.buf_area = a;
    l._clip_area = a;
    l.phy_clip_area = a;
    lv_draw_rect_dsc_t d;
    lv_draw_rect_dsc_init(&d);
    d.radius = LV_RADIUS_CIRCLE;
    d.bg_opa = LV_OPA_TRANSP;
    d.border_color = lv_color_white();
    d.border_width = w;
    d.border_opa = LV_OPA_COVER;
    lv_draw_rect(&l, &d, &a);
    render_now(&l);
    // Keep only the alpha, as an A8 mask: LVGL blends an A8 image as its
    // recolour through that mask, the same way it blends a border.
    for (int y = 0; y < n; y++) {
        const uint8_t *src = argb->data + y * argb->header.stride;
        uint8_t *dst = a8->data + y * a8->header.stride;
        for (int x = 0; x < n; x++) dst[x] = src[4 * x + 3];
    }
    lv_draw_buf_destroy(argb);
    rc->buf = a8;
    lv_image_cache_drop(rc->buf);
    rc->r = r, rc->w = w, rc->c = c32;
}

typedef struct {
    lv_draw_image_dsc_t d;
    lv_area_t a;
} image_job_t;

static void image_fn(lv_layer_t *l, const void *p)
{
    const image_job_t *j = p;
    lv_draw_image(l, &j->d, &j->a);
}

void th_ring_cached(lv_layer_t *l, const th_ringcache_t *rc, int x, int y, int r, int w, lv_color_t c, lv_opa_t opa)
{
    if (!rc->buf || rc->r != r || rc->w != w || rc->c != lv_color_to_u32(c)) {
        th_ring(l, x, y, r, w, c, opa);
        return;
    }
    if (opa <= LV_OPA_MIN) return;
    image_job_t j;
    lv_draw_image_dsc_init(&j.d);
    j.d.src = rc->buf;
    j.d.opa = opa;
    j.d.recolor = c;
    j.a = (lv_area_t){x - r, y - r, x + r, y + r};
    if (outside(l, &j.a)) return;
    // Image strips are cheap to set up, so fewer, taller ones.
    draw_ring_halves(l, x, y, r + 1.5f, fmaxf(0, r - w - 1.5f), 40, image_fn, &j);
}

void th_text(lv_layer_t *l, const lv_font_t *f, int x, int y, int w, lv_text_align_t align, lv_color_t c,
             lv_opa_t opa, int spacing, const char *txt)
{
    if (opa <= LV_OPA_MIN) return;
    lv_draw_label_dsc_t d;
    lv_draw_label_dsc_init(&d);
    d.font = f;
    d.color = c;
    d.opa = opa;
    d.letter_space = spacing;
    d.align = align;
    d.text = txt;
    d.text_local = 1;
    int x0 = x;
    if (align == LV_TEXT_ALIGN_CENTER) x0 = x - w / 2;
    else if (align == LV_TEXT_ALIGN_RIGHT) x0 = x - w;
    lv_area_t a = {x0, y, x0 + w - 1, y + lv_font_get_line_height(f)};
    if (outside(l, &a)) return;  // LVGL draws nothing of a label whose box is out of view
    lv_draw_label(l, &d, &a);
}

static lv_layer_t s_scenery_layer;

lv_layer_t *th_scenery_begin(th_scenery_t *sc)
{
    // Same pixel format as the display (the device renders RGB565 in the
    // panel's byte order), so the copy each frame is a plain memcpy.
    lv_color_format_t cf = lv_display_get_color_format(lv_display_get_default());
    if (sc->buf && sc->buf->header.cf != cf) {
        lv_draw_buf_destroy(sc->buf);
        sc->buf = NULL;
    }
    if (!sc->buf) {
        sc->buf = lv_draw_buf_create(SCR_W, SCR_H, cf, 0);
        if (!sc->buf) return NULL;
    }
    lv_draw_buf_clear(sc->buf, NULL);  // C_BG is black, all zero bits in either byte order
    lv_layer_t *l = &s_scenery_layer;
    lv_layer_init(l);
    lv_area_t a = {0, 0, SCR_W - 1, SCR_H - 1};
    l->draw_buf = sc->buf;
    l->color_format = cf;
    l->buf_area = a;
    l->_clip_area = a;
    l->phy_clip_area = a;
    return l;
}

void th_scenery_end(th_scenery_t *sc, lv_layer_t *l)
{
    // Render it now (the same loop lv_canvas_finish_layer uses).
    lv_display_t *d = lv_display_get_default();
    while (l->draw_task_head) {
        lv_draw_dispatch_wait_for_request();
        if (!lv_draw_dispatch_layer(d, l)) {
            lv_draw_wait_for_finish();
            lv_draw_dispatch_request();
        }
    }
    lv_image_cache_drop(sc->buf);

    // Find where it is not black, band by band: copying black onto the black
    // background changes nothing, so those parts are skipped.
    uint32_t stride = sc->buf->header.stride;
    for (int b = 0; b < TH_SC_STRIPS; b++) {
        uint8_t used[SCR_W];
        memset(used, 0, sizeof used);
        int y0 = b * TH_SC_STRIP, y1 = LV_MIN(y0 + TH_SC_STRIP, SCR_H);
        for (int y = y0; y < y1; y++) {
            const uint16_t *row = (const uint16_t *)(sc->buf->data + y * stride);
            for (int x = 0; x < SCR_W; x++) used[x] |= row[x] != 0;
        }
        int x1 = 0, x2 = SCR_W - 1;
        while (x1 < SCR_W && !used[x1]) x1++;
        while (x2 >= 0 && !used[x2]) x2--;
        // The widest black gap inside, if worth splitting at.
        int gs = -1, ge = -1;
        for (int x = x1; x <= x2;) {
            if (used[x]) { x++; continue; }
            int e = x;
            while (e <= x2 && !used[e]) e++;
            if (e - x > ge - gs) gs = x, ge = e;
            x = e;
        }
        int16_t (*sp)[2] = sc->span[b];
        if (x1 > x2) {
            sp[0][0] = sp[1][0] = 1, sp[0][1] = sp[1][1] = 0;
        } else if (gs >= 0 && ge - gs >= 32) {
            sp[0][0] = x1, sp[0][1] = gs - 1, sp[1][0] = ge, sp[1][1] = x2;
        } else {
            sp[0][0] = x1, sp[0][1] = x2, sp[1][0] = 1, sp[1][1] = 0;
        }
    }
}

bool th_scenery_blit(lv_layer_t *l, const th_scenery_t *sc, lv_obj_t *obj)
{
    if (!sc->buf) return false;
    lv_draw_image_dsc_t d;
    lv_draw_image_dsc_init(&d);
    d.src = sc->buf;
    // The scenery was drawn with th_ox = 0; place it where the page is now.
    (void)obj;
    lv_area_t a = {th_ox, 0, th_ox + SCR_W - 1, SCR_H - 1};
    const lv_area_t saved = l->_clip_area;
    for (int b = 0; b < TH_SC_STRIPS; b++) {
        for (int k = 0; k < 2; k++) {
            const int16_t *sp = sc->span[b][k];
            if (sp[0] > sp[1]) continue;
            lv_area_t part = {sp[0] + th_ox, b * TH_SC_STRIP, sp[1] + th_ox, LV_MIN((b + 1) * TH_SC_STRIP, SCR_H) - 1}, c;
            if (!intersect(&c, &part, &saved)) continue;
            l->_clip_area = c;
            lv_draw_image(l, &d, &a);
        }
    }
    l->_clip_area = saved;
    return true;
}

uint32_t th_hash_point(uint32_t h, float x, float y)
{
    lv_point_precise_t pp = {x, y};
    lv_point_t p = lv_point_from_precise(&pp);
    return th_hash(th_hash(h, p.x), p.y);
}

void th_tri_behind(lv_layer_t *l, float x0, float y0, float x1, float y1, float x2, float y2, lv_color_t c,
                   lv_opa_t opa, int disc_cx, int disc_cy, int disc_r)
{
    if (opa <= LV_OPA_MIN) return;
    lv_draw_triangle_dsc_t d;
    lv_draw_triangle_dsc_init(&d);
    d.p[0].x = x0;
    d.p[0].y = y0;
    d.p[1].x = x1;
    d.p[1].y = y1;
    d.p[2].x = x2;
    d.p[2].y = y2;
    d.color = c;
    d.opa = opa;
    // LVGL rounds the corners to whole pixels before drawing.
    float xs[3], ys[3];
    for (int i = 0; i < 3; i++) {
        lv_point_t p = lv_point_from_precise(&d.p[i]);
        xs[i] = p.x, ys[i] = p.y;
    }
    // The disc's own edge is anti-aliased: only trust it 2 px inside.
    hole_t h = {disc_cx, disc_cy, disc_r - 2};
    draw_stripped_x(l, xs, ys, 3, 1, 6, INT32_MIN, INT32_MAX, disc_r > 2 ? &h : NULL, tri_fn, &d);
}

void th_tri(lv_layer_t *l, float x0, float y0, float x1, float y1, float x2, float y2, lv_color_t c, lv_opa_t opa)
{
    th_tri_behind(l, x0, y0, x1, y1, x2, y2, c, opa, 0, 0, 0);
}
