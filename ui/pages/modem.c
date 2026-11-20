// Modem page: pdn-soundmodem's own view of the channel, from its Prometheus
// metrics. A ladder of the stations it hears, most recently heard first, each
// with its last SNR as a bar from a centre spine; rows glide when the order
// changes and flash when a station is heard again. Two thin rim gauges show how
// hard the receiver is working (left) and whether it keeps up with real time
// (right). Tap to sort by signal strength instead.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../model.h"
#include "../page.h"
#include "../theme.h"

#define ROWS 6
#define ROWS_BOTTOM (ROW_Y0 + ROWS * ROW_H)
#define ROW_H 34
#define ROW_Y0 70
#define SPINE_X (SCR_CX + 6)
#define SNR_PX 4.2f      // pixels per dB
#define SNR_MAX 30.0f

typedef struct {
    char key[40];        // call + mode
    float y;             // displayed row position, eased
    float bar;           // displayed bar length, eased
    uint32_t flash_ms;
    uint32_t last_heard;
    bool live;
} row_t;

typedef struct {
    lv_obj_t *canvas;
    uint32_t now, shown_ms;
    bool was_visible, by_snr;
    float busy_disp, rt_disp;
    model_t m;            // snapshot of the modem part (copied whole for simplicity)
    int order[MODEL_MAX_MSTATIONS];
    row_t rows[MODEL_MAX_MSTATIONS];
    th_scenery_t scenery;  // draw_scale(): gauge tracks and ticks, spine and dB scale
} modem_t;

static const char *mode_short(const char *mode, char *b, int n)
{
    // "bpsk300-il2pc" -> "BPSK300", "afsk1200" -> "AFSK1200"
    int i = 0;
    for (; mode[i] && mode[i] != '-' && i < n - 1; i++) b[i] = (mode[i] >= 'a' && mode[i] <= 'z') ? mode[i] - 32 : mode[i];
    b[i] = 0;
    return b;
}

static void fmt_age(char *b, int n, uint32_t ms)
{
    uint32_t s = ms / 1000;
    if (s < 60) snprintf(b, n, "%us", (unsigned)s);
    else if (s < 3600) snprintf(b, n, "%um", (unsigned)(s / 60));
    else snprintf(b, n, "%uh", (unsigned)(s / 3600));
}

static row_t *row_for(modem_t *s, const mstation_t *st)
{
    char key[40];
    snprintf(key, sizeof key, "%s/%s", st->call, st->mode);
    row_t *free_row = NULL;
    for (int i = 0; i < MODEL_MAX_MSTATIONS; i++) {
        if (s->rows[i].live && !strcmp(s->rows[i].key, key)) return &s->rows[i];
        if (!s->rows[i].live && !free_row) free_row = &s->rows[i];
    }
    if (!free_row) return NULL;
    memset(free_row, 0, sizeof *free_row);
    snprintf(free_row->key, sizeof free_row->key, "%s", key);
    free_row->live = true;
    free_row->y = ROW_Y0 + ROWS * ROW_H + 20;  // slide in from below
    return free_row;
}

static int cmp_recent(const void *a, const void *b, void *ctx)
{
    const mstation_t *x = &((model_t *)ctx)->mstations[*(const int *)a];
    const mstation_t *y = &((model_t *)ctx)->mstations[*(const int *)b];
    if (x->heard_ms != y->heard_ms) return x->heard_ms > y->heard_ms ? -1 : 1;
    return x->frames > y->frames ? -1 : x->frames < y->frames;
}

static int cmp_snr(const void *a, const void *b, void *ctx)
{
    const mstation_t *x = &((model_t *)ctx)->mstations[*(const int *)a];
    const mstation_t *y = &((model_t *)ctx)->mstations[*(const int *)b];
    float sx = isnan(x->snr_last) ? -99 : x->snr_last, sy = isnan(y->snr_last) ? -99 : y->snr_last;
    return sx > sy ? -1 : sx < sy;
}

