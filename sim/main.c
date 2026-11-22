// Desktop simulator: runs the real UI code against fake data on a simulated
// clock and writes frames as raw RGB565 for tools/frames.py to turn into PNG/GIF.
//   desk_sim <scenario> <outdir> [fps]
// Scenarios: hosts, station, alert, tour
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "lvgl.h"
#include "model.h"
#include "theme.h"
#include "ui.h"

static uint32_t s_now;
static uint16_t s_fb[SCR_W * SCR_H];
static bool s_pressed;
static int s_px, s_py;

void model_lock(void) {}
void model_unlock(void) {}

void platform_alert(alert_level_t level, const char *t, const char *why)
{
    fprintf(stderr, "[%6.2fs] ALERT %s %s: %s\n", s_now / 1000.0, level == ALERT_CRIT ? "CRIT" : "WARN", t, why);
}

static uint32_t tick_cb(void) { return s_now; }
static void flush_cb(lv_display_t *d, const lv_area_t *a, uint8_t *px) { lv_display_flush_ready(d); }
static void touch_cb(lv_indev_t *i, lv_indev_data_t *data)
{
    data->point.x = s_px;
    data->point.y = s_py;
    data->state = s_pressed ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
}

static uint32_t rng = 12345;
static float frand(void) { rng = rng * 1664525u + 1013904223u; return (rng >> 8) / 16777216.0f; }

typedef struct { const char *name, *addr; float base, jitter; } fake_host_t;
static const fake_host_t hosts[] = {
    {"router", "192.168.1.1", 0.6f, 0.3f},       {"server", "192.168.1.10", 0.35f, 0.2f},
    {"nas", "192.168.1.20", 0.4f, 0.2f},         {"mqtt", "192.168.1.70", 0.5f, 0.3f},
    {"node", "192.168.1.37", 2.8f, 1.5f},        {"shack-pi", "192.168.1.41", 3.5f, 2.0f},
    {"printer", "192.168.1.42", 3.1f, 2.0f},     {"cloudflare", "1.1.1.1", 11.5f, 3.0f},
    {"bbc.co.uk", "151.101.0.81", 13.0f, 4.0f}, {"remote-rx", "192.168.2.25", 182.0f, 40.0f},
};
#define NH (int)(sizeof hosts / sizeof hosts[0])

static const char *calls[] = {"G4ABC", "M0XYZ", "G7DEF", "M7GHI-1", "2E0JKL", "MB7XYZ", "GB7ABC", "G0MNO-7", "M1PQR"};

typedef struct { const char *call, *mode; int subch; float snr; } fake_st_t;
static const fake_st_t fst[] = {
    {"GB7ABC", "afsk300-il2pc", 0, 14}, {"EI0XYZ", "bpsk300-il2pc", 2, 6},  {"GB7DEF", "bpsk300-il2pc", 2, 17},
    {"GB7GHI", "bpsk300-il2pc", 2, 9},  {"M0XYZ", "afsk1200", 1, 22},       {"G4ABC", "afsk1200", 1, 25},
    {"MB7XYZ", "bpsk300-il2pc", 2, 3},  {"GB7JKL", "afsk300-il2pc", 0, 11}, {"2E0JKL", "afsk1200", 1, 19},
    {"SA0ABC", "ms110d-wn2", 3, 8},
};
#define NST (int)(sizeof fst / sizeof fst[0])

static void fake_metrics(void)
{
    if (s_now % 10000 >= 11) return;  // a poll every 10 s, like the device
    g_model.modem_up = true;
    g_model.modem_poll_ms = s_now;
    g_model.n_mstations = NST;
    for (int i = 0; i < NST; i++) {
        mstation_t *m = &g_model.mstations[i];
        if (!m->call[0]) {
            snprintf(m->call, sizeof m->call, "%s", fst[i].call);
            snprintf(m->mode, sizeof m->mode, "%s", fst[i].mode);
            m->subch = fst[i].subch;
            m->snr_mean = fst[i].snr;
            m->snr_last = fst[i].snr;
            m->offset_last = 0;
            m->heard_ms = s_now - (uint32_t)(frand() * 1800000);
            m->frames = 100 + (uint32_t)(frand() * 900);
        }
        if (frand() < 0.25f) {
            m->frames += 1 + (uint32_t)(frand() * 4);
            m->snr_last = fst[i].snr + (frand() - 0.5f) * 6;
            m->heard_ms = s_now;
            m->corrected_10m = frand() < 0.3f ? (uint32_t)(frand() * 40) : 0;
        }
    }
    g_model.rt_ratio = 1.0f + (frand() - 0.5f) * 0.004f;
    g_model.busy_pct = 34 + frand() * 10;
    g_model.tx_stalls_1h = 3;
    g_model.input_losses_1h = 0;
}

