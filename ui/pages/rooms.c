// Rooms page: one room at a time, as the climate controller sees it. A thermostat dial
// runs round the rim from 16 to 28 C: a white marker for the room's
// temperature, an amber handle for its target, and the span between them lit
// in the colour of what the brain is doing about it (heat, cool, or nothing).
// The centre is the temperature, with target and humidity under it; below
// that, the A/C and radiator state and the brain's own one-line reason.
// Tap the left or right edge to flip rooms. Tap the middle to edit the
// target: the bezel then moves it half a degree a click, and it is saved after
// three seconds untouched, or on another tap. The knob writes the room's
// target helper in Home Assistant, never the A/C or valves directly.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../model.h"
#include "../page.h"
#include "../theme.h"

#define DIAL_R 150
#define T_LO 16.0f
#define T_HI 28.0f
#define A_LO 225.0f   // 16 C at 7:30 on a clock face
#define A_SPAN 270.0f
#define EDIT_IDLE_MS 3000
#define PENDING_MS 120000

typedef struct {
    lv_obj_t *canvas;
    uint32_t now;
    int cur;                 // room index
    int n;
    room_t rooms[MODEL_MAX_ROOMS];
    bool up;
    uint32_t poll_ms;
    // editing
    bool editing;
    float edit_val;
    uint32_t edit_ms;        // last bezel move or start
    // a saved change waiting for the brain to report it
    bool pending;
    char pend_room[16];
    float pend_val;
    uint32_t pend_ms, pend_seq;
    int8_t pend_result;      // 0 waiting, 1 written, -1 failed
    uint32_t result_ms;
    // animation
    float temp_disp, target_disp, handle_disp;
    uint32_t flip_ms;
    int flip_dir;
} rooms_t;

static float t2a(float t)
{
    float k = (t - T_LO) / (T_HI - T_LO);
    k = fminf(1, fmaxf(0, k));
    return A_LO + A_SPAN * k;
}

static lv_color_t action_colour(const room_t *r)
{
    if (!strcmp(r->status, "heating") || r->heat_demand || r->valves == 1 || !strcmp(r->ac_mode, "heat")) return C_HEAT;
    if (!strcmp(r->status, "cooling") || !strcmp(r->ac_mode, "cool")) return C_COOL;
    return C_DIM;
}

static const room_t *cur_room(rooms_t *s) { return s->n ? &s->rooms[s->cur < s->n ? s->cur : 0] : NULL; }

static void upper(char *d, int n, const char *src)
{
    int i = 0;
    for (; src[i] && i < n - 1; i++) d[i] = (src[i] >= 'a' && src[i] <= 'z') ? src[i] - 32 : src[i];
    d[i] = 0;
}

static void draw_dial(lv_layer_t *l, rooms_t *s, const room_t *r)
{
    // Track and scale.
    th_arc(l, SCR_CX, SCR_CY, DIAL_R, 2, A_LO, A_LO + A_SPAN, C_FAINT, LV_OPA_COVER);
    for (int i = 0; i <= (int)((T_HI - T_LO) * 2); i++) {
        float t = T_LO + i * 0.5f, a = t2a(t);
        bool major = i % 4 == 0;
        th_line(l, th_px(DIAL_R + 6, a), th_py(DIAL_R + 6, a), th_px(DIAL_R + (major ? 16 : 11), a),
                th_py(DIAL_R + (major ? 16 : 11), a), 1, major ? C_DIM : C_FAINT, LV_OPA_COVER);
        if (major) {
            char b[4];
            snprintf(b, sizeof b, "%d", (int)t);
            th_text(l, &font_mono_11, (int)th_px(DIAL_R - 14, a), (int)th_py(DIAL_R - 14, a) - 7, 24,
                    LV_TEXT_ALIGN_CENTER, C_DIM, LV_OPA_COVER, 0, b);
        }
    }
    if (!r || isnan(s->temp_disp)) return;
    // The gap between temperature and target, lit by what is being done about it.
    float a_t = t2a(s->temp_disp), a_s = t2a(s->handle_disp);
    lv_color_t c = s->editing ? C_ACCENT : action_colour(r);
    float a0 = fminf(a_t, a_s), a1 = fmaxf(a_t, a_s);
    if (a1 - a0 > 0.5f) th_arc(l, SCR_CX, SCR_CY, DIAL_R, 6, a0, a1, c, s->editing ? 140 : 220);
    // Temperature marker: a short white bar across the track.
    th_line(l, th_px(DIAL_R - 9, a_t), th_py(DIAL_R - 9, a_t), th_px(DIAL_R + 9, a_t), th_py(DIAL_R + 9, a_t), 3, C_INK,
            LV_OPA_COVER);
    // Target handle.
    float hx = th_px(DIAL_R, a_s), hy = th_py(DIAL_R, a_s);
    if (s->editing) {
        float k = (s->now % 1200) / 1200.0f;
        th_ring(l, hx, hy, (int)(10 + 6 * k), 2, C_ACCENT, (lv_opa_t)(200 * (1 - k)));
    }
    th_dot(l, hx, hy, s->editing ? 8 : 6, C_ACCENT, LV_OPA_COVER);
    th_dot(l, hx, hy, 2, C_BG, LV_OPA_COVER);
}

