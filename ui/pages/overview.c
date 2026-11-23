// Overview page: the headline of every other page in the list, at a glance.
// Each page gets a cell on a ring, in page order clockwise from the top: a
// miniature of that page's own drawing (the ping plot, the traffic clock, the
// SNR ladder, the battery ring with its flows, rooms against their targets,
// the aircraft map, the spectrum, the lock reticle, the bell), one value and
// the page's title. The rim carries one segment per page in that page's
// colour; it turns yellow or red and pulses with the page's alert, amber for
// a notice, white for a call. The centre spells out one page at a time: the
// alerting ones if there are any, otherwise each in turn.
// Tap a cell to go to its page, or tap the centre to go to the one it is
// describing. The bezel walks an amber selection round the ring (tap to go);
// walking off either end carries on to the neighbouring page as usual.
// Reads only g_model and what the pager knows of each page's status.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "../model.h"
#include "../page.h"
#include "../theme.h"
#include "../ui.h"

#define MAX_CELLS 11
#define R_CELL 118            // cell centres
#define R_RIM 162             // status segments
#define R_CORE 66             // centre disc
#define GLYPH_R 17
#define SPOT_MS 3500          // how long the centre stays on one page
#define SELECT_HOLD_MS 6000
#define SETTLE_MS 1200        // the bezel pages straight through until the page has been up this long
#define WF_BINS 10
#define ST_SPAN_MS 600000u

enum { K_HOSTS, K_STATION, K_MODEM, K_ENERGY, K_ROOMS, K_SKY, K_WATERFALL, K_GPSDO, K_DOOR, K_COUNT };
static const char *const kind_type[K_COUNT] = {"hosts", "station", "modem", "energy", "rooms",
                                               "sky",   "waterfall", "gpsdo", "doorbell"};
// One colour per page, from the colours those pages already draw in.
static const uint32_t kind_rgb[K_COUNT] = {0x46D5A5, 0xEDE8DF, 0xB68CFF, 0xF5D547, 0xFF7A3D,
                                           0x4CC2FF, 0xFF8FA3, 0xD7E36F, 0xE8C27A};

typedef struct {
    int page, kind;
    float ang;
    char label[12];
    // worked out each frame
    bool live;
    alert_level_t lv;
    char val[12];
    lv_color_t val_c;
} cell_t;

typedef struct {
    lv_obj_t *canvas;
    uint32_t now, shown_ms;
    bool was_visible;
    cell_t c[MAX_CELLS];
    int n;
    int spot, sel;
    uint32_t spot_ms, sel_ms;
    float ptr_ang;
    th_scenery_t scenery;
    uint32_t sc_key;
    time_t wall;

    // Headlines, copied out of the model under its lock.
    struct {
        int n, up, down_i, slow_i;
        uint8_t state[MODEL_MAX_HOSTS], slow[MODEL_MAX_HOSTS];
        float rtt[MODEL_MAX_HOSTS];
        char down[20], slowest[20];
        float worst_ms;
    } h;
    struct {
        bool up;
        int n, rx, tx;
        uint32_t t[KISS_FRAMES], seq, seen_seq, arrive_ms;
        uint8_t is_tx[KISS_FRAMES];
        kiss_frame_t last;
    } st;
    struct {
        bool up, behind, any;
        int n, heard;
        float busy, rt, snr[5];
        uint32_t stalls_1h, heard_ms[5];
        char last[12];
        int nbar;
    } m;
    struct {
        bool any;
        float pv, load, grid, batt, soc, rate;
        char plan[24];
    } e;
    struct {
        bool any;
        int n, heating, cooling;
        float worst;
        room_t r[MODEL_MAX_ROOMS];
    } r;
    struct {
        bool any;
        int n, nearest;
        float radius, dx[MODEL_MAX_AC], dy[MODEL_MAX_AC], d[MODEL_MAX_AC], alt[MODEL_MAX_AC];
        char name[12], type[6];
    } a;
    struct {
        bool up, any, tx;
        float bin[WF_BINS], lo, hi;
        uint32_t tags;
        wf_tag_t last;
        double dial;
    } w;
    struct {
        bool up, any, locked, ref_ok, pps;
        char state[16], fix[8];
        float tic, tic_disp;
        int used, view;
        uint32_t locked_s;
    } g;
    struct {
        bool up;
        uint32_t rings, wall;
    } d;
} ov_t;

static lv_color_t kind_colour(int k) { return lv_color_hex(kind_rgb[k]); }

static lv_color_t level_colour(alert_level_t lv)
{
    return lv == ALERT_CRIT ? C_CRIT : lv == ALERT_WARN ? C_WARN : lv == ALERT_CALL ? C_INK : C_ACCENT;
}

// A call or a critical alert: the ones that will not wait.
static bool urgent_level(alert_level_t lv) { return lv == ALERT_CALL || lv == ALERT_CRIT; }

static void upper(char *b)
{
    for (; *b; b++)
        if (*b >= 'a' && *b <= 'z') *b -= 32;
}

static void fmt_age(char *b, int n, uint32_t sec)
{
    if (sec < 60) snprintf(b, n, "%us", (unsigned)sec);
    else if (sec < 3600) snprintf(b, n, "%um", (unsigned)(sec / 60));
    else if (sec < 172800) snprintf(b, n, "%uh", (unsigned)(sec / 3600));
    else snprintf(b, n, "%ud", (unsigned)(sec / 86400));
}

static void fmt_kw(char *b, int n, float w)
{
    if (isnan(w)) snprintf(b, n, "--");
    else if (fabsf(w) < 995) snprintf(b, n, "%.0fW", w);
    else snprintf(b, n, "%.1fkW", w / 1000);
}

static void txt(lv_layer_t *l, const lv_font_t *f, int x, int y, int w, lv_color_t c, lv_opa_t opa, int sp,
                const char *t)
{
    th_text(l, f, x, y, w, LV_TEXT_ALIGN_CENTER, c, opa, sp, t);
}

static float pulse(uint32_t now, uint32_t period)
{
    float k = (now % period) / (float)period;
    return k < 0.5f ? k * 2 : (1 - k) * 2;
}

static lv_color_t gps_colour(const ov_t *s)
{
    if (!s->g.any) return C_DIM;
    if (s->g.locked && s->g.ref_ok && s->g.pps) return C_OK;
    if (!strcmp(s->g.state, "HOLDOVER") || !strcmp(s->g.state, "WARMUP")) return C_WARN;
    return C_CRIT;
}

static lv_color_t room_colour(const room_t *r)
{
    if (!strcmp(r->status, "heating") || r->heat_demand || r->valves == 1 || !strcmp(r->ac_mode, "heat")) return C_HEAT;
    if (!strcmp(r->status, "cooling") || !strcmp(r->ac_mode, "cool")) return C_COOL;
    return C_DIM;
}

// ---- The model, boiled down ----

