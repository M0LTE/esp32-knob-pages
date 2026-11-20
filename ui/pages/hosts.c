// Network page: every monitored host as a blip on a polar plot. Angle is just
// the host's slot; distance from the centre is round-trip time on a log scale
// (0.1 ms near the middle, 1 s at the edge); hosts that stopped answering sit on
// the outer ring in red. A sweep circles the plot and each blip flashes as the
// sweep passes it. Tap to step through hosts; the centre opens into a readout.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../model.h"
#include "../page.h"
#include "../theme.h"

#define R_DOWN 152
#define R_MAX 140
#define R_MIN_IDLE 44
#define R_MIN_OPEN 92
#define SWEEP_DEG_PER_S 72.0f
#define SELECT_HOLD_MS 12000

typedef struct {
    float r;            // displayed radius, eased
    float rtt;          // displayed rtt, eased
    uint32_t flash_ms;  // when the sweep last crossed it
} blip_t;

typedef struct {
    lv_obj_t *canvas;
    float sweep;        // degrees
    float r_min;        // centre disc radius, eased between idle and open
    int sel;            // -1 = summary
    uint32_t sel_ms;
    uint32_t now;
    uint32_t shown_ms;  // when the page last came into view, for the fading title
    bool was_visible;
    blip_t blips[MODEL_MAX_HOSTS];
    host_t snap[MODEL_MAX_HOSTS];  // copied under the lock each frame
    int n;
    th_scenery_t scenery;          // draw_scale(), for the inputs hashed in sc_key
    uint32_t sc_key;
    th_ringcache_t disc_ring;
} hosts_t;

static float host_angle(int i, int n) { return (i + 0.5f) * 360.0f / (n ? n : 1); }

static float rtt_radius(const hosts_t *s, float rtt)
{
    float lg = (log10f(rtt < 0.1f ? 0.1f : rtt) + 1.0f) / 4.0f;  // 0.1 ms .. 1 s
    if (lg > 1) lg = 1;
    return s->r_min + (R_MAX - s->r_min) * lg;
}

static bool host_slow(const host_t *h) { return h->state == HOST_UP && (h->rtt_ms >= 150 || h->loss_pct >= 5); }

static lv_color_t host_colour(const host_t *h)
{
    if (h->state == HOST_DOWN) return C_CRIT;
    if (h->state == HOST_UNKNOWN) return C_DIM;
    return host_slow(h) ? C_WARN : C_OK;
}

static void draw_scale(lv_layer_t *l, hosts_t *s)
{
    // Rim: 120 ticks, every tenth longer.
    for (int i = 0; i < 120; i++) {
        float a = i * 3.0f;
        if (a > 156 && a < 204) continue;  // leave room for the page pips
        int major = i % 10 == 0;
        th_line(l, th_px(major ? 165 : 169, a), th_py(major ? 165 : 169, a), th_px(174, a), th_py(174, a), 1,
                major ? C_DIM : C_FAINT, LV_OPA_COVER);
    }
    // Decade rings and their labels, along the 12 o'clock axis.
    static const char *lab[] = {"", "1ms", "10", "100", "1s"};
    for (int d = 0; d <= 4; d++) {
        float r = s->r_min + (R_MAX - s->r_min) * d / 4.0f;
        th_ring(l, SCR_CX, SCR_CY, (int)r, 1, d == 4 ? C_DIM : C_FAINT, LV_OPA_COVER);
        float open = (s->r_min - R_MIN_IDLE) / (R_MIN_OPEN - R_MIN_IDLE);
        if (d > 0) th_text(l, &font_mono_11, SCR_CX + 4, (int)(SCR_CY - r + 1), 40, LV_TEXT_ALIGN_LEFT, C_DIM,
                           (lv_opa_t)(255 * (1 - open)), 0, lab[d]);
    }
    // The "lost" ring outside the scale, drawn as a dotted circle.
    for (int i = 0; i < 90; i++) th_dot(l, th_px(R_DOWN, i * 4.0f), th_py(R_DOWN, i * 4.0f), 1, C_FAINT, LV_OPA_COVER);
    // Spokes at each host slot, very faint.
    for (int i = 0; i < s->n; i++) {
        float a = host_angle(i, s->n);
        th_line(l, th_px(s->r_min, a), th_py(s->r_min, a), th_px(R_MAX, a), th_py(R_MAX, a), 1, C_HAIR, LV_OPA_COVER);
    }
}