static void draw_header(lv_layer_t *l, rooms_t *s, const room_t *r)
{
    char b[24];
    uint32_t since = s->now - s->flip_ms;
    float k = since < 300 ? since / 300.0f : 1;
    int dx = (int)(s->flip_dir * 24 * (1 - k));
    lv_opa_t o = (lv_opa_t)(255 * k);
    th_text(l, &font_sans_20, SCR_CX + dx, 50, 200, LV_TEXT_ALIGN_CENTER, C_INK, o, 2, r ? r->name : "ROOMS");
    if (r) {
        upper(b, sizeof b, r->status[0] ? r->status : "--");
        th_text(l, &font_mono_11, SCR_CX + dx, 76, 160, LV_TEXT_ALIGN_CENTER, action_colour(r), o, 3, b);
    }
    // Room pips.
    for (int i = 0; i < s->n; i++) {
        float x = SCR_CX + (i - (s->n - 1) / 2.0f) * 10;
        th_dot(l, x, 96, i == s->cur ? 3 : 2, i == s->cur ? C_ACCENT : C_FAINT, LV_OPA_COVER);
    }
    // Flip hints either side of the pips.
    float half = (s->n - 1) / 2.0f * 10 + 16;
    th_text(l, &font_mono_13, (int)(SCR_CX - half), 88, 16, LV_TEXT_ALIGN_CENTER, C_DIM, LV_OPA_COVER, 0, "<");
    th_text(l, &font_mono_13, (int)(SCR_CX + half), 88, 16, LV_TEXT_ALIGN_CENTER, C_DIM, LV_OPA_COVER, 0, ">");
}

