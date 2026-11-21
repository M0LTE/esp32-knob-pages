// Sky page: aircraft round home from ADS-B, and which of them you are likely
// to hear. North is up and home is the centre; range rings at 2, 5, 10 and 25
// km on a square-root scale, so nearby traffic gets the room. Each aircraft is
// an arrow along its track with a fading trail, brighter the lower it is.
// "Can I hear it" comes from slant distance against a range that grows with
// the aircraft's size class, and from where its present track will pass
// closest. The next one you will hear is shown in amber with a card: who it
// is, where it is going, and how long until it is overhead; when one is about
// to pass close the page raises a NOTICE (a soft chime and a jump here).
// Tap to step through aircraft by distance; it returns to the automatic
// choice after a while.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../model.h"
#include "../page.h"
#include "../theme.h"

#define MAP_R 150
#define MAX_ALT_FT 10000.0f   // above this, assume you won't hear it
#define NOTICE_LEAD_S 45.0f
#define AUDIBLE_SCORE 0.25f  // well inside the range, not at its edge
#define NOTICE_SCORE 0.5f    // chime only for ones that come about half as close   // raise the notice this long before the closest point
#define SELECT_HOLD_MS 15000
#define LABELS 7

typedef struct {
    float dx, dy, d;         // km from home, east and north
    float slant;             // km, now
    float score;             // 0..1, how loud now
    float t_cpa;             // s until closest approach (0 if past)
    float score_cpa;         // 0..1 at the closest approach (within 3 min)
    bool audible_soon;
} acx_t;

typedef struct {
    lv_obj_t *canvas;
    uint32_t now, shown_ms;
    bool was_visible;
    float home_lat, home_lon, radius;
    bool up;
    uint32_t poll_ms;
    int n;
    aircraft_t ac[MODEL_MAX_AC];
    acx_t x[MODEL_MAX_AC];
    int order[MODEL_MAX_AC];  // by distance
    int focus;                // index into ac, -1 none
    char sel_hex[8];          // chosen by tap, "" = automatic
    uint32_t sel_ms;
    float cy;                 // map centre y, eased up while a card is showing
} sky_t;

static float cat_range_km(const char *cat)
{
    // Slant range at which it is just audible indoors. Tuned down from the first
    // guesses (which chimed for too much); expect to tune again.
    if (!strcmp(cat, "A1")) return 1.2f;   // light
    if (!strcmp(cat, "A2")) return 1.8f;   // small
    if (!strcmp(cat, "A3")) return 3.5f;   // large: A320, 737
    if (!strcmp(cat, "A4")) return 4.0f;   // high vortex: 757
    if (!strcmp(cat, "A5")) return 5.0f;   // heavy
    if (!strcmp(cat, "A6")) return 4.5f;   // high performance
    if (!strcmp(cat, "A7")) return 2.5f;   // rotorcraft
    return 2.5f;
}

static float loudness(float slant_km, float alt_ft, const char *cat)
{
    if (isnan(alt_ft) || alt_ft > MAX_ALT_FT) return 0;
    float k = 1 - slant_km / cat_range_km(cat);
    return k < 0 ? 0 : k;
}

static void work_out(sky_t *s)
{
    float kx = 111.32f * cosf(DEG2RAD(s->home_lat)), ky = 110.57f;
    for (int i = 0; i < s->n; i++) {
        const aircraft_t *a = &s->ac[i];
        acx_t *x = &s->x[i];
        x->dx = (a->lon - s->home_lon) * kx;
        x->dy = (a->lat - s->home_lat) * ky;
        x->d = sqrtf(x->dx * x->dx + x->dy * x->dy);
        float h = isnan(a->alt_ft) ? 0 : a->alt_ft * 0.0003048f;
        x->slant = sqrtf(x->d * x->d + h * h);
        x->score = loudness(x->slant, a->alt_ft, a->cat);
        x->t_cpa = 0;
        x->score_cpa = x->score;
        if (!isnan(a->gs_kt) && !isnan(a->track) && a->gs_kt > 30) {
            float v = a->gs_kt * 0.000514444f;  // km/s
            float vx = v * sinf(DEG2RAD(a->track)), vy = v * cosf(DEG2RAD(a->track));
            float t = -(x->dx * vx + x->dy * vy) / (v * v);
            if (t > 0 && t < 180) {
                float cx = x->dx + vx * t, cy = x->dy + vy * t;
                float alt = isnan(a->alt_ft) ? NAN : a->alt_ft + (isnan(a->rate_fpm) ? 0 : a->rate_fpm * t / 60);
                if (!isnan(alt) && alt < 0) alt = 0;
                float hh = isnan(alt) ? 0 : alt * 0.0003048f;
                x->t_cpa = t;
                x->score_cpa = fmaxf(x->score, loudness(sqrtf(cx * cx + cy * cy + hh * hh), alt, a->cat));
            }
        }
        x->audible_soon = x->score > AUDIBLE_SCORE || (x->score_cpa > AUDIBLE_SCORE && x->t_cpa < 120);
        s->order[i] = i;
    }
    // Sort by distance (insertion sort; there are few).
    for (int i = 1; i < s->n; i++)
        for (int j = i; j > 0 && s->x[s->order[j]].d < s->x[s->order[j - 1]].d; j--) {
            int t = s->order[j];
            s->order[j] = s->order[j - 1];
            s->order[j - 1] = t;
        }
}