static void fake_energy(void)
{
    if (s_now % 10000 >= 11) return;
    g_model.energy_up = true;
    g_model.energy_poll_ms = s_now;
    g_model.pv_w = 620 + frand() * 40;
    g_model.load_w = 2200 + frand() * 300;
    g_model.batt_w = -450 - frand() * 50;
    g_model.grid_w = g_model.load_w - g_model.pv_w + g_model.batt_w;
    g_model.soc_pct = 34;
    g_model.soc_kwh = 5.9f;
    g_model.soc_max_kwh = 17.4f;
    g_model.export_rate_p = 19.55f;
    snprintf(g_model.plan_status, sizeof g_model.plan_status, "Demand");
    g_model.pv_today_kwh = 1.6f;
    g_model.import_today_kwh = 23.4f;
    g_model.cost_today_p = 426.9f;
    // 2026-11-20 12:00 UTC, a day on a tariff with three cheap windows and an evening peak.
    g_model.plan_start_wall = 1763640000;
    for (int i = 0; i < ENERGY_SLOTS; i++) {
        float h = 12 + i / 4.0f;
        float hh = fmodf(h, 24);
        float rate = (hh >= 4 && hh < 7) || (hh >= 13 && hh < 16) || hh >= 22 ? 14.05f : (hh >= 16 && hh < 19) ? 42.98f : 28.66f;
        g_model.plan_rate_p[i] = rate;
        // Charge in cheap slots, drain in the evening peak.
        float prev = i ? g_model.plan_soc_kwh[i - 1] : 5.9f;
        float v = rate < 15 ? prev + 1.0f : rate > 40 ? prev - 1.1f : prev - 0.35f;
        g_model.plan_soc_kwh[i] = fminf(17.4f, fmaxf(1.74f, v));
    }
    g_model.rate_p = 28.66f;
    g_model.rate_next_p = 14.05f;
    g_model.rate_next_wall = 1763640000 + 3600;
}

typedef struct { const char *id, *name, *status, *ac; float target, temp, hum, acsp; int valves; const char *why; } fake_room_t;
static const fake_room_t frooms[] = {
    {"living", "Living room", "holding", "off", 23.0f, 22.9f, 52, NAN, 0, "PI idle: room 22.9 <= target 23.0+0.3 -> off"},
    {"office", "Office", "cooling", "cool", 22.0f, 22.4f, 49, 19.0f, 0, "PI cooling: room 22.4 vs target 22.0 (err +0.4) -> setpoint 19.0"},
    {"bedroom", "Bedroom", "holding", "off", 24.0f, 20.2f, 63, NAN, -1, "PI: room 20.2 is >=0.3 below target 24.0 -> off"},
    {"kids", "Kids room", "heating", "off", 22.5f, 21.7f, 52, NAN, 1, "heat: room 21.7 vs target 22.5 -> demand 0.40"},
    {"guest", "Guest room", "holding", "off", 21.5f, 21.6f, NAN, NAN, -1, "PI idle: room 21.6 <= target 21.5+0.3 -> off"},
};
#define NROOMS (int)(sizeof frooms / sizeof frooms[0])
static uint32_t room_write_at;

static void fake_rooms(void)
{
    // Pretend to be the device: take a target request, "write" it, and let the
    // brain pick it up two seconds later.
    if (g_model.room_set.seq != g_model.room_set.done_seq) {
        g_model.room_set.done_seq = g_model.room_set.seq;
        g_model.room_set.result = 1;
        room_write_at = s_now + 2000;
    }
    if (room_write_at && s_now >= room_write_at) {
        for (int i = 0; i < g_model.n_rooms; i++)
            if (!strcmp(g_model.rooms[i].id, g_model.room_set.room)) g_model.rooms[i].target = g_model.room_set.value;
        room_write_at = 0;
    }
    if (g_model.n_rooms) return;
    g_model.climate_up = true;
    g_model.climate_poll_ms = s_now;
    g_model.n_rooms = NROOMS;
    for (int i = 0; i < NROOMS; i++) {
        room_t *r = &g_model.rooms[i];
        const fake_room_t *f = &frooms[i];
        snprintf(r->id, sizeof r->id, "%s", f->id);
        snprintf(r->name, sizeof r->name, "%s", f->name);
        snprintf(r->status, sizeof r->status, "%s", f->status);
        snprintf(r->ac_mode, sizeof r->ac_mode, "%s", f->ac);
        snprintf(r->why, sizeof r->why, "%s", f->why);
        r->target = f->target;
        r->temp = f->temp;
        r->humidity = f->hum;
        r->ac_setpoint = f->acsp;
        r->valves = f->valves;
        r->heat_demand = f->valves == 1;
        r->t_min = 16;
        r->t_max = 26;
        r->t_step = 0.5f;
    }
}