static void snapshot(ov_t *s, uint32_t now, uint32_t dt)
{
    uint8_t spec[WF_W];
    model_lock();
    // Hosts
    s->h.n = g_model.n_hosts > MODEL_MAX_HOSTS ? MODEL_MAX_HOSTS : g_model.n_hosts;
    s->h.up = 0;
    s->h.down_i = s->h.slow_i = -1;
    s->h.worst_ms = 0;
    for (int i = 0; i < s->h.n; i++) {
        const host_t *h = &g_model.hosts[i];
        s->h.state[i] = h->state;
        s->h.rtt[i] = h->rtt_ms;
        s->h.slow[i] = h->state == HOST_UP && (h->rtt_ms >= 150 || h->loss_pct >= 5);
        if (h->state == HOST_UP) s->h.up++;
        if (h->state == HOST_DOWN && s->h.down_i < 0) {
            s->h.down_i = i;
            snprintf(s->h.down, sizeof s->h.down, "%s", h->name);
        }
        if (h->state == HOST_UP && h->rtt_ms > s->h.worst_ms) {
            s->h.worst_ms = h->rtt_ms;
            s->h.slow_i = i;
            snprintf(s->h.slowest, sizeof s->h.slowest, "%s", h->name);
        }
    }
    // Station
    s->st.up = g_model.kiss_up;
    s->st.n = g_model.frames_n;
    s->st.seq = g_model.frames_seq;
    s->st.rx = s->st.tx = 0;
    for (int i = 0; i < s->st.n; i++) {
        const kiss_frame_t *f = model_kiss_get(i);
        s->st.t[i] = f->t_ms;
        s->st.is_tx[i] = f->tx;
        if (now - f->t_ms <= ST_SPAN_MS) {
            if (f->tx) s->st.tx++;
            else s->st.rx++;
        }
        if (i == 0) s->st.last = *f;
    }
    // Modem
    s->m.up = g_model.modem_up;
    s->m.any = g_model.modem_poll_ms != 0;
    s->m.behind = g_model.behind;
    s->m.busy = g_model.busy_pct;
    s->m.rt = g_model.rt_ratio;
    s->m.stalls_1h = g_model.tx_stalls_1h;
    s->m.n = g_model.n_mstations;
    s->m.heard = 0;
    s->m.nbar = 0;
    s->m.last[0] = 0;
    for (int i = 0; i < g_model.n_mstations && i < MODEL_MAX_MSTATIONS; i++) {
        const mstation_t *t = &g_model.mstations[i];
        if (!t->heard_ms) continue;
        if (now - t->heard_ms <= 600000) s->m.heard++;
        // Keep the five heard most recently, newest first.
        int at = s->m.nbar;
        while (at > 0 && s->m.heard_ms[at - 1] < t->heard_ms) at--;
        if (at >= 5) continue;
        int end = s->m.nbar < 5 ? s->m.nbar : 4;
        for (int k = end; k > at; k--) {
            s->m.heard_ms[k] = s->m.heard_ms[k - 1];
            s->m.snr[k] = s->m.snr[k - 1];
        }
        s->m.heard_ms[at] = t->heard_ms;
        s->m.snr[at] = t->snr_last;
        if (s->m.nbar < 5) s->m.nbar++;
        if (at == 0) snprintf(s->m.last, sizeof s->m.last, "%s", t->call);
    }
    // Energy
    s->e.any = g_model.energy_poll_ms != 0;
    s->e.pv = g_model.pv_w;
    s->e.load = g_model.load_w;
    s->e.grid = g_model.grid_w;
    s->e.batt = g_model.batt_w;
    s->e.soc = g_model.soc_pct;
    s->e.rate = g_model.rate_p;
    snprintf(s->e.plan, sizeof s->e.plan, "%s", g_model.plan_status);
    // Rooms
    s->r.any = g_model.climate_poll_ms != 0 && g_model.n_rooms > 0;
    s->r.n = g_model.n_rooms > MODEL_MAX_ROOMS ? MODEL_MAX_ROOMS : g_model.n_rooms;
    memcpy(s->r.r, g_model.rooms, sizeof(room_t) * s->r.n);
    // Sky
    s->a.any = !isnan(g_model.home_lat) && g_model.adsb_radius_km > 0 && g_model.adsb_poll_ms != 0;
    s->a.radius = g_model.adsb_radius_km;
    s->a.n = 0;
    s->a.nearest = -1;
    if (s->a.any) {
        float kx = 111.32f * cosf(DEG2RAD(g_model.home_lat)), ky = 110.57f;
        for (int i = 0; i < g_model.n_ac && i < MODEL_MAX_AC; i++) {
            const aircraft_t *a = &g_model.ac[i];
            if (isnan(a->lat) || isnan(a->lon)) continue;
            int k = s->a.n++;
            s->a.dx[k] = (a->lon - g_model.home_lon) * kx;
            s->a.dy[k] = (a->lat - g_model.home_lat) * ky;
            s->a.d[k] = sqrtf(s->a.dx[k] * s->a.dx[k] + s->a.dy[k] * s->a.dy[k]);
            s->a.alt[k] = a->alt_ft;
            if (s->a.nearest < 0 || s->a.d[k] < s->a.d[s->a.nearest]) {
                s->a.nearest = k;
                snprintf(s->a.name, sizeof s->a.name, "%s", a->flight[0] ? a->flight : a->reg[0] ? a->reg : a->hex);
                snprintf(s->a.type, sizeof s->a.type, "%s", a->type);
            }
        }
    }
    // Waterfall
    s->w.up = g_model.wf_up;
    s->w.any = g_model.wf_row_seq != 0;
    s->w.dial = g_model.wf_dial_hz;
    s->w.tags = g_model.wf_tag_seq;
    if (s->w.tags) s->w.last = g_model.wf_tags[(s->w.tags - 1) % WF_TAGS];
    s->w.tx = s->w.any && g_model.wf_row_tx[(g_model.wf_row_seq - 1) % WF_ROWS];
    memcpy(spec, g_model.wf_spec, sizeof spec);
    // GPSDO
    s->g.up = g_model.gps_up;
    s->g.any = g_model.gps_seq != 0;
    s->g.locked = g_model.gps_locked;
    s->g.ref_ok = g_model.gps_ref_ok;
    s->g.pps = g_model.gps_pps;
    snprintf(s->g.state, sizeof s->g.state, "%s", g_model.gps_state);
    snprintf(s->g.fix, sizeof s->g.fix, "%s", g_model.gps_fix[0] ? g_model.gps_fix : "--");
    s->g.tic = g_model.gps_tic_ns;
    s->g.used = g_model.gps_sats_used;
    s->g.view = g_model.gps_sats_view;
    s->g.locked_s = g_model.gps_locked_s;
    // Doorbell
    s->d.up = g_model.door_up;
    s->d.rings = g_model.door_ring_seq;
    s->d.wall = g_model.door_ring_wall;
    model_unlock();

    if (s->st.seq != s->st.seen_seq) {
        if (s->st.seen_seq) s->st.arrive_ms = now;
        s->st.seen_seq = s->st.seq;
    }
    // Rooms: how many are being heated or cooled, and the furthest from its target.
    s->r.heating = s->r.cooling = 0;
    s->r.worst = 0;
    for (int i = 0; i < s->r.n; i++) {
        const room_t *r = &s->r.r[i];
        lv_color_t c = room_colour(r);
        if (lv_color_eq(c, C_HEAT)) s->r.heating++;
        if (lv_color_eq(c, C_COOL)) s->r.cooling++;
        float dev = r->temp - r->target;
        if (!isnan(dev) && fabsf(dev) > fabsf(s->r.worst)) s->r.worst = dev;
    }
    // Spectrum: a few bars, levelled between the quietest and the strongest.
    float lo = 255, hi = 0;
    for (int b = 0; b < WF_BINS; b++) {
        int v = 0;
        for (int x = b * WF_W / WF_BINS; x < (b + 1) * WF_W / WF_BINS; x++)
            if (spec[x] > v) v = spec[x];
        s->w.bin[b] = th_ease(s->w.bin[b], v, dt, 140);
        lo = fminf(lo, s->w.bin[b]);
        hi = fmaxf(hi, s->w.bin[b]);
    }
    s->w.lo = th_ease(s->w.lo, lo, dt, 800);
    s->w.hi = hi > s->w.hi ? hi : th_ease(s->w.hi, fmaxf(hi, lo + 50), dt, 2000);  // at least 20 dB of scale
    if (!isnan(s->g.tic)) s->g.tic_disp = isnan(s->g.tic_disp) ? s->g.tic : th_ease(s->g.tic_disp, s->g.tic, dt, 300);
    s->wall = time(NULL);
}