static bool to_screen(const sky_t *s, const acx_t *x, float *px, float *py)
{
    if (isnan(x->d) || x->d > s->radius * 1.02f) return false;
    float r = MAP_R * sqrtf(x->d / s->radius);
    float k = x->d > 0.001f ? r / x->d : 0;
    *px = SCR_CX + x->dx * k;
    *py = s->cy - x->dy * k;
    return true;
}

static float ring_r(const sky_t *s, float km) { return MAP_R * sqrtf(km / s->radius); }

static lv_color_t alt_colour(float alt)
{
    if (isnan(alt)) return C_DIM;
    if (alt < 5000) return C_INK;
    if (alt < 15000) return C_SUB;
    return C_DIM;
}

static void name_of(const aircraft_t *a, char *b, int n)
{
    snprintf(b, n, "%s", a->flight[0] ? a->flight : a->reg[0] ? a->reg : a->hex);
}

static void draw_map(lv_layer_t *l, sky_t *s)
{
    static const float rings[] = {2, 5, 10, 25};
    for (int i = 0; i < 4; i++) {
        if (rings[i] > s->radius) continue;
        float r = ring_r(s, rings[i]);
        th_ring(l, SCR_CX, (int)s->cy, (int)r, 1, i == 3 ? C_DIM : C_FAINT, LV_OPA_COVER);
        char b[8];
        snprintf(b, sizeof b, "%.0fkm", rings[i]);
        float a = DEG2RAD(52);
        th_text(l, &font_mono_11, (int)(SCR_CX + r * sinf(a)) + 3, (int)(s->cy - r * cosf(a)) - 12, 40,
                LV_TEXT_ALIGN_LEFT, C_FAINT, LV_OPA_COVER, 0, b);
    }
    // Compass points just inside the outer ring.
    static const char *pts[] = {"N", "E", "S", "W"};
    for (int i = 0; i < 4; i++) {
        float a = i * 90.0f, r = MAP_R + 12;
        if (i == 2) r = MAP_R - 12;  // keep S clear of the page pips
        float x = SCR_CX + r * sinf(DEG2RAD(a)), y = s->cy - r * cosf(DEG2RAD(a));
        if (y > 330) continue;  // S hides under the card while the map is shifted
        th_text(l, &font_mono_11, (int)x, (int)y - 7, 16, LV_TEXT_ALIGN_CENTER, i == 0 ? C_SUB : C_DIM,
                LV_OPA_COVER, 0, pts[i]);
    }
    // Home.
    th_line(l, SCR_CX - 6, s->cy, SCR_CX + 6, s->cy, 1, C_ACCENT, LV_OPA_COVER);
    th_line(l, SCR_CX, s->cy - 6, SCR_CX, s->cy + 6, 1, C_ACCENT, LV_OPA_COVER);
}