// Made-up traffic round a placeholder point in central London.
typedef struct { const char *hex, *flight, *reg, *type, *desc, *op, *cat, *orig, *ocity, *dest, *dcity;
                 float dx, dy, alt, gs, track, rate; } fake_ac_t;
static fake_ac_t fac[] = {
    {"400a1b", "BAW2CK", "G-XWBC", "A35K", "AIRBUS A-350-1000", "British Airways", "A5", "EGLL", "London", "KJFK", "New York",
     4.2f, 0.6f, 5200, 260, 272, 1800},
    {"4cad80", "EIN165", "EI-NSG", "A20N", "AIRBUS A-320neo", "Aer Lingus", "A3", "EGLL", "London", "EIDW", "Dublin",
     11.0f, 4.0f, 19800, 345, 284, 1600},
    {"406f2a", "", "G-BOYC", "C172", "CESSNA 172", "", "A1", "", "", "", "", -2.0f, -6.0f, 2100, 95, 40, 0},
    {"43c7e1", "", "G-NHVH", "EC35", "EUROCOPTER EC-135", "Air Ambulance", "A7", "", "", "", "",
     -8.0f, -2.0f, 1400, 120, 20, 0},
    {"4ca2b1", "RYR8KQ", "EI-IGD", "B38M", "BOEING 737 MAX 8", "Ryanair", "A3", "EGSS", "Stansted", "LEMG", "Malaga",
     -6.0f, 12.0f, 36000, 470, 225, 0},
    {"406b3c", "EZY73TW", "G-UZHA", "A20N", "AIRBUS A-320neo", "easyJet", "A3", "EGKK", "Gatwick", "EGPH", "Edinburgh",
     9.0f, -14.0f, 24000, 410, 330, 2200},
};
#define NAC (int)(sizeof fac / sizeof fac[0])

static void fake_adsb(void)
{
    const float lat0 = 51.5074f, lon0 = -0.1278f;
    g_model.home_lat = lat0;
    g_model.home_lon = lon0;
    g_model.adsb_radius_km = 25;
    static uint32_t last;
    float dt = last ? (s_now - last) / 1000.0f : 0;
    last = s_now;
    for (int i = 0; i < NAC; i++) {
        fake_ac_t *f = &fac[i];
        float v = f->gs * 0.000514444f;
        if (!strcmp(f->cat, "A1")) f->track = fmodf(f->track + 3 * dt, 360);  // circling
        f->dx += v * sinf(DEG2RAD(f->track)) * dt;
        f->dy += v * cosf(DEG2RAD(f->track)) * dt;
        f->alt += f->rate * dt / 60;
    }
    if (s_now % 3000 >= 11) return;  // a poll every 3 s
    g_model.adsb_up = true;
    g_model.adsb_poll_ms = s_now;
    g_model.n_ac = NAC;
    float kx = 111.32f * cosf(DEG2RAD(lat0));
    for (int i = 0; i < NAC; i++) {
        aircraft_t *a = &g_model.ac[i];
        const fake_ac_t *f = &fac[i];
        if (!a->hex[0]) {
            snprintf(a->hex, sizeof a->hex, "%s", f->hex);
            snprintf(a->flight, sizeof a->flight, "%s", f->flight);
            snprintf(a->reg, sizeof a->reg, "%s", f->reg);
            snprintf(a->type, sizeof a->type, "%s", f->type);
            snprintf(a->desc, sizeof a->desc, "%s", f->desc);
            snprintf(a->op, sizeof a->op, "%s", f->op);
            snprintf(a->cat, sizeof a->cat, "%s", f->cat);
            snprintf(a->orig, sizeof a->orig, "%s", f->orig);
            snprintf(a->orig_city, sizeof a->orig_city, "%s", f->ocity);
            snprintf(a->dest, sizeof a->dest, "%s", f->dest);
            snprintf(a->dest_city, sizeof a->dest_city, "%s", f->dcity);
        }
        if (a->trail_n == AC_TRAIL) {
            memmove(a->trail_lat, a->trail_lat + 1, sizeof(float) * (AC_TRAIL - 1));
            memmove(a->trail_lon, a->trail_lon + 1, sizeof(float) * (AC_TRAIL - 1));
            a->trail_n--;
        }
        if (a->seen_ms) {
            a->trail_lat[a->trail_n] = a->lat;
            a->trail_lon[a->trail_n] = a->lon;
            a->trail_n++;
        }
        a->lat = lat0 + f->dy / 110.57f;
        a->lon = lon0 + f->dx / kx;
        a->alt_ft = f->alt;
        a->gs_kt = f->gs;
        a->track = f->track;
        a->rate_fpm = f->rate;
        a->seen_ms = s_now;
    }
}