// Each cell's one value, and whether its source has anything to say yet.
static void headline(ov_t *s, cell_t *c)
{
    c->val_c = C_INK;
    c->live = true;
    switch (c->kind) {
    case K_HOSTS:
        c->live = s->h.n > 0;
        snprintf(c->val, sizeof c->val, "%d/%d", s->h.up, s->h.n);
        break;
    case K_STATION:
        c->live = s->st.up || s->st.n;
        if (!s->st.up) {
            snprintf(c->val, sizeof c->val, "NO LINK");
            c->val_c = C_CRIT;
        } else if (s->st.n) {
            snprintf(c->val, sizeof c->val, "%s", s->st.last.src);
            if (s->st.last.tx) c->val_c = C_ACCENT;
        } else {
            snprintf(c->val, sizeof c->val, "LISTENING");
            c->val_c = C_SUB;
        }
        break;
    case K_MODEM:
        c->live = s->m.any;
        if (!s->m.up) {
            snprintf(c->val, sizeof c->val, "NO DATA");
            c->val_c = C_SUB;
        } else {
            snprintf(c->val, sizeof c->val, "%d HEARD", s->m.heard);
        }
        break;
    case K_ENERGY:
        c->live = s->e.any && !isnan(s->e.soc);
        snprintf(c->val, sizeof c->val, "%.0f%% %s", s->e.soc,
                 isnan(s->e.batt) || fabsf(s->e.batt) < 30 ? "" : s->e.batt > 0 ? "\xE2\x86\x91" : "\xE2\x86\x93");
        break;
    case K_ROOMS:
        c->live = s->r.any;
        if (s->r.heating) {
            snprintf(c->val, sizeof c->val, "HEAT %d", s->r.heating);
            c->val_c = C_HEAT;
        } else if (s->r.cooling) {
            snprintf(c->val, sizeof c->val, "COOL %d", s->r.cooling);
            c->val_c = C_COOL;
        } else if (fabsf(s->r.worst) <= 0.5f) {
            snprintf(c->val, sizeof c->val, "STEADY");
        } else {
            snprintf(c->val, sizeof c->val, "%+.1f\xC2\xB0", s->r.worst);
        }
        break;
    case K_SKY:
        c->live = s->a.any;
        if (s->a.nearest < 0) {
            snprintf(c->val, sizeof c->val, "CLEAR");
            c->val_c = C_SUB;
        } else {
            float d = s->a.d[s->a.nearest];
            snprintf(c->val, sizeof c->val, d < 10 ? "%.1f km" : "%.0f km", d);
        }
        break;
    case K_WATERFALL:
        c->live = s->w.any;
        if (!s->w.up) {
            snprintf(c->val, sizeof c->val, "NO FEED");
            c->val_c = C_SUB;
        } else if (s->w.tags) {
            snprintf(c->val, sizeof c->val, "%s", s->w.last.from);
            if (s->w.last.tx) c->val_c = C_ACCENT;
        } else {
            snprintf(c->val, sizeof c->val, "LISTENING");
            c->val_c = C_SUB;
        }
        break;
    case K_GPSDO:
        c->live = s->g.any;
        snprintf(c->val, sizeof c->val, "%.9s", s->g.state[0] ? s->g.state : "--");
        c->val_c = gps_colour(s);
        break;
    case K_DOOR:
        c->live = s->d.up;
        if (c->lv == ALERT_CALL) {
            snprintf(c->val, sizeof c->val, "RINGING");
        } else if (s->d.rings && s->d.wall && s->wall > (time_t)s->d.wall && s->wall > 1600000000) {
            char a[8];
            fmt_age(a, sizeof a, (uint32_t)(s->wall - s->d.wall));
            snprintf(c->val, sizeof c->val, "%s AGO", a);
            c->val_c = C_SUB;
        } else if (s->d.rings) {
            snprintf(c->val, sizeof c->val, "%u RUNG", (unsigned)s->d.rings);
            c->val_c = C_SUB;
        } else {
            snprintf(c->val, sizeof c->val, "QUIET");
            c->val_c = C_SUB;
        }
        break;
    }
    if (!c->live) {
        snprintf(c->val, sizeof c->val, "--");
        c->val_c = C_DIM;
    }
    if (c->lv == ALERT_WARN || c->lv == ALERT_CRIT) c->val_c = level_colour(c->lv);
    else if (c->lv == ALERT_NOTICE) c->val_c = C_ACCENT;
}

// ---- Miniatures ----

static void glyph_hosts(lv_layer_t *l, const ov_t *s, float x, float y)
{
    th_ring(l, x, y, GLYPH_R, 1, C_FAINT, LV_OPA_COVER);
    th_ring(l, x, y, 7, 1, C_HAIR, LV_OPA_COVER);
    float sw = DEG2RAD(fmodf(s->now * 0.072f, 360.0f));
    th_line(l, x, y, x + GLYPH_R * sinf(sw), y - GLYPH_R * cosf(sw), 1, C_ACCENT, 150);
    for (int i = 0; i < s->h.n; i++) {
        float a = DEG2RAD((i + 0.5f) * 360.0f / s->h.n);
        float rtt = s->h.rtt[i] < 0.1f ? 0.1f : s->h.rtt[i];
        float lg = fminf(1, (log10f(rtt) + 1.0f) / 4.0f);
        float r = s->h.state[i] == HOST_DOWN ? GLYPH_R : 3 + (GLYPH_R - 4) * lg;
        lv_color_t c = s->h.state[i] == HOST_DOWN ? C_CRIT : s->h.state[i] == HOST_UNKNOWN ? C_DIM
                                                           : s->h.slow[i] ? C_WARN : C_OK;
        th_dot(l, x + r * sinf(a), y - r * cosf(a), s->h.state[i] == HOST_UP && !s->h.slow[i] ? 1 : 2, c, LV_OPA_COVER);
    }
}