static void draw_aircraft(lv_layer_t *l, sky_t *s)
{
    char b[24], nm[12];
    int labelled = 0;
    // Furthest first so the near ones sit on top.
    for (int oi = s->n - 1; oi >= 0; oi--) {
        int i = s->order[oi];
        const aircraft_t *a = &s->ac[i];
        const acx_t *x = &s->x[i];
        float px, py;
        if (!to_screen(s, x, &px, &py)) continue;
        bool hot = x->audible_soon;
        bool foc = i == s->focus;
        lv_color_t c = foc || hot ? C_ACCENT : alt_colour(a->alt_ft);
        // Trail.
        acx_t t;
        for (int k = 0; k < a->trail_n; k++) {
            aircraft_t tmp = *a;
            tmp.lat = a->trail_lat[k];
            tmp.lon = a->trail_lon[k];
            float kx = 111.32f * cosf(DEG2RAD(s->home_lat));
            t.dx = (tmp.lon - s->home_lon) * kx;
            t.dy = (tmp.lat - s->home_lat) * 110.57f;
            t.d = sqrtf(t.dx * t.dx + t.dy * t.dy);
            float tx, ty;
            if (to_screen(s, &t, &tx, &ty))
                th_dot(l, tx, ty, 1, c, (lv_opa_t)(40 + 120 * (k + 1) / (a->trail_n + 1)));
        }
        // Arrow along the track; bigger for bigger aircraft.
        float sz = !strcmp(a->cat, "A5") ? 9 : (!strcmp(a->cat, "A1") || !strcmp(a->cat, "A7")) ? 5.5f : 7;
        float tr = isnan(a->track) ? 0 : a->track;
        float nx = sinf(DEG2RAD(tr)), ny = -cosf(DEG2RAD(tr));
        float ax = px + nx * sz, ay = py + ny * sz;
        float lx = px - nx * sz * 0.7f - ny * sz * 0.6f, ly = py - ny * sz * 0.7f + nx * sz * 0.6f;
        float rx = px - nx * sz * 0.7f + ny * sz * 0.6f, ry = py - ny * sz * 0.7f - nx * sz * 0.6f;
        th_tri(l, ax, ay, lx, ly, rx, ry, c, LV_OPA_COVER);
        if (hot) {
            float k = (s->now % 1400) / 1400.0f;
            th_ring(l, px, py, (int)(8 + 10 * k), 1, C_ACCENT, (lv_opa_t)(180 * (1 - k)));
        }
        if (foc)
            for (int q = 0; q < 4; q++) th_arc(l, (int)px, (int)py, 13, 2, q * 90 + 20, q * 90 + 70, C_ACCENT, LV_OPA_COVER);
        // Labels for the nearest few (and always the focus).
        if (foc || (oi < LABELS && labelled < LABELS)) {
            labelled++;
            name_of(a, nm, sizeof nm);
            if (isnan(a->alt_ft)) snprintf(b, sizeof b, "%s", nm);
            else if (a->alt_ft == 0) snprintf(b, sizeof b, "%s GND", nm);
            else snprintf(b, sizeof b, "%s %.1fk", nm, a->alt_ft / 1000);
            bool right = px < SCR_CX + 60;
            th_text(l, &font_mono_11, (int)(right ? px + 11 : px - 11), (int)py - 7, 110,
                    right ? LV_TEXT_ALIGN_LEFT : LV_TEXT_ALIGN_RIGHT, foc || hot ? C_ACCENT : C_SUB,
                    s->focus >= 0 && !foc ? 140 : LV_OPA_COVER, 0, b);
        }
    }
}

