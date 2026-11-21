#include "ui.h"

#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

#include "theme.h"

#define MAX_PAGES 12
#define FRAME_MS 33
#define SLIDE_MS 320
#define STATUS_EVERY_MS 1000
#define SETTLE_MS 15000   // no alerts while sources fill in after start-up

extern const page_type_t page_hosts;
extern const page_type_t page_station;
extern const page_type_t page_modem;
extern const page_type_t page_energy;
extern const page_type_t page_rooms;
extern const page_type_t page_sky;
extern const page_type_t page_waterfall;
static const page_type_t *const s_types[] = {&page_hosts, &page_station, &page_modem, &page_energy, &page_rooms,
                                             &page_sky, &page_waterfall};

const page_type_t *page_type_find(const char *type)
{
    for (size_t i = 0; i < sizeof s_types / sizeof s_types[0]; i++)
        if (!strcmp(s_types[i]->type, type)) return s_types[i];
    return NULL;
}

__attribute__((weak)) void platform_alert(alert_level_t level, const char *t, const char *why) {}
__attribute__((weak)) void platform_page_changed(int index, int count) {}
__attribute__((weak)) void platform_alert_cleared(void) {}

static page_t s_pages[MAX_PAGES];
static alert_level_t s_level[MAX_PAGES];
static char s_why[MAX_PAGES][64];
static int s_n, s_cur, s_from = -1;
static uint32_t s_slide_start, s_last, s_last_status, s_start;
static int s_slide_dir;
static atomic_int s_knob;
static lv_obj_t *s_overlay;
static uint32_t s_alert_ms;   // last alert raised, drives the banner
static int s_alert_page = -1;
static th_ringcache_t s_alert_ring[2];
static bool s_alert_acked = true;

int ui_page_count(void) { return s_n; }
void ui_knob(int delta) { atomic_fetch_add(&s_knob, delta); }