static void glyph_station(lv_layer_t *l, const ov_t *s, float x, float y)
{
    th_ring(l, x, y, 9, 1, C_HAIR, LV_OPA_COVER);
    for (int i = s->st.n - 1; i >= 0; i--) {
        uint32_t age = s->now - s->st.t[i];
        if (age >= ST_SPAN_MS) continue;
        float a = DEG2RAD(-360.0f * age / ST_SPAN_MS);
        float sn = sinf(a), cs = cosf(a);
        lv_opa_t o = (lv_opa_t)(255 - 150 * (float)age / ST_SPAN_MS);
        th_line(l, x + 11 * sn, y - 11 * cs, x + GLYPH_R * sn, y - GLYPH_R * cs, 1, s->st.is_tx[i] ? C_ACCENT : C_INK, o);
    }
    th_line(l, x, y - GLYPH_R - 2, x, y - 9, 1, C_ACCENT, 230);
    uint32_t since = s->now - s->st.arrive_ms;
    if (s->st.arrive_ms && since < 700) {
        float k = since / 700.0f;
        th_ring(l, x, y, (int)(2 + 6 * k), 1, s->st.last.tx ? C_ACCENT : C_INK, (lv_opa_t)(220 * (1 - k)));
    } else {
        th_dot(l, x, y, 1, s->st.up ? C_SUB : C_CRIT, LV_OPA_COVER);
    }
}

static void glyph_modem(lv_layer_t *l, const ov_t *s, float x, float y)
{
    const float x0 = x - 14;
    th_line(l, x0, y - 16, x0, y + 16, 1, C_FAINT, LV_OPA_COVER);
    for (int i = 0; i < 5; i++) {
        float yy = y - 14 + i * 7;
        if (i >= s->m.nbar) {
            th_line(l, x0 + 2, yy, x0 + 5, yy, 1, C_HAIR, LV_OPA_COVER);
            continue;
        }
        float snr = isnan(s->m.snr[i]) ? 0 : s->m.snr[i];
        float len = 3 + 27 * fminf(1, fmaxf(0, snr / 25));
        lv_color_t c = isnan(s->m.snr[i]) ? C_DIM : snr >= 10 ? C_INK : snr >= 4 ? C_WARN : C_CRIT;
        uint32_t since = s->now - s->m.heard_ms[i];
        lv_opa_t o = since < 1500 ? 255 : (lv_opa_t)(200 - 28 * i);
        th_line(l, x0 + 2, yy, x0 + 2 + len, yy, since < 1500 ? 3 : 2, c, o);
    }
}

static void glyph_energy(lv_layer_t *l, const ov_t *s, float x, float y)
{
    th_ring(l, x, y, GLYPH_R, 1, C_FAINT, LV_OPA_COVER);
    float k = isnan(s->e.soc) ? 0 : fminf(1, fmaxf(0, s->e.soc / 100));
    if (k > 0.01f) th_arc(l, (int)x, (int)y, GLYPH_R, 3, 0, 360 * k, s->e.soc < 15 ? C_WARN : C_OK, LV_OPA_COVER);
    // Flows as on the page: solar in from the top, grid on the left, house on the right.
    const float w[3] = {s->e.pv, s->e.grid, -s->e.load};
    const float dxs[3] = {0, -1, 1}, dys[3] = {-1, 0, 0};
    const lv_color_t col[3] = {C_WARN, C_INK, C_SUB};
    for (int p = 0; p < 3; p++) {
        if (isnan(w[p]) || fabsf(w[p]) < 30) {
            th_dot(l, x + dxs[p] * 6, y + dys[p] * 6, 1, C_FAINT, LV_OPA_COVER);
            continue;
        }
        float t = (s->now % 900) / 900.0f;
        if (w[p] < 0) t = 1 - t;                       // flowing outwards
        float r = 11 - 9 * t;
        float fade = t < 0.2f ? t / 0.2f : t > 0.8f ? (1 - t) / 0.2f : 1;
        th_dot(l, x + dxs[p] * r, y + dys[p] * r, 1, col[p], (lv_opa_t)(255 * fade));
        th_dot(l, x + dxs[p] * 11, y + dys[p] * 11, 1, col[p], 90);
    }
}

static void glyph_rooms(lv_layer_t *l, const ov_t *s, float x, float y)
{
    // Each room as a stem off the target line: above it when warm, below when cold.
    th_line(l, x - GLYPH_R, y, x + GLYPH_R, y, 1, C_FAINT, LV_OPA_COVER);
    int n = s->r.n;
    for (int i = 0; i < n; i++) {
        const room_t *r = &s->r.r[i];
        float xx = x - GLYPH_R + (i + 0.5f) * (2.0f * GLYPH_R) / n;
        float dev = r->temp - r->target;
        if (isnan(dev)) continue;
        float off = fminf(15, fmaxf(-15, dev * 8));
        lv_color_t c = room_colour(r);
        if (lv_color_eq(c, C_DIM)) c = fabsf(dev) <= 0.5f ? C_INK : C_SUB;
        if (fabsf(off) >= 2) th_line(l, xx, y, xx, y - off, 1, c, 160);
        th_dot(l, xx, y - off, n > 6 ? 1 : 2, c, LV_OPA_COVER);
    }
}

static void glyph_sky(lv_layer_t *l, const ov_t *s, const cell_t *c, float x, float y)
{
    th_ring(l, x, y, GLYPH_R, 1, C_FAINT, LV_OPA_COVER);
    th_ring(l, x, y, 7, 1, C_HAIR, LV_OPA_COVER);
    th_line(l, x - 2, y, x + 2, y, 1, C_ACCENT, LV_OPA_COVER);
    th_line(l, x, y - 2, x, y + 2, 1, C_ACCENT, LV_OPA_COVER);
    for (int i = 0; i < s->a.n; i++) {
        if (s->a.d[i] > s->a.radius) continue;
        float r = GLYPH_R * sqrtf(s->a.d[i] / s->a.radius);
        float k = s->a.d[i] > 0.01f ? r / s->a.d[i] : 0;
        bool hot = i == s->a.nearest && c->lv == ALERT_NOTICE;
        lv_color_t col = hot ? C_ACCENT : isnan(s->a.alt[i]) ? C_DIM : s->a.alt[i] < 5000 ? C_INK
                                        : s->a.alt[i] < 15000 ? C_SUB : C_DIM;
        th_dot(l, x + s->a.dx[i] * k, y - s->a.dy[i] * k, hot || i == s->a.nearest ? 2 : 1, col, LV_OPA_COVER);
    }
}