static void draw_card(lv_layer_t *l, sky_t *s)
{
    if (s->focus < 0) return;
    const aircraft_t *a = &s->ac[s->focus];
    const acx_t *x = &s->x[s->focus];
    char b[64], nm[12];
    lv_draw_rect_dsc_t bd;
    lv_draw_rect_dsc_init(&bd);
    bd.bg_color = C_BG;
    bd.bg_opa = LV_OPA_COVER;
    bd.radius = 14;
    bd.border_color = x->audible_soon ? C_ACCENT : C_FAINT;
    bd.border_width = 1;
    bd.border_opa = 180;
    lv_area_t ba = {SCR_CX - 118, 214, SCR_CX + 118, 306};
    lv_draw_rect(l, &bd, &ba);

    // Countdown or state line, above the card's text.
    if (x->score > AUDIBLE_SCORE) snprintf(b, sizeof b, "AUDIBLE NOW");
    else if (x->audible_soon) snprintf(b, sizeof b, "OVERHEAD IN %.0fs", x->t_cpa);
    else snprintf(b, sizeof b, "%.1f km", x->d);
    th_text(l, &font_mono_11, SCR_CX, 220, 220, LV_TEXT_ALIGN_CENTER, x->audible_soon ? C_ACCENT : C_DIM,
            LV_OPA_COVER, 2, b);

    name_of(a, nm, sizeof nm);
    th_text(l, &font_sans_20, SCR_CX - 6, 234, 120, LV_TEXT_ALIGN_RIGHT, C_INK, LV_OPA_COVER, 1, nm);
    snprintf(b, sizeof b, "%s %s", a->type[0] ? a->type : "?", a->reg);
    th_text(l, &font_mono_13, SCR_CX + 4, 239, 120, LV_TEXT_ALIGN_LEFT, C_SUB, LV_OPA_COVER, 0, b);

    // Operator, or the type in words.
    char who[40];
    snprintf(who, sizeof who, "%s", a->op[0] ? a->op : a->desc);
    th_text(l, &font_mono_11, SCR_CX, 259, 220, LV_TEXT_ALIGN_CENTER, C_DIM, LV_OPA_COVER, 0, who);

    if (a->orig[0] || a->dest[0]) {
        snprintf(b, sizeof b, "%s %s \xE2\x86\x92 %s %s", a->orig, a->orig_city, a->dest, a->dest_city);
        th_text(l, &font_mono_11, SCR_CX, 273, 228, LV_TEXT_ALIGN_CENTER, C_INK, LV_OPA_COVER, 0, b);
    }
    char alt[16], rate[12] = "";
    if (isnan(a->alt_ft)) snprintf(alt, sizeof alt, "-- ft");
    else if (a->alt_ft == 0) snprintf(alt, sizeof alt, "GROUND");
    else snprintf(alt, sizeof alt, "%.0f ft", a->alt_ft);
    if (!isnan(a->rate_fpm) && fabsf(a->rate_fpm) >= 300)
        snprintf(rate, sizeof rate, " %s%.0f", a->rate_fpm > 0 ? "\xE2\x86\x91" : "\xE2\x86\x93", fabsf(a->rate_fpm));
    snprintf(b, sizeof b, "%s%s  %.0f kt  %.1f km", alt, rate, isnan(a->gs_kt) ? 0.0f : a->gs_kt, x->d);
    th_text(l, &font_mono_11, SCR_CX, 288, 228, LV_TEXT_ALIGN_CENTER, C_SUB, LV_OPA_COVER, 0, b);
}

static void draw_body(lv_event_t *e)
{
    sky_t *s = lv_event_get_user_data(e);
    lv_layer_t *l = lv_event_get_layer(e);
    if (isnan(s->home_lat) || isnan(s->home_lon) || s->radius <= 0) {
        th_text(l, &font_mono_16, SCR_CX, SCR_CY - 30, 260, LV_TEXT_ALIGN_CENTER, C_SUB, LV_OPA_COVER, 2, "SKY");
        th_text(l, &font_mono_11, SCR_CX, SCR_CY - 4, 240, LV_TEXT_ALIGN_CENTER, C_DIM, LV_OPA_COVER, 1,
                "set your location on");
        th_text(l, &font_mono_11, SCR_CX, SCR_CY + 12, 240, LV_TEXT_ALIGN_CENTER, C_DIM, LV_OPA_COVER, 1,
                "the settings page");
        return;
    }
    draw_map(l, s);
    draw_aircraft(l, s);
    draw_card(l, s);
    char b[24];
    uint32_t age = s->now - s->shown_ms;
    if (age < 3500) {
        lv_opa_t o = age < 2500 ? LV_OPA_COVER : (lv_opa_t)(255 * (3500 - age) / 1000);
        th_text(l, &font_sans_15, SCR_CX, 8, 160, LV_TEXT_ALIGN_CENTER, C_SUB, o, 4, "SKY");
    }
    if (!s->up && s->poll_ms) {
        th_text(l, &font_mono_11, SCR_CX, 50, 120, LV_TEXT_ALIGN_CENTER, C_CRIT, LV_OPA_COVER, 2, "STALE");
    } else if (s->poll_ms && age >= 3500 && s->focus < 0) {
        snprintf(b, sizeof b, "%d AIRCRAFT", s->n);
        th_text(l, &font_mono_11, SCR_CX, 50, 160, LV_TEXT_ALIGN_CENTER, C_DIM, LV_OPA_COVER, 2, b);
    }
}

TH_PAGE_DRAW(draw_cb, draw_body, (lv_obj_t *)lv_event_get_current_target(e))

static void create(page_t *p, lv_obj_t *root)
{
    sky_t *s = calloc(1, sizeof *s);
    p->state = s;
    s->focus = -1;
    s->home_lat = s->home_lon = NAN;
    s->cy = SCR_CY;
    s->canvas = lv_obj_create(root);
    lv_obj_remove_style_all(s->canvas);
    lv_obj_set_size(s->canvas, SCR_W, SCR_H);
    lv_obj_remove_flag(s->canvas, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s->canvas, draw_cb, LV_EVENT_DRAW_MAIN, s);
}

