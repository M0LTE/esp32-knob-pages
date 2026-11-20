// Energy page: home power from Home Assistant (Solax inverter, Octopus rates,
// Predbat's plan). The outer ring is the next 24 hours as a clock face: "now"
// at 12 o'clock, the future running clockwise; inside it, Predbat's planned
// battery level as a filled profile, and on its outer edge the import tariff,
// teal where cheap and red at peak. Inside, live power flows between solar
// (top), grid (left), house (right) and battery (bottom): dots stream along
// each path in the direction power is moving, faster for more power. The
// centre is the battery's state of charge. Tap to show today's totals.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "../model.h"
#include "../page.h"
#include "../theme.h"

#define PLAN_IN 124
#define PLAN_OUT 152
#define TARIFF_R 157
#define NODE_R 100
#define CORE_R 44
#define DOTS 4

typedef struct {
    lv_obj_t *canvas;
    th_scenery_t scenery;
    uint32_t scen_hash;
    uint32_t now, shown_ms;
    bool was_visible, totals;
    // snapshot
    bool up;
    uint32_t poll_ms;
    float pv, load, grid, batt, soc, soc_kwh, soc_max, rate, rate_next, export_rate;
    uint32_t rate_next_wall, plan_start;
    char status[24];
    float pv_today, import_today, cost_today;
    float plan_soc[ENERGY_SLOTS], plan_rate[ENERGY_SLOTS];
    // animation
    float phase[4];          // flow dot phase per path, 0..1
    float soc_disp;
} energy_t;

enum { P_SOLAR, P_GRID, P_HOUSE, P_BATT };
static const float node_angle[4] = {0, 270, 90, 180};

static void fmt_power(char *b, int n, float w)
{
    float a = fabsf(w);
    if (isnan(w)) snprintf(b, n, "--");
    else if (a < 1000) snprintf(b, n, "%.0f W", a);
    else snprintf(b, n, "%.2f kW", a / 1000);
}

static void fmt_clock(char *b, int n, uint32_t wall)
{
    time_t t = wall;
    struct tm tm;
    localtime_r(&t, &tm);
    snprintf(b, n, "%02d:%02d", tm.tm_hour, tm.tm_min);
}

static lv_color_t rate_colour(float p, float lo, float hi)
{
    if (isnan(p)) return C_FAINT;
    if (p <= lo + 0.5f) return C_OK;
    if (p >= hi - 0.5f) return C_CRIT;
    return C_DIM;
}

// Slot index to angle: slot 0 starts at 12 o'clock.
static float slot_angle(int i) { return i * 360.0f / ENERGY_SLOTS; }

static void draw_plan(lv_layer_t *l, energy_t *s)
{
    float lo = 1e9f, hi = -1e9f;
    for (int i = 0; i < ENERGY_SLOTS; i++) {
        if (isnan(s->plan_rate[i])) continue;
        lo = fminf(lo, s->plan_rate[i]);
        hi = fmaxf(hi, s->plan_rate[i]);
    }
    th_ring(l, SCR_CX, SCR_CY, PLAN_IN - 2, 1, C_FAINT, LV_OPA_COVER);
    float max = s->soc_max > 0 ? s->soc_max : 17.4f;
    for (int i = 0; i < ENERGY_SLOTS; i++) {
        float a = slot_angle(i) + 1.875f;
        // Planned battery level as a radial bar.
        if (!isnan(s->plan_soc[i])) {
            float r1 = PLAN_IN + (PLAN_OUT - PLAN_IN) * fminf(1, fmaxf(0, s->plan_soc[i] / max));
            th_line(l, th_px(PLAN_IN, a), th_py(PLAN_IN, a), th_px(r1, a), th_py(r1, a), 3, C_INK, 120);
        }
        // Tariff on the outer edge.
        th_arc(l, SCR_CX, SCR_CY, TARIFF_R, 3, slot_angle(i) + 0.4f, slot_angle(i + 1) - 0.4f,
               rate_colour(s->plan_rate[i], lo, hi), LV_OPA_COVER);
    }
    // Hour marks every three hours, inside the band.
    if (s->plan_start) {
        for (int i = 0; i < ENERGY_SLOTS; i++) {
            uint32_t w = s->plan_start + i * 900;
            time_t t = w;
            struct tm tm;
            localtime_r(&t, &tm);
            if (tm.tm_min != 0 || tm.tm_hour % 3) continue;
            float a = slot_angle(i);
            th_line(l, th_px(TARIFF_R + 3, a), th_py(TARIFF_R + 3, a), th_px(TARIFF_R + 7, a), th_py(TARIFF_R + 7, a), 1,
                    C_DIM, LV_OPA_COVER);
            // Labels outside the tariff ring, clear of the now marker and the page pips.
            if (i < 3 || i > ENERGY_SLOTS - 3 || fabsf(a - 180) < 22) continue;
            char b[4];
            snprintf(b, sizeof b, "%02d", tm.tm_hour);
            th_text(l, &font_mono_11, (int)th_px(TARIFF_R + 14, a), (int)th_py(TARIFF_R + 14, a) - 7, 24,
                    LV_TEXT_ALIGN_CENTER, C_DIM, LV_OPA_COVER, 0, b);
        }
    }
}