// qsort with a context argument, portable enough for both targets.
static model_t *s_sort_ctx;
static int (*s_sort_fn)(const void *, const void *, void *);
static int sort_tramp(const void *a, const void *b) { return s_sort_fn(a, b, s_sort_ctx); }

// Everything that never moves: gauge tracks and ticks, the keeping-up band,
// the spine and its dB scale. Drawn first, onto black, so it can be cached.
static void draw_scale(lv_layer_t *l)
{
    char b[8];
    const float l0 = 232, l1 = 308;
    th_arc(l, SCR_CX, SCR_CY, 168, 3, l0, l1, C_FAINT, LV_OPA_COVER);
    for (int i = 0; i <= 4; i++) {
        float a = l0 + (l1 - l0) * i / 4;
        th_line(l, th_px(160, a), th_py(160, a), th_px(164, a), th_py(164, a), 1, C_DIM, LV_OPA_COVER);
    }
    const float r0 = 52, r1 = 128, rm = 90;
    th_arc(l, SCR_CX, SCR_CY, 168, 1, r0, r1, C_FAINT, LV_OPA_COVER);
    th_arc(l, SCR_CX, SCR_CY, 168, 3, rm - 6, rm + 6, C_OK, 120);  // keeping-up band
    for (int i = 0; i <= 4; i++) {
        float a = r0 + (r1 - r0) * i / 4;
        th_line(l, th_px(160, a), th_py(160, a), th_px(164, a), th_py(164, a), 1, C_DIM, LV_OPA_COVER);
    }
    th_line(l, SPINE_X, ROW_Y0 - 8, SPINE_X, ROW_Y0 + ROWS * ROW_H - 6, 1, C_FAINT, LV_OPA_COVER);
    for (int db = 10; db <= 20; db += 10) {
        float x = SPINE_X + db * SNR_PX;
        th_line(l, x, ROW_Y0 - 8, x, ROW_Y0 - 3, 1, C_DIM, LV_OPA_COVER);
        snprintf(b, sizeof b, "%d", db);
        th_text(l, &font_mono_11, (int)x, ROW_Y0 - 22, 30, LV_TEXT_ALIGN_CENTER, C_DIM, LV_OPA_COVER, 0, b);
    }
    th_text(l, &font_mono_11, SPINE_X + 2, ROW_Y0 - 22, 30, LV_TEXT_ALIGN_LEFT, C_DIM, LV_OPA_COVER, 0, "dB");
}

static void draw_gauges(lv_layer_t *l, modem_t *s)
{
    // Left: receive loop busy, 0..100 % over 232..308 degrees (bottom to top).
    const float l0 = 232, l1 = 308;
    float k = fminf(1, fmaxf(0, s->busy_disp / 100.0f));
    lv_color_t bc = s->busy_disp > 85 ? C_CRIT : s->busy_disp > 65 ? C_WARN : C_INK;
    th_arc(l, SCR_CX, SCR_CY, 168, 3, l0, l0 + (l1 - l0) * k, bc, LV_OPA_COVER);
    // Right: real-time ratio as a centre-zero meter, 0.98..1.02 over 52..128 degrees.
    const float r1 = 128, rm = 90;
    float d = (s->rt_disp - 1.0f) / 0.02f;
    d = fminf(1, fmaxf(-1, d));
    float a = rm - d * (r1 - rm);  // faster than real time reads upward
    lv_color_t rc = s->m.behind ? C_CRIT : fabsf(s->rt_disp - 1) > 0.005f ? C_WARN : C_INK;
    th_line(l, th_px(158, a), th_py(158, a), th_px(174, a), th_py(174, a), 3, rc, LV_OPA_COVER);
}