// A synthetic passband: four modems laid out like a busy HF packet station.
typedef struct { float lo, hi; int sub; const char *name; int kind; } fake_band_t;  // kind: 0 afsk, 1 ofdm-ish, 2 psk, 3 wide
static const fake_band_t fbands[] = {
    {386, 715, 0, "AFSK300", 0}, {950, 1450, 1, "ARDOP", 1}, {1676, 2027, 2, "BPSK300", 2}, {2660, 5461, 4, "MS110D", 3},
};
#define NFB 4
typedef struct { int band; uint32_t start; int rows; float level; int tx; const char *from, *to; } fake_burst_t;
static fake_burst_t fbursts[6];

static float wf_hz(int col) { return g_model.wf_lo_hz + (g_model.wf_hi_hz - g_model.wf_lo_hz) * col / WF_W; }

static void fake_waterfall(void)
{
    static uint32_t next_row_at, next_burst_at;
    if (!g_model.wf_hi_hz) {
        g_model.wf_up = true;
        snprintf(g_model.wf_name, sizeof g_model.wf_name, "GB7XYZ");
        g_model.wf_dial_hz = 7049750;
        g_model.wf_lo_hz = 236;
        g_model.wf_hi_hz = 5611;
        g_model.wf_rows_per_s = 7.5f;
        g_model.wf_n_bands = NFB;
        for (int i = 0; i < NFB; i++) {
            g_model.wf_bands[i].lo_hz = fbands[i].lo;
            g_model.wf_bands[i].hi_hz = fbands[i].hi;
            g_model.wf_bands[i].sub = fbands[i].sub;
            snprintf(g_model.wf_bands[i].name, sizeof g_model.wf_bands[i].name, "%s", fbands[i].name);
        }
        next_row_at = s_now;
        next_burst_at = s_now + 500;
    }
    // Pretend to be the device's audio side.
    g_model.wf_audio_on = g_model.wf_listen;
    g_model.wf_audio_dbfs = g_model.wf_listen ? -38 + frand() * 18 : NAN;
    if (s_now < next_row_at) return;
    next_row_at += 133;  // 7.5 rows a second
    uint32_t row = g_model.wf_row_seq;
    if (s_now >= next_burst_at) {
        for (int k = 0; k < 6; k++) {
            if (fbursts[k].rows && row < fbursts[k].start + fbursts[k].rows) continue;
            int b = (int)(frand() * NFB);
            fbursts[k].band = b;
            fbursts[k].start = row;
            fbursts[k].rows = b == 3 ? 20 + (int)(frand() * 25) : 6 + (int)(frand() * 18);
            fbursts[k].level = 0.55f + frand() * 0.4f;
            fbursts[k].tx = frand() < 0.2f;
            fbursts[k].from = fbursts[k].tx ? "GB7XYZ" : calls[(int)(frand() * 9)];
            fbursts[k].to = fbursts[k].tx ? calls[(int)(frand() * 9)] : "GB7XYZ";
            break;
        }
        next_burst_at = s_now + 1200 + (uint32_t)(frand() * 3500);
    }
    uint8_t *r = g_model.wf_rows[row % WF_ROWS];
    int tx = 0;
    for (int c = 0; c < WF_W; c++) {
        float hz = wf_hz(c);
        float v = 62 + frand() * 14 - hz / 500;  // noise, falling gently with frequency
        for (int k = 0; k < 6; k++) {
            fake_burst_t *fb = &fbursts[k];
            if (!fb->rows || row < fb->start || row >= fb->start + fb->rows) continue;
            const fake_band_t *bd = &fbands[fb->band];
            float mid = (bd->lo + bd->hi) / 2, half = (bd->hi - bd->lo) / 2;
            float d = fabsf(hz - mid) / half, s = 0;
            if (bd->kind == 0) s = expf(-powf((fabsf(hz - mid) - half * 0.5f) / 25, 2));   // two tones
            else if (bd->kind == 3) s = d < 0.95f ? 0.85f : 0;                            // flat and wide
            else s = d < 1 ? cosf(d * 1.5707f) : 0;                                       // a hump
            v += s * fb->level * 150 * (0.9f + frand() * 0.2f);
            if (s > 0.2f && fb->tx) tx = 1;
        }
        r[c] = (uint8_t)fminf(255, fmaxf(0, v));
        g_model.wf_spec[c] = r[c];
    }
    g_model.wf_row_tx[row % WF_ROWS] = (uint8_t)tx;
    g_model.wf_row_seq++;
    // Tag bursts as they end, like decoded frames.
    for (int k = 0; k < 6; k++) {
        fake_burst_t *fb = &fbursts[k];
        if (!fb->rows || row != fb->start + fb->rows - 1) continue;
        wf_tag_t *t = &g_model.wf_tags[g_model.wf_tag_seq % WF_TAGS];
        t->row = fb->start;
        t->rows = (uint16_t)fb->rows;
        t->sub = (uint8_t)fbands[fb->band].sub;
        t->tx = (uint8_t)fb->tx;
        t->snr_db = fb->tx ? NAN : 4 + fb->level * 20;
        snprintf(t->from, sizeof t->from, "%s", fb->from);
        snprintf(t->to, sizeof t->to, "%s", fb->to);
        g_model.wf_tag_seq++;
    }
}