static void destroy(page_t *p) { free(p->state); }

static void tick(page_t *p, uint32_t now, uint32_t dt, bool visible)
{
    sky_t *s = p->state;
    s->now = now;
    if (visible && !s->was_visible) s->shown_ms = now;
    s->was_visible = visible;
    model_lock();
    s->home_lat = g_model.home_lat;
    s->home_lon = g_model.home_lon;
    s->radius = g_model.adsb_radius_km;
    s->up = g_model.adsb_up;
    s->poll_ms = g_model.adsb_poll_ms;
    s->n = g_model.n_ac;
    memcpy(s->ac, g_model.ac, sizeof(aircraft_t) * s->n);
    model_unlock();
    if (isnan(s->home_lat) || s->radius <= 0) {
        s->n = 0;
        if (visible) lv_obj_invalidate(s->canvas);
        return;
    }
    // Positions move between polls: dead-reckon from the last fix so arrows glide.
    for (int i = 0; i < s->n; i++) {
        aircraft_t *a = &s->ac[i];
        if (isnan(a->gs_kt) || isnan(a->track) || isnan(a->lat)) continue;
        float age = (now - a->seen_ms) / 1000.0f;
        if (age < 0 || age > 10) continue;
        float km = a->gs_kt * 0.000514444f * age;
        a->lat += km * cosf(DEG2RAD(a->track)) / 110.57f;
        a->lon += km * sinf(DEG2RAD(a->track)) / (111.32f * cosf(DEG2RAD(s->home_lat)));
    }
    work_out(s);

    // Focus: a tapped choice while fresh, else the next one you will hear
    // (soonest, loudest), else nothing.
    s->focus = -1;
    if (s->sel_hex[0] && now - s->sel_ms < SELECT_HOLD_MS) {
        for (int i = 0; i < s->n; i++)
            if (!strcmp(s->ac[i].hex, s->sel_hex)) s->focus = i;
    } else {
        s->sel_hex[0] = 0;
    }
    if (s->focus < 0) {
        float best = 0;
        for (int i = 0; i < s->n; i++) {
            const acx_t *x = &s->x[i];
            if (!x->audible_soon) continue;
            float v = fmaxf(x->score, x->score_cpa) * (x->score > 0 ? 2 : 1) / (1 + x->t_cpa / 60);
            if (v > best) {
                best = v;
                s->focus = i;
            }
        }
    }
    s->cy = th_ease(s->cy, s->focus >= 0 ? SCR_CY - 40 : SCR_CY, dt, 220);
    if (visible) lv_obj_invalidate(s->canvas);
}

static void tap(page_t *p, int x, int y)
{
    sky_t *s = p->state;
    if (!s->n) return;
    // Step through aircraft by distance, starting from the nearest.
    int at = -1;
    for (int oi = 0; oi < s->n; oi++)
        if (s->sel_hex[0] && !strcmp(s->ac[s->order[oi]].hex, s->sel_hex)) at = oi;
    at = at + 1 >= s->n || at + 1 >= 10 ? -1 : at + 1;
    if (at < 0) s->sel_hex[0] = 0;
    else snprintf(s->sel_hex, sizeof s->sel_hex, "%s", s->ac[s->order[at]].hex);
    s->sel_ms = s->now;
}

static alert_level_t status(page_t *p, char *why, int len)
{
    sky_t *s = p->state;
    // A notice for each aircraft that is about to pass close enough to hear;
    // it stays up while that aircraft is audible, and the reason names it so a
    // different aircraft raises a fresh notice.
    for (int i = 0; i < s->n; i++) {
        const acx_t *x = &s->x[i];
        bool near = x->score > NOTICE_SCORE || (x->score_cpa > NOTICE_SCORE && x->t_cpa < NOTICE_LEAD_S);
        if (!near) continue;
        char nm[12];
        name_of(&s->ac[i], nm, sizeof nm);
        snprintf(why, len, "%s %s overhead", nm, s->ac[i].type);
        return ALERT_NOTICE;
    }
    return ALERT_NONE;
}

const page_type_t page_sky = {
    .type = "sky", .label = "Sky", .create = create, .destroy = destroy,
    .tick = tick, .tap = tap, .status = status,
};