static void draw_centre(lv_layer_t *l, rooms_t *s, const room_t *r)
{
    char b[48];
    if (s->editing) {
        th_text(l, &font_mono_11, SCR_CX, 112, 160, LV_TEXT_ALIGN_CENTER, C_ACCENT, LV_OPA_COVER, 3, "SET TARGET");
        snprintf(b, sizeof b, "%.1f", s->edit_val);
        th_text(l, &font_mono_52, SCR_CX - 8, 128, 200, LV_TEXT_ALIGN_CENTER, C_ACCENT, LV_OPA_COVER, 0, b);
        th_text(l, &font_mono_16, SCR_CX + 66, 134, 20, LV_TEXT_ALIGN_LEFT, C_ACCENT, LV_OPA_COVER, 0, "\xC2\xB0");
        snprintf(b, sizeof b, "ROOM %.1f\xC2\xB0", r->temp);
        th_text(l, &font_mono_13, SCR_CX, 196, 160, LV_TEXT_ALIGN_CENTER, C_SUB, LV_OPA_COVER, 0, b);
        // Countdown to save: a short bar that shrinks to nothing.
        float k = 1 - fminf(1, (float)(s->now - s->edit_ms) / EDIT_IDLE_MS);
        th_line(l, SCR_CX - 50, 226, SCR_CX + 50, 226, 2, C_FAINT, LV_OPA_COVER);
        if (k > 0.01f) th_line(l, SCR_CX - 50 * k, 226, SCR_CX + 50 * k, 226, 2, C_ACCENT, LV_OPA_COVER);
        th_text(l, &font_mono_11, SCR_CX, 236, 260, LV_TEXT_ALIGN_CENTER, C_DIM, LV_OPA_COVER, 0, "turn to change, tap to save");
        return;
    }
    if (isnan(r->temp)) snprintf(b, sizeof b, "--");
    else snprintf(b, sizeof b, "%.1f", s->temp_disp);
    th_text(l, &font_mono_52, SCR_CX - 8, 118, 200, LV_TEXT_ALIGN_CENTER, C_INK, LV_OPA_COVER, 0, b);
    th_text(l, &font_mono_16, SCR_CX + 66, 124, 20, LV_TEXT_ALIGN_LEFT, C_SUB, LV_OPA_COVER, 0, "\xC2\xB0");

    // Target (or the value just saved and not yet taken up) and humidity.
    bool pend = s->pending && !strcmp(s->pend_room, r->id);
    if (pend) snprintf(b, sizeof b, "\xE2\x86\x92 %.1f\xC2\xB0", s->pend_val);
    else snprintf(b, sizeof b, "\xE2\x86\x92 %.1f\xC2\xB0", r->target);
    th_text(l, &font_mono_16, SCR_CX - 4, 186, 120, LV_TEXT_ALIGN_RIGHT, C_ACCENT, LV_OPA_COVER, 0, b);
    if (isnan(r->humidity)) snprintf(b, sizeof b, "-- RH");
    else snprintf(b, sizeof b, "%.0f%% RH", r->humidity);
    th_text(l, &font_mono_16, SCR_CX + 10, 186, 120, LV_TEXT_ALIGN_LEFT, C_SUB, LV_OPA_COVER, 0, b);
    if (pend) {
        const char *m = s->pend_result < 0 ? "SAVE FAILED" : s->pend_result > 0 ? "SAVED, WAITING FOR THE BRAIN" : "SAVING";
        th_text(l, &font_mono_11, SCR_CX, 208, 240, LV_TEXT_ALIGN_CENTER, s->pend_result < 0 ? C_CRIT : C_DIM,
                LV_OPA_COVER, 1, m);
    }
}

static void draw_devices(lv_layer_t *l, rooms_t *s, const room_t *r)
{
    char b[32];
    // A/C chip (left) and radiator chip (right), on one line.
    const int y = 232;
    lv_color_t ac = !strcmp(r->ac_mode, "cool") ? C_COOL : !strcmp(r->ac_mode, "heat") ? C_HEAT
                    : (!r->ac_mode[0] || !strcmp(r->ac_mode, "off")) ? C_FAINT : C_SUB;
    char mode[12];
    upper(mode, sizeof mode, r->ac_mode[0] ? r->ac_mode : "none");
    if (!strcmp(mode, "FAN_ONLY")) snprintf(mode, sizeof mode, "DRY");
    if (!isnan(r->ac_setpoint)) snprintf(b, sizeof b, "%s %.1f", mode, r->ac_setpoint);
    else snprintf(b, sizeof b, "%s", mode);
    th_dot(l, SCR_CX - 104, y + 13, 4, ac, LV_OPA_COVER);
    th_text(l, &font_mono_11, SCR_CX - 96, y, 80, LV_TEXT_ALIGN_LEFT, C_DIM, LV_OPA_COVER, 2, "A/C");
    th_text(l, &font_mono_13, SCR_CX - 96, y + 13, 90, LV_TEXT_ALIGN_LEFT, C_INK, LV_OPA_COVER, 0, b);

    lv_color_t rc = r->valves == 1 ? C_HEAT : r->valves == 0 ? C_FAINT : C_HAIR;
    const char *rv = r->valves == 1 ? "OPEN" : r->valves == 0 ? (r->heat_demand ? "CALLING" : "SHUT") : "NONE";
    th_dot(l, SCR_CX + 20, y + 13, 4, rc, LV_OPA_COVER);
    th_text(l, &font_mono_11, SCR_CX + 28, y, 90, LV_TEXT_ALIGN_LEFT, C_DIM, LV_OPA_COVER, 2, "RADIATOR");
    th_text(l, &font_mono_13, SCR_CX + 28, y + 13, 90, LV_TEXT_ALIGN_LEFT, r->valves < 0 ? C_DIM : C_INK,
            LV_OPA_COVER, 0, rv);

    // The brain's reason, wrapped over two short lines.
    lv_draw_label_dsc_t d;
    lv_draw_label_dsc_init(&d);
    d.font = &font_mono_11;
    d.color = C_DIM;
    d.align = LV_TEXT_ALIGN_CENTER;
    // "->" as an arrow, so it never splits across lines.
    char why[96];
    int o = 0;
    for (const char *q = r->why; *q && o < (int)sizeof why - 4; q++) {
        if (q[0] == '-' && q[1] == '>') {
            memcpy(why + o, "\xE2\x86\x92", 3);
            o += 3;
            q++;
        } else {
            why[o++] = *q;
        }
    }
    why[o] = 0;
    d.text = why;
    d.text_local = 1;
    lv_area_t a = {SCR_CX - 80, 264, SCR_CX + 80, 292};  // two lines
    lv_draw_label(l, &d, &a);
}