// A GPSDO's status every 5 s: phase wandering a few ns, with one short loss of
// lock about twenty minutes before the end of the run-up hour.
static void fake_gpsdo(uint32_t t_rel_s)
{
    static float tic;
    static uint32_t missed;
    bool lost = t_rel_s > 2400 && t_rel_s < 2580;
    tic += (frand() - 0.5f) * 4;
    tic *= 0.97f;
    if (lost) tic += (frand() - 0.3f) * 8;
    if (t_rel_s == 2400) missed += 3;
    g_model.gps_up = true;
    g_model.gps_seq++;
    g_model.gps_msg_ms = s_now;
    snprintf(g_model.gps_name, sizeof g_model.gps_name, "GPSDO");
    snprintf(g_model.gps_state, sizeof g_model.gps_state, "%s", lost ? "HOLDOVER" : "LOCKED");
    g_model.gps_locked = !lost;
    g_model.gps_ref_ok = !lost;
    g_model.gps_pps = true;
    g_model.gps_locked_s = lost ? 0 : (t_rel_s > 2580 ? t_rel_s - 2580 : 452000 + t_rel_s);
    g_model.gps_lock_counter = lost ? (t_rel_s - 2400) * 8 : 454000;
    g_model.gps_lock_target = 1500;
    g_model.gps_missed_pps = missed;
    g_model.gps_tic_ns = tic;
    g_model.gps_ocxo_c = 44.2f;
    g_model.gps_ambient_c = 23.5f + frand() * 0.2f;
    g_model.gps_ppb_5m = -0.05f + (frand() - 0.5f) * 0.02f;
    g_model.gps_ppb_30m = -0.0145f;
    g_model.gps_dac = 30280 + (int)(tic * 3);
    snprintf(g_model.gps_fix, sizeof g_model.gps_fix, "3D");
    g_model.gps_sats_used = 10;
    g_model.gps_sats_view = 21;
    g_model.gps_hdop = 0.84f;
}