// Everything draw_scale() takes from the state, as the draw calls round it.
static uint32_t scale_key(const hosts_t *s)
{
    uint32_t h = th_hash(TH_HASH_SEED, s->n);
    float open = (s->r_min - R_MIN_IDLE) / (R_MIN_OPEN - R_MIN_IDLE);
    h = th_hash(h, (lv_opa_t)(255 * (1 - open)));
    for (int d = 0; d <= 4; d++) {
        float r = s->r_min + (R_MAX - s->r_min) * d / 4.0f;
        h = th_hash(th_hash(h, (int)r), (int)(SCR_CY - r + 1));
    }
    for (int i = 0; i < s->n; i++) {
        float a = host_angle(i, s->n);
        h = th_hash_point(th_hash_point(h, th_px(s->r_min, a), th_py(s->r_min, a)), th_px(R_MAX, a), th_py(R_MAX, a));
    }
    return h;
}

static void draw_sweep(lv_layer_t *l, hosts_t *s)
{
    const int segs = 14;
    const float span = 4.0f;
    float r1 = R_MAX;
    for (int k = 0; k < segs; k++) {
        float a1 = s->sweep - k * span, a0 = a1 - span;
        lv_opa_t o = (lv_opa_t)(46 * (1.0f - (float)k / segs) * (1.0f - (float)k / segs));
        if (o < 2) continue;
        // One wedge as a single triangle from the centre: the part inside the
        // centre disc is painted over by the disc, so only the part between
        // the disc and the scale shows. (Half the work of drawing the visible
        // part as two triangles, and no seam down the middle.)
        th_tri_behind(l, SCR_CX, SCR_CY, th_px(r1, a0), th_py(r1, a0), th_px(r1, a1), th_py(r1, a1), C_ACCENT, o,
                      SCR_CX, SCR_CY, (int)s->r_min);
    }
    th_line(l, th_px(s->r_min, s->sweep), th_py(s->r_min, s->sweep), th_px(R_MAX, s->sweep), th_py(R_MAX, s->sweep), 2,
            C_ACCENT, 200);
}

static void draw_blips(lv_layer_t *l, hosts_t *s)
{
    for (int i = 0; i < s->n; i++) {
        const host_t *h = &s->snap[i];
        blip_t *b = &s->blips[i];
        float a = host_angle(i, s->n);
        float x = th_px(b->r, a), y = th_py(b->r, a);
        lv_color_t c = host_colour(h);

        uint32_t since = s->now - b->flash_ms;
        if (since < 900) {
            float k = since / 900.0f;
            th_ring(l, x, y, (int)(5 + 16 * k), 1, c, (lv_opa_t)(200 * (1 - k)));
        }
        if (h->state == HOST_DOWN) {
            float k = (s->now % 1200) / 1200.0f;
            th_ring(l, x, y, (int)(6 + 10 * k), 2, C_CRIT, (lv_opa_t)(160 * (1 - k)));
        }
        th_dot(l, x, y, i == s->sel ? 5 : 4, c, LV_OPA_COVER);

        // Name beside the blip: outward, unless that would run off the glass.
        bool right = a < 180;
        if (fabsf(x - SCR_CX) > 100) right = !right;
        bool dimmed = s->sel >= 0 && i != s->sel;
        char name[20];
        snprintf(name, sizeof name, "%s", h->name);
        for (char *p = name; *p; p++) if (*p >= 'a' && *p <= 'z') *p -= 32;
        float open = (s->r_min - R_MIN_IDLE) / (R_MIN_OPEN - R_MIN_IDLE);
        lv_opa_t lo = dimmed ? (lv_opa_t)(255 * (1 - open)) : LV_OPA_COVER;  // only the chosen host is named
        th_text(l, &font_mono_11, (int)(right ? x + 9 : x - 9), (int)(y - 7), 90,
                right ? LV_TEXT_ALIGN_LEFT : LV_TEXT_ALIGN_RIGHT, h->state == HOST_DOWN ? C_CRIT : C_SUB, lo, 0,
                name);

        if (i == s->sel) {
            for (int q = 0; q < 4; q++) th_arc(l, (int)x, (int)y, 11, 2, q * 90 + 20, q * 90 + 70, C_ACCENT, LV_OPA_COVER);
            th_line(l, th_px(s->r_min + 2, a), th_py(s->r_min + 2, a), th_px(b->r - 12, a), th_py(b->r - 12, a), 1,
                    C_ACCENT, 140);
        }
    }
}