static void draw_rows(lv_layer_t *l, modem_t *s)
{
    char b[32], mb[16];

    int shown = s->m.n_mstations < ROWS ? s->m.n_mstations : ROWS;
    for (int r = 0; r < s->m.n_mstations; r++) {
        const mstation_t *st = &s->m.mstations[s->order[r]];
        row_t *rw = row_for(s, st);
        if (!rw || r >= ROWS + 1) continue;
        float y = rw->y;
        if (y > ROW_Y0 + ROWS * ROW_H) continue;
        lv_opa_t o = r < shown ? LV_OPA_COVER : 0;
        if (y > ROW_Y0 + (ROWS - 1) * ROW_H) o = (lv_opa_t)fmaxf(0, 255 * (ROW_Y0 + ROWS * ROW_H - y) / ROW_H);

        uint32_t since = s->now - rw->flash_ms;
        bool flash = rw->flash_ms && since < 1500;
        float fk = flash ? 1 - since / 1500.0f : 0;

        th_text(l, &font_mono_16, SPINE_X - 10, (int)y, 120, LV_TEXT_ALIGN_RIGHT, flash ? C_ACCENT : C_INK, o, 0,
                st->call);
        th_text(l, &font_mono_11, SPINE_X - 10, (int)y + 17, 120, LV_TEXT_ALIGN_RIGHT, C_DIM, o, 1,
                mode_short(st->mode, mb, sizeof mb));
        if (st->heard_ms) {
            fmt_age(b, sizeof b, s->now - st->heard_ms);
            th_text(l, &font_mono_11, SPINE_X - 10 - 98, (int)y + 3, 40, LV_TEXT_ALIGN_RIGHT, C_DIM, o, 0, b);
        }
        // SNR bar from the spine.
        if (!isnan(st->snr_last)) {
            float len = rw->bar;
            lv_color_t c = st->snr_last >= 10 ? C_INK : st->snr_last >= 4 ? C_WARN : C_CRIT;
            th_line(l, SPINE_X + 1, y + 9, SPINE_X + 1 + len, y + 9, 6, c, (lv_opa_t)(o * (0.8f + 0.2f * fk)));
            if (flash) th_line(l, SPINE_X + 1, y + 9, SPINE_X + 1 + len, y + 9, 7, C_ACCENT, (lv_opa_t)(o * fk * 0.8f));
            // Lifetime mean as a hairline tick across the bar.
            float end = len;
            if (!isnan(st->snr_mean)) {
                float mlen = fminf(SNR_MAX, fmaxf(0, st->snr_mean)) * SNR_PX;
                th_line(l, SPINE_X + 1 + mlen, y + 2, SPINE_X + 1 + mlen, y + 16, 1, C_ACCENT, o);
                if (mlen > end) end = mlen;
            }
            snprintf(b, sizeof b, "%.0f", st->snr_last);
            th_text(l, &font_mono_13, (int)(SPINE_X + 7 + end), (int)y + 2, 40, LV_TEXT_ALIGN_LEFT, C_SUB, o, 0, b);
        }
        // Reed-Solomon repairs in the last ten minutes: an amber pip under the call.
        if (st->corrected_10m) {
            snprintf(b, sizeof b, "RS%u", (unsigned)st->corrected_10m);
            th_text(l, &font_mono_11, SPINE_X + 8, (int)y + 18, 60, LV_TEXT_ALIGN_LEFT, C_ACCENT, o, 0, b);
        }
    }
}