// A door camera: a made-up porch scene, a figure stepping up to the door, a
// few frames a second, and a ring three seconds in.
static uint16_t s_door_frame[320 * 240];
static void fake_doorbell(uint32_t t_ms)
{
    snprintf(g_model.door_name, sizeof g_model.door_name, "FRONT DOOR");
    g_model.door_up = true;
    if (t_ms >= 3000 && !g_model.door_ring_seq) {
        g_model.door_ring_seq = 1;
        g_model.door_ring_wall = 1763740000;
    }
    static uint32_t next;
    if (!g_model.door_want_video || s_now < next) return;
    next = s_now + 250;  // 4 frames a second
    float step = fminf(1, t_ms / 6000.0f);
    for (int y = 0; y < 240; y++)
        for (int x = 0; x < 320; x++) {
            // Sky, a path, a door frame on the right, a figure that grows as it approaches.
            int r, g, b;
            if (y < 90) { r = 120 - y / 2; g = 140 - y / 3; b = 170; }
            else { r = 70 + (y - 90) / 4; g = 66 + (y - 90) / 5; b = 58; }
            if (x > 236 && x < 300 && y > 30) { r = 92; g = 60; b = 40; }
            if (x > 244 && x < 292 && y > 40) { r = 40; g = 36; b = 34; }
            float fx = 150, fy = 200 - 40 * step, sc = 0.6f + 0.6f * step;
            float dx = (x - fx) / sc, dy = (y - fy) / sc;
            if ((dx * dx + (dy + 92) * (dy + 92) < 15 * 15) || (fabsf(dx) < 26 && dy > -76 && dy < 40)) {
                r = 30; g = 44; b = 80;
            }
            r += (int)(frand() * 6);
            s_door_frame[y * 320 + x] = (uint16_t)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
        }
    g_model.door_w = 320;
    g_model.door_h = 240;
    g_model.door_frame = s_door_frame;
    g_model.door_frame_seq++;
    g_model.door_fps = 4.0f;
}

// The 2.4 GHz band as the knob's radio might hear it: bursty Wi-Fi on
// channels 1, 6 and 11, Bluetooth hopping 1 MHz wide, and noise.
static void fake_scope(void)
{
    static uint32_t want_since, next_row;
    if (!g_model.sc_want) {
        g_model.sc_active = false;
        want_since = 0;
        return;
    }
    if (!want_since) {
        want_since = s_now;
        snprintf(g_model.sc_status, sizeof g_model.sc_status, "STOPPING WI-FI");
    }
    if (s_now - want_since < 1500) return;
    g_model.sc_active = true;
    snprintf(g_model.sc_status, sizeof g_model.sc_status, "capturing");
    g_model.sc_center_mhz = g_model.sc_center_mhz_want;
    g_model.sc_span_mhz = g_model.sc_span_mhz_want;
    g_model.sc_rows_per_s = 15;
    if (s_now < next_row) return;
    next_row = s_now + 66;
    float lo = g_model.sc_center_mhz - g_model.sc_span_mhz / 2;
    static const float wifi[] = {2412, 2437, 2462};
    static int on[3];
    for (int k = 0; k < 3; k++) if (frand() < 0.25f) on[k] = frand() < (k == 1 ? 0.6f : 0.35f);
    float bt = 2402 + (int)(frand() * 79);
    uint8_t *r = g_model.sc_rows[g_model.sc_row_seq % WF_ROWS];
    for (int c = 0; c < WF_W; c++) {
        float f = lo + g_model.sc_span_mhz * c / WF_W;
        float v = 60 + frand() * 12;
        for (int k = 0; k < 3; k++)
            if (on[k] && fabsf(f - wifi[k]) < 9) v += (k == 1 ? 90 : 60) * (fabsf(f - wifi[k]) < 8.3f ? 1 : 0.4f) * (0.85f + frand() * 0.3f);
        if (fabsf(f - bt) < 0.5f) v += 70;
        r[c] = (uint8_t)fminf(255, v);
        g_model.sc_spec[c] = r[c];
    }
    g_model.sc_row_seq++;
}

static int down_host = -1;
static uint32_t down_from;

static void fake_ping(void)
{
    // Each host is pinged every 2 s, staggered.
    for (int i = 0; i < NH; i++) {
        uint32_t slot = 2000 / NH * i;
        if ((s_now + 2000 - slot) % 2000 >= 33) continue;
        float r = hosts[i].base + hosts[i].jitter * frand() * frand() * 3;
        if (frand() < 0.01f) r = -1;
        if (i == down_host && s_now >= down_from) r = -1;
        model_host_sample(&g_model.hosts[i], r, s_now);
    }
}

static void fake_frame(uint32_t t)
{
    kiss_frame_t f = {0};
    f.t_ms = t;
    f.tx = frand() < 0.3f;
    f.subch = frand() < 0.7f ? 0 : 1;
    f.len = (uint16_t)(17 + frand() * frand() * 240);
    const char *other = calls[(int)(frand() * 9)];
    snprintf(f.src, sizeof f.src, "%s", f.tx ? "GB7XYZ" : other);
    snprintf(f.dst, sizeof f.dst, "%s", f.tx ? other : "GB7XYZ");
    model_kiss_push(&f);
}

