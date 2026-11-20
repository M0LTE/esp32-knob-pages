#include "metrics_parse.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Copies the value of label `key` from a {a="x",b="y"} block. False if absent.
static bool label(const char *lb, const char *end, const char *key, char *out, size_t outn)
{
    size_t kl = strlen(key);
    const char *p = lb;
    while (p < end) {
        while (p < end && (*p == '{' || *p == ',' || *p == ' ')) p++;
        const char *k = p;
        while (p < end && *p != '=') p++;
        if (p >= end) return false;
        bool match = (size_t)(p - k) == kl && !strncmp(k, key, kl);
        p++;  // '='
        if (p >= end || *p != '"') return false;
        p++;
        size_t o = 0;
        while (p < end && *p != '"') {
            char c = *p++;
            if (c == '\\' && p < end) c = *p++;
            if (match && o + 1 < outn) out[o++] = c;
        }
        p++;  // closing quote
        if (match) {
            out[o] = 0;
            return true;
        }
    }
    return false;
}

static prom_station_t *station(prom_snap_t *s, const char *call, const char *mode)
{
    for (int i = 0; i < s->n; i++)
        if (!strcmp(s->st[i].call, call) && !strcmp(s->st[i].mode, mode)) return &s->st[i];
    if (s->n >= PROM_MAX_STATIONS) return NULL;
    prom_station_t *p = &s->st[s->n++];
    memset(p, 0, sizeof *p);
    snprintf(p->call, sizeof p->call, "%s", call);
    snprintf(p->mode, sizeof p->mode, "%s", mode);
    p->subch = -1;
    return p;
}

int prom_parse(const char *text, size_t n, prom_snap_t *out)
{
    memset(out, 0, sizeof *out);
    int understood = 0;
    const char *p = text, *end = text + n;
    char name[64], call[12], mode[24], sub[8], vbuf[40];
    while (p < end) {
        const char *eol = memchr(p, '\n', end - p);
        if (!eol) eol = end;
        const char *line = p;
        p = eol + 1;
        if (line == eol || *line == '#') continue;
        // name
        const char *q = line;
        while (q < eol && *q != '{' && *q != ' ') q++;
        size_t nl = q - line;
        if (nl == 0 || nl >= sizeof name) continue;
        memcpy(name, line, nl);
        name[nl] = 0;
        if (strncmp(name, "pdn_", 4)) continue;
        const char *lb = NULL, *lbe = NULL;
        if (q < eol && *q == '{') {
            lb = q;
            bool inq = false;
            while (q < eol && (inq || *q != '}')) {
                if (*q == '"' && q[-1] != '\\') inq = !inq;
                q++;
            }
            if (q >= eol) continue;
            lbe = q;
            q++;
        }
        while (q < eol && *q == ' ') q++;
        size_t vl = 0;
        while (q + vl < eol && q[vl] != ' ' && q[vl] != '\r' && vl + 1 < sizeof vbuf) vl++;
        if (!vl) continue;
        memcpy(vbuf, q, vl);
        vbuf[vl] = 0;
        char *ve;
        double v = strtod(vbuf, &ve);
        if (ve == vbuf) continue;

        if (!strncmp(name, "pdn_station_", 12)) {
            if (!lb || !label(lb, lbe, "station", call, sizeof call) || !label(lb, lbe, "mode", mode, sizeof mode)) continue;
            prom_station_t *s = station(out, call, mode);
            if (!s) continue;
            const char *f = name + 12;
            if (!strcmp(f, "info")) {
                if (label(lb, lbe, "sub_channel", sub, sizeof sub)) s->subch = atoi(sub);
            } else if (!strcmp(f, "frames_total")) s->frames = v;
            else if (!strcmp(f, "bytes_total")) s->bytes = v;
            else if (!strcmp(f, "corrected_bytes_total")) s->corrected = v;
            else if (!strcmp(f, "snr_db_sum")) s->snr_sum = v;
            else if (!strcmp(f, "frames_with_snr_total")) s->snr_n = v;
            else if (!strcmp(f, "snr_db_last")) s->snr_last = v, s->has_snr_last = !isnan(v);
            else if (!strcmp(f, "frames_with_offset_total")) s->off_n = v;
            else if (!strcmp(f, "frequency_offset_hz_last")) s->off_last = v, s->has_off_last = !isnan(v);
            else continue;
            understood++;
            continue;
        }
        double *g = NULL;
        if (!strcmp(name, "pdn_receive_realtime_ratio")) g = &out->rt_ratio;
        else if (!strcmp(name, "pdn_receive_behind")) g = &out->behind;
        else if (!strcmp(name, "pdn_receive_input_losses_total")) g = &out->input_losses;
        else if (!strcmp(name, "pdn_transmit_stalls_total")) g = &out->tx_stalls;
        else if (!strcmp(name, "pdn_transmit_stall_seconds_total")) g = &out->tx_stall_s;
        else if (!strcmp(name, "pdn_frames_uncounted_total")) g = &out->uncounted;
        else if (!strcmp(name, "pdn_receive_busy_seconds_total")) g = &out->busy_s;
        else if (!strcmp(name, "pdn_receive_seconds_total")) g = &out->recv_s;
        if (g) {
            *g = v;
            out->globals++;
            understood++;
        }
    }
    return understood;
}