static void draw_sparkline(lv_layer_t *l, const host_t *h, int x0, int y0, int w, int hgt)
{
    th_line(l, x0, y0 + hgt, x0 + w, y0 + hgt, 1, C_FAINT, LV_OPA_COVER);
    if (h->hist_n < 2) return;
    float mx = 1;
    for (int i = 0; i < h->hist_n; i++) if (h->hist[i] > mx) mx = h->hist[i];
    float step = (float)w / (HOST_HIST - 1);
    float px = -1, py = 0;
    for (int i = 0; i < h->hist_n; i++) {
        float x = x0 + w - (h->hist_n - 1 - i) * step;
        if (h->hist[i] < 0) {
            th_line(l, x, y0 + hgt - 5, x, y0 + hgt + 1, 2, C_CRIT, LV_OPA_COVER);
            px = -1;
            continue;
        }
        float y = y0 + hgt - 1 - (hgt - 2) * (h->hist[i] / mx);
        if (px >= 0) th_line(l, px, py, x, y, 1, C_ACCENT, LV_OPA_COVER);
        px = x;
        py = y;
    }
}

static void draw_centre(lv_layer_t *l, hosts_t *s)
{
    // The disc itself: black with a hairline edge, so the sweep never crosses it.
    th_dot(l, SCR_CX, SCR_CY, (int)s->r_min, C_BG, LV_OPA_COVER);
    th_ring_cached(l, &s->disc_ring, SCR_CX, SCR_CY, (int)s->r_min, 1, s->sel >= 0 ? C_ACCENT : C_DIM,
                   s->sel >= 0 ? 160 : LV_OPA_COVER);

    float open = (s->r_min - R_MIN_IDLE) / (R_MIN_OPEN - R_MIN_IDLE);  // 0..1
    char buf[48];
    if (s->sel < 0 || open < 0.6f) {
        int up = 0, known = 0;
        for (int i = 0; i < s->n; i++) {
            up += s->snap[i].state == HOST_UP;
            known += s->snap[i].state != HOST_UNKNOWN;
        }
        lv_opa_t o = (lv_opa_t)(255 * (1 - open));
        snprintf(buf, sizeof buf, "%d/%d", up, s->n);
        th_text(l, &font_mono_22, SCR_CX, SCR_CY - 17, 80, LV_TEXT_ALIGN_CENTER,
                up == s->n ? C_INK : (known && up < known ? C_CRIT : C_INK), o, 0, buf);
        th_text(l, &font_mono_11, SCR_CX, SCR_CY + 8, 80, LV_TEXT_ALIGN_CENTER, C_DIM, o, 2, "UP");
        return;
    }
    const host_t *h = &s->snap[s->sel];
    const blip_t *b = &s->blips[s->sel];
    lv_opa_t o = (lv_opa_t)(255 * (open - 0.6f) / 0.4f);
    th_text(l, &font_sans_20, SCR_CX, SCR_CY - 66, 160, LV_TEXT_ALIGN_CENTER, C_INK, o, 1, h->name);
    th_text(l, &font_mono_11, SCR_CX, SCR_CY - 42, 170, LV_TEXT_ALIGN_CENTER, C_DIM, o, 0, h->addr);
    if (h->state == HOST_DOWN) {
        th_text(l, &font_mono_22, SCR_CX, SCR_CY - 22, 160, LV_TEXT_ALIGN_CENTER, C_CRIT, o, 2, "NO REPLY");
        uint32_t secs = (s->now - h->changed_ms) / 1000;
        if (secs < 120) snprintf(buf, sizeof buf, "for %us", (unsigned)secs);
        else snprintf(buf, sizeof buf, "for %um", (unsigned)(secs / 60));
        th_text(l, &font_mono_13, SCR_CX, SCR_CY + 6, 160, LV_TEXT_ALIGN_CENTER, C_SUB, o, 0, buf);
    } else {
        if (b->rtt < 10) snprintf(buf, sizeof buf, "%.2f", b->rtt);
        else if (b->rtt < 100) snprintf(buf, sizeof buf, "%.1f", b->rtt);
        else snprintf(buf, sizeof buf, "%.0f", b->rtt);
        th_text(l, &font_mono_34, SCR_CX - 8, SCR_CY - 28, 120, LV_TEXT_ALIGN_CENTER, C_INK, o, 0, buf);
        th_text(l, &font_mono_13, SCR_CX + 56, SCR_CY - 9, 30, LV_TEXT_ALIGN_LEFT, C_SUB, o, 0, "ms");
        snprintf(buf, sizeof buf, "LOSS %u%%", h->loss_pct);
        th_text(l, &font_mono_11, SCR_CX, SCR_CY + 12, 160, LV_TEXT_ALIGN_CENTER,
                h->loss_pct ? C_WARN : C_DIM, o, 1, buf);
    }
    draw_sparkline(l, h, SCR_CX - 62, SCR_CY + 32, 124, 24);
}