static void setup_model(void)
{
    g_model.n_hosts = NH;
    for (int i = 0; i < NH; i++) {
        snprintf(g_model.hosts[i].name, sizeof g_model.hosts[i].name, "%s", hosts[i].name);
        snprintf(g_model.hosts[i].addr, sizeof g_model.hosts[i].addr, "%s", hosts[i].addr);
    }
    // A minute of history already in.
    for (int k = 0; k < 30; k++)
        for (int i = 0; i < NH; i++)
            model_host_sample(&g_model.hosts[i], hosts[i].base + hosts[i].jitter * frand() * frand() * 3, 0);
    snprintf(g_model.kiss_name, sizeof g_model.kiss_name, "GB7XYZ");
    snprintf(g_model.modem_name, sizeof g_model.modem_name, "GB7XYZ");
    g_model.kiss_up = true;
}

static void dump(const char *dir, int n)
{
    char path[512];
    snprintf(path, sizeof path, "%s/f%05d.rgb565", dir, n);
    FILE *f = fopen(path, "wb");
    fwrite(s_fb, 2, SCR_W * SCR_H, f);
    fclose(f);
}

int main(int argc, char **argv)
{
    const char *scen = argc > 1 ? argv[1] : "tour";
    const char *out = argc > 2 ? argv[2] : "out";
    int fps = argc > 3 ? atoi(argv[3]) : 15;
    mkdir(out, 0755);

    // Start the clock well past zero so pre-filled history has positive times.
    s_now = 3600000;
    lv_init();
    lv_tick_set_cb(tick_cb);
    lv_display_t *d = lv_display_create(SCR_W, SCR_H);
    lv_display_set_color_format(d, LV_COLOR_FORMAT_RGB565);
    lv_display_set_buffers(d, s_fb, NULL, sizeof s_fb, LV_DISPLAY_RENDER_MODE_DIRECT);
    lv_display_set_flush_cb(d, flush_cb);
    lv_indev_t *in = lv_indev_create();
    lv_indev_set_type(in, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(in, touch_cb);

    setup_model();
    // Ten minutes of traffic already heard.
    uint32_t t = s_now - 600000;
    while (t < s_now) {
        t += (uint32_t)(4000 + frand() * 16000);
        if (t < s_now) fake_frame(t);
    }
    for (int i = 0; i < NH; i++) g_model.hosts[i].changed_ms = s_now - 3600000;

    ui_page_cfg_t pages[] = {{"hosts", "Network"}, {"station", "Station"}, {"modem", "Modem"}, {"energy", "Energy"},
                             {"rooms", "Rooms"}, {"sky", "Sky"}, {"waterfall", "Waterfall"}, {"gpsdo", "GPSDO"},
                             {"doorbell", "Doorbell"}};
    const int npages = 9;
    ui_init(pages, npages);

    // Each scenario can start on a page of its own.
    const char *start_type = NULL;
    // Scenario timeline: (time s, action)
    float dur = 10;
    float taps[8] = {0};
    int ntaps = 0;
    float knob_at = -1;
    if (!strcmp(scen, "hosts")) {
        dur = 14;
        taps[ntaps++] = 6.0f;
        taps[ntaps++] = 10.0f;
    } else if (!strcmp(scen, "station")) {
        dur = 14;
        knob_at = 0.1f;
    } else if (!strcmp(scen, "doorbell")) {
        dur = 14;
        start_type = "energy";     // ringing pulls the knob over from whatever it shows
        taps[ntaps++] = 10.0f;     // tap to dismiss: back to Energy
    } else if (!strcmp(scen, "scope")) {
        dur = 16;
    } else if (!strcmp(scen, "menu")) {
        dur = 6;
    } else if (!strcmp(scen, "gpsdo")) {
        dur = 12;
        start_type = "gpsdo";
    } else if (!strcmp(scen, "waterfall")) {
        dur = 50;
        start_type = "waterfall";
        taps[ntaps++] = 44.0f;
    } else if (!strcmp(scen, "sky")) {
        dur = 40;
        start_type = "sky";
    } else if (!strcmp(scen, "rooms")) {
        dur = 16;
        start_type = "rooms";
    } else if (!strcmp(scen, "energy")) {
        dur = 16;
        start_type = "energy";
        taps[ntaps++] = 11.0f;
    } else if (!strcmp(scen, "modem")) {
        dur = 30;
        start_type = "modem";  // handled below: two steps
        taps[ntaps++] = 20.0f;
    } else if (!strcmp(scen, "alert")) {
        dur = 24;
        down_host = 0;
        knob_at = 0.1f;
        down_from = s_now + 16000;
    } else {
        dur = 20;
        knob_at = 8.0f;
        taps[ntaps++] = 3.0f;
    }

    for (int i = 0; start_type && i < npages; i++)
        if (!strcmp(pages[i].type, start_type)) ui_goto(i, false);
    if (!strcmp(scen, "gpsdo")) {
        // An hour of status messages, fast, so the page has its history ring.
        for (uint32_t t = 0; t < 3600; t += 5) {
            s_now += 5000;
            fake_gpsdo(t);
            lv_timer_handler();
        }
    }
    uint32_t start = s_now, next_frame_at = start, next_kiss = s_now + 2500;
    int nframe = 0, tapi = 0;
    bool knobbed = false;
    uint32_t frame_ms = 1000 / fps;
    while (s_now - start < dur * 1000) {
        float ts = (s_now - start) / 1000.0f;
        fake_ping();
        fake_metrics();
        fake_energy();
        fake_rooms();
        if (!strcmp(scen, "sky")) fake_adsb();  // its notices would pull other scenarios to the Sky page
        fake_waterfall();
        if (!strcmp(scen, "doorbell")) fake_doorbell(s_now - start);
        fake_scope();
        if (!strcmp(scen, "gpsdo") && (s_now % 5000) < 11) fake_gpsdo(3600 + (s_now - start) / 1000);
        if (s_now >= next_kiss) {
            fake_frame(s_now);
            next_kiss = s_now + (uint32_t)(1500 + frand() * 5000);
        }
        if (knob_at >= 0 && !knobbed && ts >= knob_at) {
            ui_knob(1);
            knobbed = true;
        }
        s_pressed = false;
        // Waterfall: after the tap at 44 s starts listening, turn the volume up.
        if (!strcmp(scen, "waterfall") && ts >= 45.0f && ts < 47.0f && ((int)(ts * 1000) % 400) < 11) ui_knob(1);
        if (!strcmp(scen, "rooms")) {
            // 3 s: tap the middle to edit; 3.6, 4.0, 4.4 s: three clicks up; then it saves itself.
            // 11 s: tap the right edge for the next room.
            static int step;
            static const float at[] = {3.0f, 3.6f, 4.0f, 4.4f, 11.0f};
            if (step < 5 && ts >= at[step]) {
                if (step == 0 || step == 4) {
                    s_px = step == 0 ? 180 : 330;
                    s_py = 180;
                    s_pressed = ts < at[step] + 0.07f;
                    if (!s_pressed) step++;
                } else {
                    ui_knob(1);
                    step++;
                }
            }
        }
        if (!strcmp(scen, "scope")) {
            // Long press for the menu, tap 2.4 GHZ SCOPE, then tune up and change the span.
            s_px = 180;
            s_py = ts < 2.6f ? 120 : 298;
            s_pressed = (ts >= 1.0f && ts < 2.5f) || (ts >= 3.0f && ts < 3.07f) || (ts >= 12.0f && ts < 12.07f);
            if (ts >= 12.0f) s_py = 250;
            if ((ts >= 8.0f && ts < 8.011f) || (ts >= 8.4f && ts < 8.411f)) ui_knob(1);
        }
        if (!strcmp(scen, "menu")) {
            // Hold for 1.5 s to open the system menu, then turn the bezel down three clicks.
            s_px = 180;
            s_py = 120;
            s_pressed = ts >= 1.0f && ts < 2.5f;
            if ((ts >= 3.5f && ts < 3.511f) || (ts >= 3.8f && ts < 3.811f) || (ts >= 4.1f && ts < 4.111f)) ui_knob(-1);
        }
        if (tapi < ntaps && ts >= taps[tapi]) {
            s_px = 180;
            s_py = 180;
            s_pressed = ts < taps[tapi] + 0.07f;
            if (!s_pressed) tapi++;
        }
        lv_timer_handler();
        if (s_now >= next_frame_at) {
            dump(out, nframe++);
            next_frame_at += frame_ms;
        }
        s_now += 11;
    }
    fprintf(stderr, "%d frames\n", nframe);
    return 0;
}