static void draw_body(lv_event_t *e)
{
    rooms_t *s = lv_event_get_user_data(e);
    lv_layer_t *l = lv_event_get_layer(e);
    const room_t *r = cur_room(s);
    if (!r) {
        draw_dial(l, s, NULL);
        th_text(l, &font_mono_16, SCR_CX, SCR_CY - 24, 260, LV_TEXT_ALIGN_CENTER, C_SUB, LV_OPA_COVER, 2,
                s->poll_ms ? "NO ROOMS" : "CLIMATE");
        th_text(l, &font_mono_11, SCR_CX, SCR_CY + 2, 240, LV_TEXT_ALIGN_CENTER, C_DIM, LV_OPA_COVER, 1,
                s->poll_ms ? "check the rooms list in settings" : "waiting for data");
        return;
    }
    draw_dial(l, s, r);
    draw_header(l, s, r);
    draw_centre(l, s, r);
    if (!s->editing) draw_devices(l, s, r);
    if (!s->up && s->poll_ms)
        th_text(l, &font_mono_11, SCR_CX, 312, 120, LV_TEXT_ALIGN_CENTER, C_CRIT, LV_OPA_COVER, 2, "STALE");
}

TH_PAGE_DRAW(draw_cb, draw_body, (lv_obj_t *)lv_event_get_current_target(e))

static void create(page_t *p, lv_obj_t *root)
{
    rooms_t *s = calloc(1, sizeof *s);
    p->state = s;
    s->temp_disp = s->target_disp = s->handle_disp = NAN;
    s->canvas = lv_obj_create(root);
    lv_obj_remove_style_all(s->canvas);
    lv_obj_set_size(s->canvas, SCR_W, SCR_H);
    lv_obj_remove_flag(s->canvas, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s->canvas, draw_cb, LV_EVENT_DRAW_MAIN, s);
}

static void destroy(page_t *p) { free(p->state); }

static void save(rooms_t *s)
{
    const room_t *r = cur_room(s);
    s->editing = false;
    if (!r || fabsf(s->edit_val - r->target) < 0.01f) return;
    model_lock();
    g_model.room_set.seq++;
    snprintf(g_model.room_set.room, sizeof g_model.room_set.room, "%s", r->id);
    g_model.room_set.value = s->edit_val;
    s->pend_seq = g_model.room_set.seq;
    model_unlock();
    s->pending = true;
    snprintf(s->pend_room, sizeof s->pend_room, "%s", r->id);
    s->pend_val = s->edit_val;
    s->pend_ms = s->now;
    s->pend_result = 0;
}