void metrics_state_init(metrics_state_t *st) { memset(st, 0, sizeof *st); }

// Counter increase since the oldest sample within the window; a counter that
// went down (the modem restarted) counts from zero.
static double windowed(const uint32_t *t, const double *v, int head, int n, int cap, uint32_t now, uint32_t win,
                       double cur)
{
    double old = cur;
    for (int k = n - 1; k >= 0; k--) {  // oldest first
        int i = (head - 1 - k + cap) % cap;
        if (now - t[i] <= win) {
            old = v[i];
            break;
        }
    }
    double d = cur - old;
    return d < 0 ? cur : d;
}

static mtrack_t *track(metrics_state_t *st, const char *call, const char *mode, uint32_t now)
{
    mtrack_t *oldest = &st->tr[0];
    for (int i = 0; i < MST_TRACK; i++) {
        mtrack_t *t = &st->tr[i];
        if (t->call[0] && !strcmp(t->call, call) && !strcmp(t->mode, mode)) return t;
        if (!t->call[0]) {
            oldest = t;
            break;
        }
        if ((int32_t)(t->last_seen_ms - oldest->last_seen_ms) < 0) oldest = t;
    }
    memset(oldest, 0, sizeof *oldest);
    snprintf(oldest->call, sizeof oldest->call, "%s", call);
    snprintf(oldest->mode, sizeof oldest->mode, "%s", mode);
    return oldest;
}

void metrics_apply(metrics_state_t *st, const prom_snap_t *s, uint32_t now, model_t *m)
{
    // Globals
    m->rt_ratio = (float)s->rt_ratio;
    m->behind = s->behind != 0;
    m->input_losses = (uint32_t)s->input_losses;
    m->tx_stalls = (uint32_t)s->tx_stalls;
    m->tx_stall_s = (float)s->tx_stall_s;
    m->uncounted = (uint32_t)s->uncounted;
    if (st->have_prev) {
        double dr = s->recv_s - st->prev_recv, db = s->busy_s - st->prev_busy;
        if (dr > 0.5 && db >= 0) {
            float b = (float)(100.0 * db / dr);
            m->busy_pct = b < 0 ? 0 : b > 100 ? 100 : b;
        }
    }
    st->prev_recv = s->recv_s;
    st->prev_busy = s->busy_s;
    st->have_prev = true;

    st->gt[st->ghead] = now;
    st->g_losses[st->ghead] = s->input_losses;
    st->g_stalls[st->ghead] = s->tx_stalls;
    st->ghead = (st->ghead + 1) % MG_HIST;
    if (st->gn < MG_HIST) st->gn++;
    m->input_losses_1h = (uint32_t)windowed(st->gt, st->g_losses, st->ghead, st->gn, MG_HIST, now, 3600000, s->input_losses);
    m->tx_stalls_1h = (uint32_t)windowed(st->gt, st->g_stalls, st->ghead, st->gn, MG_HIST, now, 3600000, s->tx_stalls);

    // Stations
    m->n_mstations = 0;
    for (int i = 0; i < s->n && m->n_mstations < MODEL_MAX_MSTATIONS; i++) {
        const prom_station_t *p = &s->st[i];
        mtrack_t *t = track(st, p->call, p->mode, now);
        if (t->seen_before && p->frames > t->frames) t->heard_ms = now;
        t->frames = p->frames;
        t->seen_before = true;
        t->last_seen_ms = now;
        t->t[t->head] = now;
        t->corr[t->head] = p->corrected;
        t->head = (t->head + 1) % MST_HIST;
        if (t->n < MST_HIST) t->n++;

        mstation_t *o = &m->mstations[m->n_mstations++];
        memset(o, 0, sizeof *o);
        snprintf(o->call, sizeof o->call, "%s", p->call);
        snprintf(o->mode, sizeof o->mode, "%s", p->mode);
        o->subch = p->subch < 0 ? 0 : (uint8_t)p->subch;
        o->frames = (uint32_t)p->frames;
        o->bytes = (uint32_t)p->bytes;
        o->corrected = (uint32_t)p->corrected;
        o->snr_last = p->has_snr_last && p->snr_n > 0 ? (float)p->snr_last : NAN;
        o->snr_mean = p->snr_n > 0 ? (float)(p->snr_sum / p->snr_n) : NAN;
        o->offset_last = p->has_off_last && p->off_n > 0 ? (float)p->off_last : NAN;
        o->heard_ms = t->heard_ms;
        o->corrected_10m = (uint32_t)windowed(t->t, t->corr, t->head, t->n, MST_HIST, now, 600000, p->corrected);
    }
}