static void glyph_waterfall(lv_layer_t *l, const ov_t *s, float x, float y)
{
    static const uint32_t rx[] = {0x7A2448, 0xD9601E, 0xFF9F1C, 0xFFE2A8}, tx[] = {0x0E3A5C, 0x2A7FB0, 0x2A7FB0, 0x9AD8FF};
    const float base = y + 14;
    th_line(l, x - GLYPH_R, base + 1, x + GLYPH_R, base + 1, 1, C_FAINT, LV_OPA_COVER);
    float span = s->w.hi - s->w.lo;
    if (span < 1) span = 1;
    for (int b = 0; b < WF_BINS; b++) {
        float k = fminf(1, fmaxf(0, (s->w.bin[b] - s->w.lo) / span));
        float xx = x - GLYPH_R + 2 + b * 3.4f;
        int ci = k > 0.85f ? 3 : k > 0.55f ? 2 : k > 0.25f ? 1 : 0;
        th_line(l, xx, base, xx, base - 2 - 26 * k, 2, lv_color_hex((s->w.tx ? tx : rx)[ci]), LV_OPA_COVER);
    }
}

static void glyph_gpsdo(lv_layer_t *l, const ov_t *s, float x, float y)
{
    lv_color_t c = gps_colour(s);
    th_ring(l, x, y, 12, s->g.any ? 2 : 1, s->g.any ? c : C_FAINT, LV_OPA_COVER);
    for (int q = 0; q < 4; q++) {
        float a = DEG2RAD(q * 90.0f);
        th_line(l, x + 9 * sinf(a), y - 9 * cosf(a), x + 18 * sinf(a), y - 18 * cosf(a), 1, s->g.any ? c : C_FAINT,
                LV_OPA_COVER);
    }
    if (s->g.any && !isnan(s->g.tic_disp)) {
        float off = fminf(8, fmaxf(-8, s->g.tic_disp * 0.25f));
        th_dot(l, x + off, y, 2, C_ACCENT, LV_OPA_COVER);
    }
}

static void glyph_door(lv_layer_t *l, const ov_t *s, const cell_t *c, float x, float y)
{
    bool ring = c->lv == ALERT_CALL;
    lv_color_t col = !c->live ? C_FAINT : ring ? C_INK : C_SUB;
    float sw = ring ? 3 * sinf(s->now / 55.0f) : 0;
    th_arc(l, (int)x, (int)y - 3, 8, 2, -90, 90, col, LV_OPA_COVER);
    th_line(l, x - 7, y - 3, x - 9, y + 7, 2, col, LV_OPA_COVER);
    th_line(l, x + 7, y - 3, x + 9, y + 7, 2, col, LV_OPA_COVER);
    th_line(l, x - 12, y + 8, x + 12, y + 8, 2, col, LV_OPA_COVER);
    th_line(l, x, y - 14, x, y - 11, 2, col, LV_OPA_COVER);
    th_dot(l, x + sw, y + 12, 2, ring ? C_ACCENT : col, LV_OPA_COVER);
    if (ring) {
        lv_opa_t o = (lv_opa_t)(90 + 160 * pulse(s->now, 500));
        th_arc(l, (int)x, (int)y, 18, 1, 50, 85, C_INK, o);
        th_arc(l, (int)x, (int)y, 18, 1, 275, 310, C_INK, o);
    }
}

// ---- Layout ----

static bool lower(const cell_t *c) { return cosf(DEG2RAD(c->ang)) < -0.05f; }

// Where the cell's parts sit: the miniature towards the rim, the text towards the centre.
static void cell_layout(const cell_t *c, int *gy, int *vy, int *ly)
{
    if (lower(c)) {
        *ly = -33;
        *vy = -20;
        *gy = 15;
    } else {
        *gy = -17;
        *vy = 2;
        *ly = 18;
    }
}

static float seg_half(const ov_t *s) { return 180.0f / s->n - 3.0f; }

// What never moves: the rim segments at rest, the centre ring, the titles.
static void draw_scenery(lv_layer_t *l, ov_t *s)
{
    for (int i = 0; i < s->n; i++) {
        const cell_t *c = &s->c[i];
        if (s->n > 1)
            th_arc(l, SCR_CX, SCR_CY, R_RIM, 2, c->ang - seg_half(s), c->ang + seg_half(s),
                   c->live ? kind_colour(c->kind) : C_FAINT, c->live ? 120 : LV_OPA_COVER);
        else th_ring(l, SCR_CX, SCR_CY, R_RIM, 2, kind_colour(c->kind), 120);
        int gy, vy, ly;
        cell_layout(c, &gy, &vy, &ly);
        txt(l, &font_mono_11, (int)th_px(R_CELL, c->ang), (int)th_py(R_CELL, c->ang) + ly, 84, C_DIM, LV_OPA_COVER, 1,
            c->label);
    }
    // A fine scale inside the rim, broken where the segments are.
    float pitch = 360.0f / s->n;
    for (int k = 0; k < 120; k++) {
        float a = k * 3.0f, off = fmodf(a + pitch / 2, pitch) - pitch / 2;
        if (s->n > 1 && fabsf(off) > seg_half(s) + 0.5f) continue;
        th_line(l, th_px(R_RIM - 4, a), th_py(R_RIM - 4, a), th_px(R_RIM - 7, a), th_py(R_RIM - 7, a), 1, C_FAINT,
                LV_OPA_COVER);
    }
    th_ring(l, SCR_CX, SCR_CY, R_CORE, 1, C_FAINT, LV_OPA_COVER);
}

static uint32_t scenery_key(const ov_t *s)
{
    uint32_t h = th_hash(TH_HASH_SEED, s->n);
    for (int i = 0; i < s->n; i++) {
        h = th_hash(h, s->c[i].kind * 2 + s->c[i].live);
        for (const char *q = s->c[i].label; *q; q++) h = th_hash(h, *q);
    }
    return h;
}

static void update_scenery(ov_t *s)
{
    uint32_t key = scenery_key(s);
    if (s->scenery.buf && key == s->sc_key) return;
    lv_layer_t *l = th_scenery_begin(&s->scenery);
    if (!l) return;
    draw_scenery(l, s);
    th_scenery_end(&s->scenery, l);
    s->sc_key = key;
}