static void tick(page_t *p, uint32_t now, uint32_t dt, bool visible)
{
    rooms_t *s = p->state;
    s->now = now;
    char keep[16] = "";
    if (s->n) snprintf(keep, sizeof keep, "%s", s->rooms[s->cur].id);
    model_lock();
    s->up = g_model.climate_up;
    s->poll_ms = g_model.climate_poll_ms;
    s->n = g_model.n_rooms;
    memcpy(s->rooms, g_model.rooms, sizeof(room_t) * s->n);
    if (s->pending && g_model.room_set.done_seq == s->pend_seq && !s->pend_result) {
        s->pend_result = g_model.room_set.result;
        s->result_ms = now;
    }
    model_unlock();
    // Stay on the same room if the list changes under us.
    for (int i = 0; i < s->n; i++)
        if (keep[0] && !strcmp(s->rooms[i].id, keep)) s->cur = i;
    if (s->cur >= s->n) s->cur = 0;

    const room_t *r = cur_room(s);
    if (s->pending) {
        const room_t *pr = NULL;
        for (int i = 0; i < s->n; i++)
            if (!strcmp(s->rooms[i].id, s->pend_room)) pr = &s->rooms[i];
        bool taken = pr && fabsf(pr->target - s->pend_val) < 0.05f;
        if (taken || now - s->pend_ms > PENDING_MS || (s->pend_result < 0 && now - s->result_ms > 6000))
            s->pending = false;
    }
    if (s->editing && now - s->edit_ms > EDIT_IDLE_MS) save(s);

    if (r) {
        bool pend = s->pending && !strcmp(s->pend_room, r->id);
        float tgt = s->editing ? s->edit_val : pend ? s->pend_val : r->target;
        if (isnan(s->temp_disp) || isnan(s->handle_disp)) {
            s->temp_disp = r->temp;
            s->handle_disp = tgt;
        }
        s->temp_disp = th_ease(s->temp_disp, r->temp, dt, 250);
        s->handle_disp = th_ease(s->handle_disp, tgt, dt, s->editing ? 70 : 250);
    }
    if (!visible && s->editing) save(s);
    if (visible) lv_obj_invalidate(s->canvas);
}

static void tap(page_t *p, int x, int y)
{
    rooms_t *s = p->state;
    const room_t *r = cur_room(s);
    if (!r) return;
    if (s->editing) {
        save(s);
        return;
    }
    if (x < 90 || x > SCR_W - 90) {
        int d = x < 90 ? -1 : 1;
        s->cur = (s->cur + d + s->n) % s->n;
        s->flip_ms = s->now;
        s->flip_dir = d;
        r = cur_room(s);
        s->temp_disp = r->temp;  // jump, then the dial eases from there
        s->handle_disp = r->target;
        return;
    }
    if (isnan(r->target)) return;
    s->editing = true;
    s->edit_val = s->pending && !strcmp(s->pend_room, r->id) ? s->pend_val : r->target;
    s->edit_ms = s->now;
}

static bool knob(page_t *p, int delta)
{
    rooms_t *s = p->state;
    if (!s->editing) return false;
    const room_t *r = cur_room(s);
    float step = r && r->t_step > 0 ? r->t_step : 0.5f;
    float lo = r && r->t_max > r->t_min ? r->t_min : T_LO, hi = r && r->t_max > r->t_min ? r->t_max : T_HI;
    s->edit_val = fminf(hi, fmaxf(lo, s->edit_val + delta * step));
    s->edit_ms = s->now;
    return true;
}

static alert_level_t status(page_t *p, char *why, int len)
{
    rooms_t *s = p->state;
    if (s->poll_ms && !s->up && s->now - s->poll_ms > 120000) {
        snprintf(why, len, "climate controller not answering");
        return ALERT_WARN;
    }
    return ALERT_NONE;
}

const page_type_t page_rooms = {
    .type = "rooms", .label = "Rooms", .create = create, .destroy = destroy,
    .tick = tick, .tap = tap, .knob = knob, .status = status,
};