static void draw_flows(lv_layer_t *l, energy_t *s)
{
    // Each path runs between the core ring and a node; dir +1 = towards the core.
    float w[4] = {s->pv, s->grid, -s->load, -s->batt};
    lv_color_t col[4] = {C_WARN, C_INK, C_SUB, C_OK};
    for (int p = 0; p < 4; p++) {
        float a = node_angle[p];
        float r0 = CORE_R + 6, r1 = (p == P_GRID || p == P_HOUSE) ? NODE_R + 4 : p == P_BATT ? NODE_R - 30 : NODE_R - 16;
        th_line(l, th_px(r0, a), th_py(r0, a), th_px(r1, a), th_py(r1, a), 1, C_FAINT, LV_OPA_COVER);
        if (isnan(w[p]) || fabsf(w[p]) < 30) continue;
        for (int k = 0; k < DOTS; k++) {
            float t = fmodf(s->phase[p] + (float)k / DOTS, 1.0f);
            if (w[p] < 0) t = 1 - t;  // flowing outwards
            float r = r1 - (r1 - r0) * t;
            float fade = t < 0.15f ? t / 0.15f : t > 0.85f ? (1 - t) / 0.15f : 1;
            th_dot(l, th_px(r, a), th_py(r, a), 2, col[p], (lv_opa_t)(255 * fade));
        }
    }
}

static void draw_node(lv_layer_t *l, energy_t *s, int p, const char *label, float w, lv_color_t c, const char *dir)
{
    char b[24];
    float a = node_angle[p];
    int x = (int)th_px(NODE_R, a), y = (int)th_py(NODE_R, a);
    fmt_power(b, sizeof b, w);
    bool idle = isnan(w) || fabsf(w) < 30;
    // Side nodes stack above their flow line (direction below it), so the dots run clear.
    bool side = p == P_GRID || p == P_HOUSE;
    int y0 = side ? y - 40 : p == P_BATT ? y - 26 : y - 16;
    th_text(l, &font_mono_11, x, y0, 90, LV_TEXT_ALIGN_CENTER, idle ? C_DIM : c, LV_OPA_COVER, 2, label);
    th_text(l, &font_mono_16, x, y0 + 14, 100, LV_TEXT_ALIGN_CENTER, idle ? C_SUB : C_INK, LV_OPA_COVER, 0, b);
    if (dir && !idle)
        th_text(l, &font_mono_11, x, side ? y + 8 : y0 + 32, 90, LV_TEXT_ALIGN_CENTER, C_DIM, LV_OPA_COVER, 1, dir);
}