static void draw_body(lv_event_t *e)
{
    modem_t *s = lv_event_get_user_data(e);
    lv_layer_t *l = lv_event_get_layer(e);
    char b[64];
    if (!s->m.modem_up && !s->m.n_mstations) {
        th_text(l, &font_mono_22, SCR_CX, SCR_CY - 30, 240, LV_TEXT_ALIGN_CENTER, C_CRIT, LV_OPA_COVER, 3, "NO METRICS");
        th_text(l, &font_mono_11, SCR_CX, SCR_CY + 2, 220, LV_TEXT_ALIGN_CENTER, C_DIM, LV_OPA_COVER, 1,
                "metrics endpoint not answering");
        return;
    }
    if (!th_scenery_blit(l, &s->scenery, s->canvas)) draw_scale(l);
    draw_gauges(l, s);
    draw_rows(l, s);

    // Header: title while fresh, then the station count.
    uint32_t age = s->now - s->shown_ms;
    const char *name = s->m.modem_name[0] ? s->m.modem_name : "MODEM";
    if (age < 3500) {
        lv_opa_t o = age < 2500 ? LV_OPA_COVER : (lv_opa_t)(255 * (3500 - age) / 1000);
        snprintf(b, sizeof b, "MODEM  %s", name);
        th_text(l, &font_sans_15, SCR_CX, 22, 220, LV_TEXT_ALIGN_CENTER, C_SUB, o, 4, b);
    } else {
        snprintf(b, sizeof b, "%d STATIONS", s->m.n_mstations);
        th_text(l, &font_mono_11, SCR_CX, 26, 200, LV_TEXT_ALIGN_CENTER, C_DIM, LV_OPA_COVER, 2, b);
    }

    // Footer: gauge readings, then transmit stalls and input losses in the last hour.
    lv_color_t bc = s->busy_disp > 85 ? C_CRIT : s->busy_disp > 65 ? C_WARN : C_INK;
    lv_color_t rc = s->m.behind ? C_CRIT : fabsf(s->rt_disp - 1) > 0.005f ? C_WARN : C_INK;
    snprintf(b, sizeof b, "%.0f%%", s->busy_disp);
    th_text(l, &font_mono_13, SCR_CX - 48, 282, 50, LV_TEXT_ALIGN_RIGHT, bc, LV_OPA_COVER, 0, b);
    th_text(l, &font_mono_11, SCR_CX - 44, 284, 40, LV_TEXT_ALIGN_LEFT, C_DIM, LV_OPA_COVER, 1, "CPU");
    snprintf(b, sizeof b, "%.3f", s->rt_disp);
    th_text(l, &font_mono_13, SCR_CX + 36, 282, 60, LV_TEXT_ALIGN_RIGHT, rc, LV_OPA_COVER, 0, b);
    th_text(l, &font_mono_11, SCR_CX + 40, 284, 30, LV_TEXT_ALIGN_LEFT, C_DIM, LV_OPA_COVER, 1, "RT");
    lv_color_t sc = s->m.tx_stalls_1h > 10 ? C_WARN : C_DIM;
    snprintf(b, sizeof b, "TX STALL %u/h", (unsigned)s->m.tx_stalls_1h);
    th_text(l, &font_mono_11, SCR_CX - 6, 302, 100, LV_TEXT_ALIGN_RIGHT, sc, LV_OPA_COVER, 0, b);
    snprintf(b, sizeof b, "LOSS %u/h", (unsigned)s->m.input_losses_1h);
    th_text(l, &font_mono_11, SCR_CX + 6, 302, 100, LV_TEXT_ALIGN_LEFT,
            s->m.input_losses_1h ? C_WARN : C_DIM, LV_OPA_COVER, 0, b);
    th_text(l, &font_mono_11, SCR_CX, 318, 160, LV_TEXT_ALIGN_CENTER, C_DIM, 160, 2,
            s->by_snr ? "BY SIGNAL" : "BY LAST HEARD");
    if (!s->m.modem_up)
        th_text(l, &font_mono_11, SCR_CX, 46, 200, LV_TEXT_ALIGN_CENTER, C_CRIT, LV_OPA_COVER, 2, "STALE");
}

static void draw_cb(lv_event_t *e);

static void create(page_t *p, lv_obj_t *root)
{
    modem_t *s = calloc(1, sizeof *s);
    p->state = s;
    s->rt_disp = 1.0f;
    s->canvas = lv_obj_create(root);
    lv_obj_remove_style_all(s->canvas);
    lv_obj_set_size(s->canvas, SCR_W, SCR_H);
    lv_obj_remove_flag(s->canvas, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s->canvas, draw_cb, LV_EVENT_DRAW_MAIN, s);
}