static void draw_cell(lv_layer_t *l, ov_t *s, int i)
{
    const cell_t *c = &s->c[i];
    int x = (int)th_px(R_CELL, c->ang), y = (int)th_py(R_CELL, c->ang);
    int gy, vy, ly;
    cell_layout(c, &gy, &vy, &ly);
    bool alert = c->lv != ALERT_NONE;
    bool focus = i == s->spot;

    // The rim segment: brighter for the page the centre is describing, the alert colour when it has one.
    if (alert) {
        lv_color_t ac = level_colour(c->lv);
        float p = pulse(s->now, c->lv == ALERT_CALL ? 500 : 1400);
        bool loud = urgent_level(c->lv);
        th_arc(l, SCR_CX, SCR_CY, R_RIM + 1, loud ? 5 : 4, c->ang - seg_half(s), c->ang + seg_half(s), ac,
               (lv_opa_t)((loud ? 90 : 150) + (loud ? 165 : 105) * p));
    } else if (focus && c->live) {
        th_arc(l, SCR_CX, SCR_CY, R_RIM, 2, c->ang - seg_half(s), c->ang + seg_half(s), kind_colour(c->kind),
               LV_OPA_COVER);
    }
    if (focus || alert)
        txt(l, &font_mono_11, x, y + ly, 84, alert ? level_colour(c->lv) : c->live ? kind_colour(c->kind) : C_SUB,
            LV_OPA_COVER, 1, c->label);
    txt(l, &font_mono_13, x, y + vy, 84, c->val_c, LV_OPA_COVER, 0, c->val);

    float gx = x, gyy = y + gy;
    if (!c->live && c->kind != K_DOOR && c->kind != K_GPSDO) {
        th_ring(l, gx, gyy, GLYPH_R, 1, C_HAIR, LV_OPA_COVER);
        th_dot(l, gx, gyy, 1, C_FAINT, LV_OPA_COVER);
        return;
    }
    switch (c->kind) {
    case K_HOSTS: glyph_hosts(l, s, gx, gyy); break;
    case K_STATION: glyph_station(l, s, gx, gyy); break;
    case K_MODEM: glyph_modem(l, s, gx, gyy); break;
    case K_ENERGY: glyph_energy(l, s, gx, gyy); break;
    case K_ROOMS: glyph_rooms(l, s, gx, gyy); break;
    case K_SKY: glyph_sky(l, s, c, gx, gyy); break;
    case K_WATERFALL: glyph_waterfall(l, s, gx, gyy); break;
    case K_GPSDO: glyph_gpsdo(l, s, gx, gyy); break;
    case K_DOOR: glyph_door(l, s, c, gx, gyy); break;
    }
}

static void draw_selection(lv_layer_t *l, ov_t *s)
{
    // The selected page's rim segment in amber, with a claw at each end.
    const cell_t *c = &s->c[s->sel];
    float a0 = c->ang - seg_half(s), a1 = c->ang + seg_half(s);
    th_arc(l, SCR_CX, SCR_CY, R_RIM + 2, 5, a0, a1, C_ACCENT, LV_OPA_COVER);
    th_line(l, th_px(R_RIM, a0), th_py(R_RIM, a0), th_px(R_RIM - 14, a0), th_py(R_RIM - 14, a0), 2, C_ACCENT, LV_OPA_COVER);
    th_line(l, th_px(R_RIM, a1), th_py(R_RIM, a1), th_px(R_RIM - 14, a1), th_py(R_RIM - 14, a1), 2, C_ACCENT, LV_OPA_COVER);
    int gy, vy, ly;
    cell_layout(c, &gy, &vy, &ly);
    txt(l, &font_mono_11, (int)th_px(R_CELL, c->ang), (int)th_py(R_CELL, c->ang) + ly, 84, C_ACCENT, LV_OPA_COVER, 1,
        c->label);
}

// ---- The centre: one page spelt out ----

typedef struct {
    char big[16], l1[24], l2[24], l3[24];
    lv_color_t big_c, l1_c;
} detail_t;

static void detail(ov_t *s, const cell_t *c, detail_t *d)
{
    char a[16], b[16];
    d->big_c = c->val_c;
    d->l1_c = C_SUB;
    d->big[0] = d->l1[0] = d->l2[0] = d->l3[0] = 0;
    if (!c->live) {
        snprintf(d->big, sizeof d->big, "--");
        snprintf(d->l1, sizeof d->l1, "no data yet");
        return;
    }
    switch (c->kind) {
    case K_HOSTS:
        snprintf(d->big, sizeof d->big, "%d/%d", s->h.up, s->h.n);
        if (s->h.down_i >= 0) {
            snprintf(d->l1, sizeof d->l1, "%.14s", s->h.down);
            upper(d->l1);
            snprintf(d->l2, sizeof d->l2, "not answering");
            d->l1_c = C_CRIT;
        } else {
            snprintf(d->l1, sizeof d->l1, "HOSTS UP");
            if (s->h.slow_i >= 0) {
                snprintf(d->l2, sizeof d->l2, "slowest %.0f ms", s->h.worst_ms);
                snprintf(d->l3, sizeof d->l3, "%.13s", s->h.slowest);
            }
        }
        break;
    case K_STATION:
        if (!s->st.up) {
            snprintf(d->big, sizeof d->big, "NO LINK");
            snprintf(d->l1, sizeof d->l1, "KISS port down");
        } else if (!s->st.n) {
            snprintf(d->big, sizeof d->big, "--");
            snprintf(d->l1, sizeof d->l1, "LISTENING");
        } else {
            snprintf(d->big, sizeof d->big, "%s", s->st.last.src);
            snprintf(d->l1, sizeof d->l1, "\xE2\x86\x92 %s", s->st.last.dst);
            fmt_age(a, sizeof a, (s->now - s->st.last.t_ms) / 1000);
            snprintf(d->l2, sizeof d->l2, "%s ago  %uB", a, s->st.last.len);
            snprintf(d->l3, sizeof d->l3, "%d /10min", s->st.rx + s->st.tx);
        }
        break;
    case K_MODEM:
        if (!s->m.up) {
            snprintf(d->big, sizeof d->big, "NO DATA");
            snprintf(d->l1, sizeof d->l1, "metrics down");
        } else {
            snprintf(d->big, sizeof d->big, "%d", s->m.heard);
            snprintf(d->l1, sizeof d->l1, "HEARD /10min");
            snprintf(d->l2, sizeof d->l2, "busy %.0f%% rt %.2f", s->m.busy, s->m.rt);
            if (s->m.behind) snprintf(d->l3, sizeof d->l3, "BEHIND");
            else if (s->m.last[0]) snprintf(d->l3, sizeof d->l3, "last %.9s", s->m.last);
        }
        break;
    case K_ENERGY:
        snprintf(d->big, sizeof d->big, "%.0f%%", s->e.soc);
        d->big_c = s->e.soc < 15 ? C_WARN : C_INK;
        fmt_kw(a, sizeof a, s->e.pv);
        snprintf(d->l1, sizeof d->l1, "SOLAR %s", a);
        d->l1_c = !isnan(s->e.pv) && s->e.pv >= 30 ? C_WARN : C_SUB;
        fmt_kw(a, sizeof a, isnan(s->e.grid) ? NAN : fabsf(s->e.grid));
        snprintf(d->l2, sizeof d->l2, "%s %s", !isnan(s->e.grid) && s->e.grid < -30 ? "export" : "import", a);
        fmt_kw(b, sizeof b, s->e.load);
        snprintf(d->l3, sizeof d->l3, "house %s", b);
        break;
    case K_ROOMS:
        break;  // drawn as a list
    case K_SKY:
        if (s->a.nearest < 0) {
            snprintf(d->big, sizeof d->big, "CLEAR");
            snprintf(d->l1, sizeof d->l1, "no aircraft");
        } else {
            int k = s->a.nearest;
            snprintf(d->big, sizeof d->big, "%s", s->a.name);
            snprintf(d->l1, sizeof d->l1, s->a.d[k] < 10 ? "%.1f km %s" : "%.0f km %s", s->a.d[k], s->a.type);
            if (!isnan(s->a.alt[k])) snprintf(d->l2, sizeof d->l2, "%.0f ft", s->a.alt[k]);
            snprintf(d->l3, sizeof d->l3, "%d in range", s->a.n);
        }
        break;
    case K_WATERFALL:
        if (!s->w.up) {
            snprintf(d->big, sizeof d->big, "NO FEED");
        } else if (!s->w.tags) {
            snprintf(d->big, sizeof d->big, "--");
            snprintf(d->l1, sizeof d->l1, "LISTENING");
        } else {
            snprintf(d->big, sizeof d->big, "%s", s->w.last.from);
            if (s->w.last.tx) snprintf(d->l1, sizeof d->l1, "TX \xE2\x86\x92 %s", s->w.last.to);
            else if (isnan(s->w.last.snr_db)) snprintf(d->l1, sizeof d->l1, "\xE2\x86\x92 %s", s->w.last.to);
            else snprintf(d->l1, sizeof d->l1, "%.0f dB \xE2\x86\x92 %s", s->w.last.snr_db, s->w.last.to);
            snprintf(d->l3, sizeof d->l3, "%u decoded", (unsigned)s->w.tags);
        }
        if (s->w.dial > 0) snprintf(d->l2, sizeof d->l2, "%.1f kHz", s->w.dial / 1000.0);
        break;
    case K_GPSDO:
        snprintf(d->big, sizeof d->big, "%s", s->g.state[0] ? s->g.state : "--");
        if (!isnan(s->g.tic)) snprintf(d->l1, sizeof d->l1, "%+.0f ns", s->g.tic);
        snprintf(d->l2, sizeof d->l2, "sats %d/%d %s", s->g.used, s->g.view, s->g.fix);
        if (s->g.locked) {
            uint32_t t = s->g.locked_s;
            if (t >= 86400) snprintf(d->l3, sizeof d->l3, "for %ud %02uh", (unsigned)(t / 86400), (unsigned)(t / 3600 % 24));
            else if (t >= 3600) snprintf(d->l3, sizeof d->l3, "for %uh %02um", (unsigned)(t / 3600), (unsigned)(t / 60 % 60));
            else snprintf(d->l3, sizeof d->l3, "for %um %02us", (unsigned)(t / 60), (unsigned)(t % 60));
        }
        break;
    case K_DOOR:
        snprintf(d->big, sizeof d->big, "%s", c->lv == ALERT_CALL ? "RINGING" : "QUIET");
        d->big_c = c->lv == ALERT_CALL ? C_INK : C_SUB;
        if (s->d.rings && s->d.wall && s->wall > (time_t)s->d.wall && s->wall > 1600000000) {
            fmt_age(a, sizeof a, (uint32_t)(s->wall - s->d.wall));
            snprintf(d->l1, sizeof d->l1, "rang %s ago", a);
        } else if (!s->d.rings) {
            snprintf(d->l1, sizeof d->l1, "no rings yet");
        }
        if (s->d.rings) snprintf(d->l2, sizeof d->l2, "%u since start", (unsigned)s->d.rings);
        break;
    }
}