static void draw_core(lv_layer_t *l, energy_t *s)
{
    char b[32];
    th_ring(l, SCR_CX, SCR_CY, CORE_R, 1, C_FAINT, LV_OPA_COVER);
    float k = fminf(1, fmaxf(0, s->soc_disp / 100));
    lv_color_t c = s->soc_disp < 15 ? C_WARN : C_OK;
    if (k > 0.005f) th_arc(l, SCR_CX, SCR_CY, CORE_R, 4, 0, 360 * k, c, LV_OPA_COVER);
    if (isnan(s->soc)) snprintf(b, sizeof b, "--");
    else snprintf(b, sizeof b, "%.0f", s->soc_disp);
    th_text(l, &font_mono_34, SCR_CX - 6, SCR_CY - 22, 80, LV_TEXT_ALIGN_CENTER, C_INK, LV_OPA_COVER, 0, b);
    th_text(l, &font_mono_13, SCR_CX + 26, SCR_CY - 12, 20, LV_TEXT_ALIGN_LEFT, C_SUB, LV_OPA_COVER, 0, "%");
    if (!isnan(s->soc_kwh)) {
        snprintf(b, sizeof b, "%.1f kWh", s->soc_kwh);
        th_text(l, &font_mono_11, SCR_CX, SCR_CY + 14, 80, LV_TEXT_ALIGN_CENTER, C_DIM, LV_OPA_COVER, 0, b);
    }
}

static void draw_corners(lv_layer_t *l, energy_t *s)
{
    char a[32], b[32];
    const int dx = 74, dy = 74;
    if (!s->totals) {
        snprintf(a, sizeof a, "%.1fp", s->rate);
        th_text(l, &font_mono_11, SCR_CX - dx, SCR_CY - dy - 8, 80, LV_TEXT_ALIGN_CENTER, C_DIM, LV_OPA_COVER, 2, "NOW");
        th_text(l, &font_mono_13, SCR_CX - dx, SCR_CY - dy + 6, 80, LV_TEXT_ALIGN_CENTER, C_INK, LV_OPA_COVER, 0, a);
        if (!isnan(s->rate_next) && s->rate_next_wall) {
            fmt_clock(b, sizeof b, s->rate_next_wall);
            snprintf(a, sizeof a, "%.1fp", s->rate_next);
            th_text(l, &font_mono_11, SCR_CX + dx, SCR_CY - dy - 8, 80, LV_TEXT_ALIGN_CENTER, C_DIM, LV_OPA_COVER, 1, b);
            th_text(l, &font_mono_13, SCR_CX + dx, SCR_CY - dy + 6, 80, LV_TEXT_ALIGN_CENTER,
                    s->rate_next < s->rate ? C_OK : s->rate_next > s->rate ? C_CRIT : C_INK, LV_OPA_COVER, 0, a);
        }
        char st[24];
        snprintf(st, sizeof st, "%s", s->status[0] ? s->status : "--");
        for (char *q = st; *q; q++) if (*q >= 'a' && *q <= 'z') *q -= 32;
        th_text(l, &font_mono_11, SCR_CX - dx, SCR_CY + dy - 6, 90, LV_TEXT_ALIGN_CENTER, C_DIM, LV_OPA_COVER, 2, "PLAN");
        th_text(l, &font_mono_11, SCR_CX - dx, SCR_CY + dy + 8, 90, LV_TEXT_ALIGN_CENTER, C_SUB, LV_OPA_COVER, 1, st);
        snprintf(a, sizeof a, "%.1fp", s->export_rate);
        th_text(l, &font_mono_11, SCR_CX + dx, SCR_CY + dy - 6, 90, LV_TEXT_ALIGN_CENTER, C_DIM, LV_OPA_COVER, 2, "EXPORT");
        th_text(l, &font_mono_11, SCR_CX + dx, SCR_CY + dy + 8, 90, LV_TEXT_ALIGN_CENTER, C_SUB, LV_OPA_COVER, 0, a);
    } else {
        snprintf(a, sizeof a, "%.1f kWh", s->pv_today);
        th_text(l, &font_mono_11, SCR_CX - dx, SCR_CY - dy - 8, 90, LV_TEXT_ALIGN_CENTER, C_DIM, LV_OPA_COVER, 2, "SOLAR");
        th_text(l, &font_mono_13, SCR_CX - dx, SCR_CY - dy + 6, 90, LV_TEXT_ALIGN_CENTER, C_INK, LV_OPA_COVER, 0, a);
        snprintf(a, sizeof a, "%.1f kWh", s->import_today);
        th_text(l, &font_mono_11, SCR_CX + dx, SCR_CY - dy - 8, 90, LV_TEXT_ALIGN_CENTER, C_DIM, LV_OPA_COVER, 2, "IMPORT");
        th_text(l, &font_mono_13, SCR_CX + dx, SCR_CY - dy + 6, 90, LV_TEXT_ALIGN_CENTER, C_INK, LV_OPA_COVER, 0, a);
        snprintf(a, sizeof a, "\xC2\xA3%.2f", s->cost_today / 100);
        th_text(l, &font_mono_11, SCR_CX - dx, SCR_CY + dy - 6, 90, LV_TEXT_ALIGN_CENTER, C_DIM, LV_OPA_COVER, 2, "COST");
        th_text(l, &font_mono_13, SCR_CX - dx, SCR_CY + dy + 8, 90, LV_TEXT_ALIGN_CENTER, C_INK, LV_OPA_COVER, 0, a);
        th_text(l, &font_mono_11, SCR_CX + dx, SCR_CY + dy + 1, 90, LV_TEXT_ALIGN_CENTER, C_DIM, LV_OPA_COVER, 2, "TODAY");
    }
}