TH_PAGE_DRAW(draw_cb, draw_body, (lv_obj_t *)lv_event_get_current_target(e))

static void destroy(page_t *p)
{
    modem_t *s = p->state;
    if (s->scenery.buf) lv_draw_buf_destroy(s->scenery.buf);
    free(s);
}

static void tick(page_t *p, uint32_t now, uint32_t dt, bool visible)
{
    modem_t *s = p->state;
    s->now = now;
    if (visible && !s->was_visible) s->shown_ms = now;
    s->was_visible = visible;

    model_lock();
    memcpy(s->m.modem_name, g_model.modem_name, sizeof s->m.modem_name);
    s->m.modem_up = g_model.modem_up;
    s->m.n_mstations = g_model.n_mstations;
    memcpy(s->m.mstations, g_model.mstations, sizeof(mstation_t) * g_model.n_mstations);
    s->m.rt_ratio = g_model.rt_ratio;
    s->m.busy_pct = g_model.busy_pct;
    s->m.behind = g_model.behind;
    s->m.tx_stalls_1h = g_model.tx_stalls_1h;
    s->m.input_losses_1h = g_model.input_losses_1h;
    model_unlock();

    for (int i = 0; i < s->m.n_mstations; i++) s->order[i] = i;
    s_sort_ctx = &s->m;
    s_sort_fn = s->by_snr ? cmp_snr : cmp_recent;
    qsort(s->order, s->m.n_mstations, sizeof(int), sort_tramp);

    for (int r = 0; r < s->m.n_mstations; r++) {
        const mstation_t *st = &s->m.mstations[s->order[r]];
        row_t *rw = row_for(s, st);
        if (!rw) continue;
        float ty = ROW_Y0 + (r < ROWS ? r : ROWS) * ROW_H;
        rw->y = th_ease(rw->y, ty, dt, 140);
        float tb = isnan(st->snr_last) ? 0 : fminf(SNR_MAX, fmaxf(0, st->snr_last)) * SNR_PX;
        rw->bar = th_ease(rw->bar, tb, dt, 220);
        if (st->heard_ms && st->heard_ms != rw->last_heard) {
            if (rw->last_heard) rw->flash_ms = now;
            rw->last_heard = st->heard_ms;
        }
    }
    s->busy_disp = th_ease(s->busy_disp, s->m.busy_pct, dt, 400);
    s->rt_disp = th_ease(s->rt_disp, s->m.rt_ratio > 0 ? s->m.rt_ratio : 1.0f, dt, 400);
    if (visible) {
        if (!s->scenery.buf) {
            lv_layer_t *l = th_scenery_begin(&s->scenery);
            if (l) {
                draw_scale(l);
                th_scenery_end(&s->scenery, l);
            }
        }
        lv_obj_invalidate(s->canvas);
    }
}

static void tap(page_t *p, int x, int y)
{
    modem_t *s = p->state;
    s->by_snr = !s->by_snr;
}

static alert_level_t status(page_t *p, char *why, int len)
{
    modem_t *s = p->state;
    const char *n = s->m.modem_name[0] ? s->m.modem_name : "modem";
    if (s->m.behind) {
        snprintf(why, len, "%s falling behind real time", n);
        return ALERT_CRIT;
    }
    if (!s->m.modem_up && s->now > 60000) {
        snprintf(why, len, "%s metrics not answering", n);
        return ALERT_CRIT;
    }
    if (s->m.tx_stalls_1h > 10) {
        snprintf(why, len, "%s %u TX stalls in the hour", n, (unsigned)s->m.tx_stalls_1h);
        return ALERT_WARN;
    }
    if (s->m.busy_pct > 85) {
        snprintf(why, len, "%s receiver %.0f%% busy", n, s->m.busy_pct);
        return ALERT_WARN;
    }
    return ALERT_NONE;
}

const page_type_t page_modem = {
    .type = "modem", .label = "Modem", .create = create, .destroy = destroy,
    .tick = tick, .tap = tap, .status = status,
};