// A reason, split over two lines at a word.
static void split(const char *why, char *a, char *b, int n)
{
    int len = (int)strlen(why), cut = len;
    if (len > n - 1) {
        cut = n - 1;
        while (cut > 0 && why[cut] != ' ') cut--;
        if (!cut) cut = n - 1;
    }
    snprintf(a, n, "%.*s", cut, why);
    while (why[cut] == ' ') cut++;
    snprintf(b, n, "%s", why + cut);
}

static void draw_rooms_list(lv_layer_t *l, ov_t *s, lv_opa_t o)
{
    int n = s->r.n > 6 ? 6 : s->r.n;
    int y = SCR_CY - 6 - n * 7;
    for (int i = 0; i < n; i++) {
        const room_t *r = &s->r.r[i];
        char b[24];
        lv_color_t c = room_colour(r);
        snprintf(b, sizeof b, "%-6.6s %4.1f", r->name, r->temp);
        th_text(l, &font_mono_11, SCR_CX - 54, y + i * 14, 84, LV_TEXT_ALIGN_LEFT, C_INK, o, 0, b);
        snprintf(b, sizeof b, "%.1f", r->target);
        th_text(l, &font_mono_11, SCR_CX + 54, y + i * 14, 34, LV_TEXT_ALIGN_RIGHT, lv_color_eq(c, C_DIM) ? C_DIM : c, o,
                0, b);
    }
}

static void draw_centre(lv_layer_t *l, ov_t *s)
{
    if (s->spot < 0) return;
    const cell_t *c = &s->c[s->spot];
    lv_color_t kc = kind_colour(c->kind);
    bool alert = c->lv != ALERT_NONE;
    lv_color_t hc = alert ? level_colour(c->lv) : kc;
    uint32_t since = s->now - s->spot_ms;
    lv_opa_t o = since < 220 ? (lv_opa_t)(255 * since / 220) : 255;

    // A pointer on the centre ring towards the cell being described.
    float a = s->ptr_ang;
    th_tri(l, th_px(R_CORE + 9, a), th_py(R_CORE + 9, a), th_px(R_CORE + 1, a - 5), th_py(R_CORE + 1, a - 5),
           th_px(R_CORE + 1, a + 5), th_py(R_CORE + 1, a + 5), s->sel >= 0 ? C_ACCENT : hc, LV_OPA_COVER);

    // The centre ring fills as the time on this page runs out.
    if (s->sel < 0 && s->n > 1 && since < SPOT_MS)
        th_arc(l, SCR_CX, SCR_CY, R_CORE, 1, c->ang, c->ang + 360.0f * since / SPOT_MS, hc, 150);

    char t[12];
    snprintf(t, sizeof t, "%.10s", c->label);
    txt(l, &font_mono_11, SCR_CX, SCR_CY - 52, 96, hc, o, 1, t);
    if (c->kind == K_ROOMS && c->live && !alert) {
        draw_rooms_list(l, s, o);
    } else {
        detail_t d;
        detail(s, c, &d);
        size_t n = strlen(d.big);
        const lv_font_t *f = n <= 6 ? &font_sans_28 : n <= 9 ? &font_sans_20 : &font_mono_13;
        int by = n <= 6 ? SCR_CY - 40 : n <= 9 ? SCR_CY - 33 : SCR_CY - 26;
        txt(l, f, SCR_CX, by, 128, alert && c->lv != ALERT_CALL ? hc : d.big_c, o, 1, d.big);
        const char *why = ui_page_why(c->page);
        if (alert && why[0]) {
            char w1[17], w2[17];
            split(why, w1, w2, sizeof w1);
            txt(l, &font_mono_11, SCR_CX, SCR_CY + 2, 130, hc, o, 0, w1);
            txt(l, &font_mono_11, SCR_CX, SCR_CY + 16, 124, hc, o, 0, w2);
        } else {
            txt(l, &font_mono_13, SCR_CX, SCR_CY, 130, d.l1_c, o, 0, d.l1);
            txt(l, &font_mono_11, SCR_CX, SCR_CY + 18, 124, C_DIM, o, 0, d.l2);
            if (s->sel < 0) txt(l, &font_mono_11, SCR_CX, SCR_CY + 32, 110, C_DIM, o, 0, d.l3);
        }
    }
    if (s->sel >= 0) txt(l, &font_mono_11, SCR_CX, SCR_CY + 38, 100, C_ACCENT, LV_OPA_COVER, 1, "TAP TO OPEN");
}