static void draw_body(lv_event_t *e)
{
    energy_t *s = lv_event_get_user_data(e);
    lv_layer_t *l = lv_event_get_layer(e);
    if (!s->poll_ms) {
        th_text(l, &font_mono_16, SCR_CX, SCR_CY - 30, 260, LV_TEXT_ALIGN_CENTER, C_SUB, LV_OPA_COVER, 2,
                "HOME ASSISTANT");
        th_text(l, &font_mono_11, SCR_CX, SCR_CY - 4, 240, LV_TEXT_ALIGN_CENTER, C_DIM, LV_OPA_COVER, 1,
                "waiting for data; set the token");
        th_text(l, &font_mono_11, SCR_CX, SCR_CY + 12, 240, LV_TEXT_ALIGN_CENTER, C_DIM, LV_OPA_COVER, 1,
                "on the settings page");
        return;
    }
    if (!th_scenery_blit(l, &s->scenery, s->canvas)) draw_plan(l, s);
    // Now marker.
    th_line(l, SCR_CX, SCR_CY - TARIFF_R - 6, SCR_CX, SCR_CY - PLAN_IN + 2, 2, C_ACCENT, 230);
    draw_flows(l, s);
    draw_node(l, s, P_SOLAR, "SOLAR", s->pv, C_WARN, NULL);
    draw_node(l, s, P_GRID, "GRID", s->grid, C_INK, s->grid > 0 ? "IMPORT" : "EXPORT");
    draw_node(l, s, P_HOUSE, "HOUSE", s->load, C_SUB, NULL);
    draw_node(l, s, P_BATT, "BATTERY", s->batt, C_OK, s->batt > 0 ? "CHARGING" : "DISCHARGING");
    draw_core(l, s);
    draw_corners(l, s);

    uint32_t age = s->now - s->shown_ms;
    if (age < 3500) {
        lv_opa_t o = age < 2500 ? LV_OPA_COVER : (lv_opa_t)(255 * (3500 - age) / 1000);
        th_text(l, &font_sans_15, SCR_CX, 14, 160, LV_TEXT_ALIGN_CENTER, C_SUB, o, 4, "ENERGY");
    }
    if (!s->up) th_text(l, &font_mono_11, SCR_CX, 312, 120, LV_TEXT_ALIGN_CENTER, C_CRIT, LV_OPA_COVER, 2, "STALE");
}