static void draw_body(lv_event_t *e)
{
    hosts_t *s = lv_event_get_user_data(e);
    lv_layer_t *l = lv_event_get_layer(e);
    if (!th_scenery_blit(l, &s->scenery, s->canvas)) draw_scale(l, s);
    draw_sweep(l, s);
    draw_blips(l, s);
    draw_centre(l, s);
    // Title only while the page is fresh, so it never sits on top of a host.
    uint32_t age = s->now - s->shown_ms;
    if (age < 3500) {
        lv_opa_t o = age < 2500 ? LV_OPA_COVER : (lv_opa_t)(255 * (3500 - age) / 1000);
        th_text(l, &font_sans_15, SCR_CX, 22, 160, LV_TEXT_ALIGN_CENTER, C_SUB, o, 4, "NETWORK");
    }
}

static void draw_cb(lv_event_t *e);

static void create(page_t *p, lv_obj_t *root)
{
    hosts_t *s = calloc(1, sizeof *s);
    p->state = s;
    s->sel = -1;
    s->r_min = R_MIN_IDLE;
    s->canvas = lv_obj_create(root);
    lv_obj_remove_style_all(s->canvas);
    lv_obj_set_size(s->canvas, SCR_W, SCR_H);
    lv_obj_remove_flag(s->canvas, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s->canvas, draw_cb, LV_EVENT_DRAW_MAIN, s);
    for (int i = 0; i < MODEL_MAX_HOSTS; i++) s->blips[i].r = R_MAX;
}

TH_PAGE_DRAW(draw_cb, draw_body, (lv_obj_t *)lv_event_get_current_target(e))

static void destroy(page_t *p)
{
    hosts_t *s = p->state;
    if (s->scenery.buf) lv_draw_buf_destroy(s->scenery.buf);
    if (s->disc_ring.buf) lv_draw_buf_destroy(s->disc_ring.buf);
    free(s);
}

static void tick(page_t *p, uint32_t now, uint32_t dt, bool visible)
{
    hosts_t *s = p->state;
    s->now = now;
    model_lock();
    s->n = g_model.n_hosts;
    memcpy(s->snap, g_model.hosts, sizeof(host_t) * s->n);
    model_unlock();

    if (visible && !s->was_visible) s->shown_ms = now;
    s->was_visible = visible;
    float prev = s->sweep;
    s->sweep = fmodf(s->sweep + SWEEP_DEG_PER_S * dt / 1000.0f, 360.0f);
    if (s->sel >= 0 && now - s->sel_ms > SELECT_HOLD_MS) s->sel = -1;
    s->r_min = th_ease(s->r_min, s->sel >= 0 ? R_MIN_OPEN : R_MIN_IDLE, dt, 120);

    for (int i = 0; i < s->n; i++) {
        const host_t *h = &s->snap[i];
        blip_t *b = &s->blips[i];
        float target = h->state == HOST_DOWN ? R_DOWN : h->state == HOST_UNKNOWN ? R_MAX : rtt_radius(s, h->rtt_ms);
        b->r = th_ease(b->r, target, dt, 250);
        b->rtt = th_ease(b->rtt, h->rtt_ms, dt, 200);
        float a = host_angle(i, s->n);
        bool crossed = prev <= s->sweep ? (a > prev && a <= s->sweep) : (a > prev || a <= s->sweep);
        if (crossed) b->flash_ms = now;
    }
    if (visible) {
        uint32_t key = scale_key(s);
        if (!s->scenery.buf || key != s->sc_key) {
            lv_layer_t *l = th_scenery_begin(&s->scenery);
            if (l) {
                draw_scale(l, s);
                th_scenery_end(&s->scenery, l);
                s->sc_key = key;
            }
        }
        th_ring_prepare(&s->disc_ring, (int)s->r_min, 1, s->sel >= 0 ? C_ACCENT : C_DIM);
        lv_obj_invalidate(s->canvas);
    }
}

static void tap(page_t *p, int x, int y)
{
    hosts_t *s = p->state;
    if (!s->n) return;
    s->sel = s->sel + 1 >= s->n ? -1 : s->sel + 1;
    s->sel_ms = s->now;
}

static alert_level_t status(page_t *p, char *why, int len)
{
    hosts_t *s = p->state;
    alert_level_t lv = ALERT_NONE;
    for (int i = 0; i < s->n; i++) {
        if (s->snap[i].state == HOST_DOWN) {
            snprintf(why, len, "%s not answering", s->snap[i].name);
            return ALERT_CRIT;
        }
        if (host_slow(&s->snap[i]) && lv == ALERT_NONE) {
            snprintf(why, len, "%s slow", s->snap[i].name);
            lv = ALERT_WARN;
        }
    }
    return lv;
}

const page_type_t page_hosts = {
    .type = "hosts", .label = "Network", .create = create, .destroy = destroy,
    .tick = tick, .tap = tap, .status = status,
};