static void place(int i, int x)
{
    lv_obj_set_pos(s_pages[i].root, x, 0);
    if (x <= -SCR_W || x >= SCR_W) lv_obj_add_flag(s_pages[i].root, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_remove_flag(s_pages[i].root, LV_OBJ_FLAG_HIDDEN);
}

void ui_goto(int index, bool animate)
{
    if (index < 0 || index >= s_n || index == s_cur) return;
    if (animate) {
        s_from = s_cur;
        s_slide_dir = index > s_cur ? 1 : -1;
        s_slide_start = lv_tick_get();
    } else {
        place(s_cur, SCR_W);
        s_from = -1;
    }
    s_cur = index;
    if (!animate) place(s_cur, 0);
    platform_page_changed(s_cur, s_n);
}

// Page pips around the bottom of the rim, plus the alert rim and banner.
static void overlay_draw(lv_event_t *e)
{
    lv_layer_t *l = lv_event_get_layer(e);
    uint32_t now = lv_tick_get();
    float pitch = 6.0f, len = 3.5f;
    float start = 180.0f + pitch * (s_n - 1) / 2.0f;
    for (int i = 0; i < s_n; i++) {
        float a = start - i * pitch;
        lv_color_t c = i == s_cur ? C_ACCENT : (s_level[i] == ALERT_CRIT ? C_CRIT : s_level[i] == ALERT_WARN ? C_WARN
                                              : s_level[i] == ALERT_NOTICE ? C_SUB : C_DIM);
        th_arc(l, SCR_CX, SCR_CY, 172, i == s_cur ? 4 : 2, a - len / 2, a + len / 2, c, LV_OPA_COVER);
    }
    // A notice gets no rim or banner: the page itself shows what is happening.
    if (!s_alert_acked && s_alert_page >= 0 && s_level[s_alert_page] > ALERT_NOTICE) {
        alert_level_t lv = s_level[s_alert_page];
        lv_color_t c = lv == ALERT_CRIT ? C_CRIT : C_WARN;
        float k = (now % 1400) / 1400.0f;
        float pulse = k < 0.5f ? k * 2 : (1 - k) * 2;
        th_ring_cached(l, &s_alert_ring[0], SCR_CX, SCR_CY, 177, 2, c, (lv_opa_t)(60 + 160 * pulse));
        th_ring_cached(l, &s_alert_ring[1], SCR_CX, SCR_CY, 174, 1, c, (lv_opa_t)(30 + 60 * pulse));
        if (s_alert_page == s_cur) {
            lv_draw_rect_dsc_t bd;
            lv_draw_rect_dsc_init(&bd);
            bd.bg_color = C_BG;
            bd.bg_opa = 235;
            bd.radius = 12;
            bd.border_color = c;
            bd.border_width = 1;
            bd.border_opa = 160;
            lv_area_t ba = {SCR_CX - 92, 292, SCR_CX + 92, 327};
            lv_draw_rect(l, &bd, &ba);
            th_text(l, &font_mono_11, SCR_CX, 296, 176, LV_TEXT_ALIGN_CENTER, c, LV_OPA_COVER, 1, s_why[s_alert_page]);
            th_text(l, &font_mono_11, SCR_CX, 311, 120, LV_TEXT_ALIGN_CENTER, C_DIM, LV_OPA_COVER, 2, "TAP TO ACK");
        }
    }
}

static void check_status(uint32_t now)
{
    for (int i = 0; i < s_n; i++) {
        page_t *p = &s_pages[i];
        if (!p->type->status) continue;
        char why[64] = "";
        alert_level_t lv = p->type->status(p, why, sizeof why);
        if (lv == ALERT_CRIT && s_level[i] < ALERT_CRIT && now - s_start > SETTLE_MS) {
            // Rising edge to critical: show it, jump to it, make a noise.
            // Warnings only colour the page's pip.
            snprintf(s_why[i], sizeof s_why[i], "%s", why);
            s_alert_page = i;
            s_alert_acked = false;
            s_alert_ms = now;
            ui_goto(i, true);
            platform_alert(lv, p->title, why);
        }
        // A notice: raised for each new event (a new reason), but never over an
        // unacknowledged critical alert.
        bool crit_open = !s_alert_acked && s_alert_page >= 0 && s_level[s_alert_page] == ALERT_CRIT;
        if (lv == ALERT_NOTICE && !crit_open && now - s_start > SETTLE_MS &&
            (s_level[i] != ALERT_NOTICE || strcmp(why, s_why[i]))) {
            snprintf(s_why[i], sizeof s_why[i], "%s", why);
            s_alert_page = i;
            s_alert_acked = false;
            s_alert_ms = now;
            ui_goto(i, true);
            platform_alert(lv, p->title, why);
        }
        if (lv == ALERT_NONE && i == s_alert_page && !s_alert_acked) {
            s_alert_acked = true;
            platform_alert_cleared();
        }
        s_level[i] = lv;
    }
}

static float ease_out(float k) { return 1 - (1 - k) * (1 - k) * (1 - k); }

static void frame(lv_timer_t *t)
{
    uint32_t now = lv_tick_get();
    uint32_t dt = now - s_last;
    s_last = now;

    int d = atomic_exchange(&s_knob, 0);
    if (d && s_from < 0 && s_pages[s_cur].type->knob && s_pages[s_cur].type->knob(&s_pages[s_cur], d)) d = 0;
    if (d && s_from < 0) {
        int to = s_cur + (d > 0 ? 1 : -1);
        if (to < 0) to = s_n - 1;
        if (to >= s_n) to = 0;
        ui_goto(to, true);
    }

    if (s_from >= 0) {
        float k = (float)(now - s_slide_start) / SLIDE_MS;
        if (k >= 1) {
            place(s_from, SCR_W * 2);
            place(s_cur, 0);
            s_from = -1;
        } else {
            int off = (int)(SCR_W * ease_out(k));
            place(s_from, -s_slide_dir * off);
            place(s_cur, s_slide_dir * (SCR_W - off));
        }
    }

    for (int i = 0; i < s_n; i++) {
        bool vis = i == s_cur || i == s_from;
        s_pages[i].type->tick(&s_pages[i], now, dt, vis);
    }
    if (now - s_last_status >= STATUS_EVERY_MS) {
        s_last_status = now;
        check_status(now);
    }
    if (!s_alert_acked && s_alert_page >= 0 && s_level[s_alert_page] > ALERT_NOTICE) {
        lv_color_t c = s_level[s_alert_page] == ALERT_CRIT ? C_CRIT : C_WARN;
        th_ring_prepare(&s_alert_ring[0], 177, 2, c);
        th_ring_prepare(&s_alert_ring[1], 174, 1, c);
    }
    lv_obj_invalidate(s_overlay);
}

static void on_click(lv_event_t *e)
{
    lv_point_t pt;
    lv_indev_get_point(lv_indev_active(), &pt);
    if (!s_alert_acked && s_alert_page == s_cur) {
        s_alert_acked = true;
        platform_alert_cleared();
        return;
    }
    page_t *p = &s_pages[s_cur];
    if (p->type->tap) p->type->tap(p, pt.x, pt.y);
}

static void on_gesture(lv_event_t *e)
{
    lv_dir_t dir = lv_indev_get_gesture_dir(lv_indev_active());
    if (dir == LV_DIR_LEFT) ui_knob(1);
    else if (dir == LV_DIR_RIGHT) ui_knob(-1);
}

void ui_init(const ui_page_cfg_t *cfg, int n)
{
    lv_obj_t *scr = lv_screen_active();
    lv_obj_remove_style_all(scr);
    lv_obj_set_style_bg_color(scr, C_BG, 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(scr, on_click, LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(scr, on_gesture, LV_EVENT_GESTURE, NULL);

    s_n = 0;
    for (int i = 0; i < n && s_n < MAX_PAGES; i++) {
        const page_type_t *t = page_type_find(cfg[i].type);
        if (!t) continue;
        page_t *p = &s_pages[s_n];
        p->type = t;
        snprintf(p->title, sizeof p->title, "%s", cfg[i].title ? cfg[i].title : t->label);
        p->root = lv_obj_create(scr);
        lv_obj_remove_style_all(p->root);
        lv_obj_set_size(p->root, SCR_W, SCR_H);
        lv_obj_remove_flag(p->root, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_flag(p->root, LV_OBJ_FLAG_EVENT_BUBBLE);
        t->create(p, p->root);
        place(s_n, s_n == 0 ? 0 : SCR_W);
        s_n++;
    }
    s_cur = 0;

    s_overlay = lv_obj_create(scr);
    lv_obj_remove_style_all(s_overlay);
    lv_obj_set_size(s_overlay, SCR_W, SCR_H);
    lv_obj_remove_flag(s_overlay, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_overlay, overlay_draw, LV_EVENT_DRAW_MAIN, NULL);

    s_last = s_start = lv_tick_get();
    lv_timer_create(frame, FRAME_MS, NULL);
}