TH_PAGE_DRAW(draw_cb, draw_body, (lv_obj_t *)lv_event_get_current_target(e))

static void create(page_t *p, lv_obj_t *root)
{
    energy_t *s = calloc(1, sizeof *s);
    p->state = s;
    s->canvas = lv_obj_create(root);
    lv_obj_remove_style_all(s->canvas);
    lv_obj_set_size(s->canvas, SCR_W, SCR_H);
    lv_obj_remove_flag(s->canvas, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s->canvas, draw_cb, LV_EVENT_DRAW_MAIN, s);
}

static void destroy(page_t *p) { free(p->state); }

static void tick(page_t *p, uint32_t now, uint32_t dt, bool visible)
{
    energy_t *s = p->state;
    s->now = now;
    if (visible && !s->was_visible) s->shown_ms = now;
    s->was_visible = visible;

    model_lock();
    s->up = g_model.energy_up;
    s->poll_ms = g_model.energy_poll_ms;
    s->pv = g_model.pv_w;
    s->load = g_model.load_w;
    s->grid = g_model.grid_w;
    s->batt = g_model.batt_w;
    s->soc = g_model.soc_pct;
    s->soc_kwh = g_model.soc_kwh;
    s->soc_max = g_model.soc_max_kwh;
    s->rate = g_model.rate_p;
    s->rate_next = g_model.rate_next_p;
    s->rate_next_wall = g_model.rate_next_wall;
    s->export_rate = g_model.export_rate_p;
    memcpy(s->status, g_model.plan_status, sizeof s->status);
    s->pv_today = g_model.pv_today_kwh;
    s->import_today = g_model.import_today_kwh;
    s->cost_today = g_model.cost_today_p;
    s->plan_start = g_model.plan_start_wall;
    memcpy(s->plan_soc, g_model.plan_soc_kwh, sizeof s->plan_soc);
    memcpy(s->plan_rate, g_model.plan_rate_p, sizeof s->plan_rate);
    model_unlock();

    // Flow dots: speed grows with the log of the power.
    float w[4] = {s->pv, s->grid, s->load, s->batt};
    for (int i = 0; i < 4; i++) {
        float a = isnan(w[i]) ? 0 : fabsf(w[i]);
        float speed = a < 30 ? 0 : 0.15f + 0.35f * log10f(a / 30) / 2.5f;  // path lengths per second
        s->phase[i] = fmodf(s->phase[i] + speed * dt / 1000.0f, 1.0f);
    }
    if (!isnan(s->soc)) s->soc_disp = th_ease(s->soc_disp, s->soc, dt, 400);

    // Rebuild the plan ring when its data changes.
    uint32_t h = TH_HASH_SEED;
    h = th_hash(h, (int32_t)s->plan_start);
    for (int i = 0; i < ENERGY_SLOTS; i++) {
        h = th_hash(h, isnan(s->plan_soc[i]) ? -1 : (int32_t)(s->plan_soc[i] * 10));
        h = th_hash(h, isnan(s->plan_rate[i]) ? -1 : (int32_t)(s->plan_rate[i] * 10));
    }
    if (s->poll_ms && h != s->scen_hash) {
        lv_layer_t *l = th_scenery_begin(&s->scenery);
        if (l) {
            draw_plan(l, s);
            th_scenery_end(&s->scenery, l);
            s->scen_hash = h;
        }
    }
    if (visible) lv_obj_invalidate(s->canvas);
}

static void tap(page_t *p, int x, int y)
{
    energy_t *s = p->state;
    s->totals = !s->totals;
}

static alert_level_t status(page_t *p, char *why, int len)
{
    energy_t *s = p->state;
    if (s->poll_ms && !s->up && s->now - s->poll_ms > 120000) {
        snprintf(why, len, "Home Assistant not answering");
        return ALERT_WARN;
    }
    return ALERT_NONE;
}

const page_type_t page_energy = {
    .type = "energy", .label = "Energy", .create = create, .destroy = destroy,
    .tick = tick, .tap = tap, .status = status,
};