static void draw_body(lv_event_t *e)
{
    ov_t *s = lv_event_get_user_data(e);
    lv_layer_t *l = lv_event_get_layer(e);
    if (!s->n) {
        txt(l, &font_mono_16, SCR_CX, SCR_CY - 30, 260, C_SUB, LV_OPA_COVER, 2, "OVERVIEW");
        txt(l, &font_mono_11, SCR_CX, SCR_CY - 4, 240, C_DIM, LV_OPA_COVER, 1, "no other pages in the list;");
        txt(l, &font_mono_11, SCR_CX, SCR_CY + 12, 240, C_DIM, LV_OPA_COVER, 1, "add some on the settings page");
        return;
    }
    if (!th_scenery_blit(l, &s->scenery, s->canvas)) draw_scenery(l, s);
    for (int i = 0; i < s->n; i++) draw_cell(l, s, i);
    if (s->sel >= 0) draw_selection(l, s);
    draw_centre(l, s);
}

TH_PAGE_DRAW(draw_cb, draw_body, (lv_obj_t *)lv_event_get_current_target(e))

static void create(page_t *p, lv_obj_t *root)
{
    ov_t *s = calloc(1, sizeof *s);
    p->state = s;
    s->sel = -1;
    s->spot = -1;
    s->g.tic_disp = NAN;
    s->canvas = lv_obj_create(root);
    lv_obj_remove_style_all(s->canvas);
    lv_obj_set_size(s->canvas, SCR_W, SCR_H);
    lv_obj_remove_flag(s->canvas, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s->canvas, draw_cb, LV_EVENT_DRAW_MAIN, s);
}

static void destroy(page_t *p)
{
    ov_t *s = p->state;
    if (s->scenery.buf) lv_draw_buf_destroy(s->scenery.buf);
    free(s);
}

// The cells: every page in the list that this page knows how to summarise.
static void find_cells(ov_t *s)
{
    int n = 0;
    for (int i = 0; i < ui_page_count() && n < MAX_CELLS; i++) {
        const page_t *pg = ui_page(i);
        int kind = -1;
        for (int k = 0; k < K_COUNT; k++)
            if (!strcmp(pg->type->type, kind_type[k])) kind = k;
        if (kind < 0) continue;
        s->c[n].page = i;
        s->c[n].kind = kind;
        snprintf(s->c[n].label, sizeof s->c[n].label, "%.10s", pg->title);
        upper(s->c[n].label);
        n++;
    }
    for (int i = 0; i < n; i++) s->c[i].ang = i * 360.0f / n;
    if (n != s->n) {
        s->sel = -1;
        s->spot = -1;
    }
    s->n = n;
}

static void tick(page_t *p, uint32_t now, uint32_t dt, bool visible)
{
    ov_t *s = p->state;
    s->now = now;
    if (!visible) {
        s->was_visible = false;
        s->sel = -1;
        return;
    }
    if (!s->was_visible) s->shown_ms = now;
    s->was_visible = true;
    find_cells(s);
    snapshot(s, now, dt);
    int urgent = 0, risen = -1;
    for (int i = 0; i < s->n; i++) {
        alert_level_t lv = ui_page_level(s->c[i].page);
        if (lv > s->c[i].lv) risen = i;
        s->c[i].lv = lv;
        if (urgent_level(lv)) urgent++;
        headline(s, &s->c[i]);
    }
    if (s->sel >= 0 && now - s->sel_ms > SELECT_HOLD_MS) s->sel = -1;

    // What the centre describes: the selection; else a page whose alert has
    // just come up; else each page in turn, only the calls and critical ones
    // while there are any.
    if (s->n) {
        int want = s->spot;
        if (s->sel >= 0) {
            want = s->sel;
        } else if (risen >= 0) {
            want = risen;
        } else if (s->spot < 0 || now - s->spot_ms >= SPOT_MS || (urgent && !urgent_level(s->c[s->spot].lv))) {
            for (int k = 1; k <= s->n; k++) {
                int i = ((s->spot < 0 ? -1 : s->spot) + k) % s->n;
                if (urgent ? urgent_level(s->c[i].lv) : s->c[i].live) {
                    want = i;
                    break;
                }
            }
            if (want < 0) want = 0;
            if (want == s->spot) s->spot_ms = now - 220;  // the only candidate: stay, without a fade
        }
        if (want != s->spot) {
            if (s->spot < 0) s->ptr_ang = s->c[want].ang;
            s->spot = want;
            s->spot_ms = now;
        }
        // The pointer swings the short way round.
        float diff = fmodf(s->c[s->spot].ang - s->ptr_ang + 540.0f, 360.0f) - 180.0f;
        s->ptr_ang = fmodf(s->ptr_ang + diff * (1.0f - expf(-(float)dt / 120.0f)) + 360.0f, 360.0f);
    }
    update_scenery(s);
    lv_obj_invalidate(s->canvas);
}

static void tap(page_t *p, int x, int y)
{
    ov_t *s = p->state;
    if (!s->n) return;
    float dx = x - SCR_W / 2, dy = y - SCR_CY;
    int go = -1;
    if (dx * dx + dy * dy < R_CORE * R_CORE) {
        go = s->sel >= 0 ? s->sel : s->spot;
    } else {
        float best = 50 * 50;
        for (int i = 0; i < s->n; i++) {
            float cx = SCR_W / 2 + R_CELL * sinf(DEG2RAD(s->c[i].ang)), cy = SCR_CY - R_CELL * cosf(DEG2RAD(s->c[i].ang));
            float d = (x - cx) * (x - cx) + (y - cy) * (y - cy);
            if (d < best) {
                best = d;
                go = i;
            }
        }
    }
    if (go < 0) return;
    s->sel = -1;
    ui_goto(s->c[go].page, true);
}

static bool knob(page_t *p, int delta)
{
    ov_t *s = p->state;
    if (!s->n) return false;
    int step = delta > 0 ? 1 : -1;
    if (s->sel < 0) {
        if (s->now - s->shown_ms < SETTLE_MS) return false;  // still paging through
        s->sel = step > 0 ? 0 : s->n - 1;
    } else {
        s->sel += step;
        if (s->sel < 0 || s->sel >= s->n) {
            s->sel = -1;
            return false;
        }
    }
    s->sel_ms = s->now;
    return true;
}

const page_type_t page_overview = {
    .type = "overview", .label = "Overview", .create = create, .destroy = destroy,
    .tick = tick, .tap = tap, .knob = knob,
};
