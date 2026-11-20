// Host tests for kiss_parse.c and metrics_parse.c.
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "kiss_parse.h"
#include "metrics_parse.h"

model_t g_model;
static int fails, checks;

#define CHECK(cond, ...)                                    \
    do {                                                    \
        checks++;                                           \
        if (!(cond)) {                                      \
            fails++;                                        \
            printf("FAIL %s:%d: ", __FILE__, __LINE__);     \
            printf(__VA_ARGS__);                            \
            printf("\n");                                   \
        }                                                   \
    } while (0)

// ---- KISS ----

static size_t ax25_call(uint8_t *o, const char *call, int ssid, int last)
{
    for (int i = 0; i < 6; i++) o[i] = (uint8_t)((i < (int)strlen(call) ? call[i] : ' ') << 1);
    o[6] = (uint8_t)(0x60 | (ssid << 1) | (last ? 1 : 0));
    return 7;
}

static kiss_frame_t got[16];
static int ngot;
static const char *own = "GB7XYZ";

static void on_frame(void *ctx, const uint8_t *f, size_t n)
{
    kiss_frame_t k = {0};
    if (kiss_frame_decode(f, n, own, &k) && ngot < 16) got[ngot++] = k;
}

static void test_kiss(void)
{
    uint8_t ax[64];
    size_t n = 0;
    n += ax25_call(ax + n, "APRS", 0, 0);
    n += ax25_call(ax + n, "G0ABC", 7, 1);
    ax[n++] = 0x03;  // UI
    ax[n++] = 0xF0;
    ax[n++] = 0xC0;  // payload bytes that need escaping
    ax[n++] = 0xDB;
    ax[n++] = 'x';
    size_t axlen = n;

    // Wrap: port 2 data, escaped.
    uint8_t stream[256];
    size_t s = 0;
    stream[s++] = 0xC0;
    stream[s++] = 0x20;
    for (size_t i = 0; i < axlen; i++) {
        if (ax[i] == 0xC0) stream[s++] = 0xDB, stream[s++] = 0xDC;
        else if (ax[i] == 0xDB) stream[s++] = 0xDB, stream[s++] = 0xDD;
        else stream[s++] = ax[i];
    }
    stream[s++] = 0xC0;
    // A non-data command (TXDELAY) and a repeated FEND.
    stream[s++] = 0xC0;
    stream[s++] = 0x01;
    stream[s++] = 0x32;
    stream[s++] = 0xC0;
    // A frame from the station itself, port 0, SSID 0... sent as GB7XYZ-10.
    stream[s++] = 0x00;
    size_t start = s;
    s += ax25_call(stream + s, "G4ABC", 0, 0);
    s += ax25_call(stream + s, "GB7XYZ", 10, 1);
    stream[s++] = 0x3F;  // SABM-ish control
    (void)start;
    stream[s++] = 0xC0;
    // A runt data frame (too short for addresses).
    stream[s++] = 0x00;
    stream[s++] = 0x82;
    stream[s++] = 0xC0;

    kiss_dec_t d;
    kiss_dec_init(&d);
    ngot = 0;
    // Feed one byte at a time to exercise state across reads.
    for (size_t i = 0; i < s; i++) kiss_dec_feed(&d, stream + i, 1, on_frame, NULL);
    CHECK(ngot == 2, "expected 2 frames, got %d", ngot);
    if (ngot >= 1) {
        CHECK(!strcmp(got[0].src, "G0ABC-7"), "src %s", got[0].src);
        CHECK(!strcmp(got[0].dst, "APRS"), "dst %s", got[0].dst);
        CHECK(got[0].len == axlen, "len %u want %zu", got[0].len, axlen);
        CHECK(got[0].subch == 2, "subch %u", got[0].subch);
        CHECK(got[0].tx == 0, "tx %u", got[0].tx);
    }
    if (ngot >= 2) {
        CHECK(!strcmp(got[1].src, "GB7XYZ-10"), "src %s", got[1].src);
        CHECK(!strcmp(got[1].dst, "G4ABC"), "dst %s", got[1].dst);
        CHECK(got[1].tx == 1, "own frame not tx");
        CHECK(got[1].subch == 0, "subch %u", got[1].subch);
        CHECK(got[1].len == 15, "len %u", got[1].len);
    }
    // Same base call must match exactly, not as a prefix.
    kiss_frame_t k;
    uint8_t f[16];
    f[0] = 0;
    ax25_call(f + 1, "X", 0, 0);
    ax25_call(f + 8, "GB7XY", 0, 1);
    uint8_t f2[17];
    memcpy(f2, f, 15);
    f2[15] = 0x03;
    CHECK(kiss_frame_decode(f2, 16, "GB7XYZ", &k) && k.tx == 0, "GB7XY counted as GB7XYZ");
    CHECK(kiss_frame_decode(f2, 16, "gb7xy-3", &k) && k.tx == 1, "case/ssid-insensitive own call");
    // Garbage address bytes are rejected.
    memset(f2 + 1, 0xFF, 14);
    CHECK(!kiss_frame_decode(f2, 16, "GB7XYZ", &k), "garbage accepted");
    // Oversize frame is dropped and the next one still decodes.
    static uint8_t big[2000];
    memset(big, 0x41, sizeof big);
    big[0] = 0xC0;
    big[1] = 0x00;
    kiss_dec_init(&d);
    ngot = 0;
    kiss_dec_feed(&d, big, sizeof big, on_frame, NULL);
    kiss_dec_feed(&d, stream, s, on_frame, NULL);
    CHECK(ngot == 2, "after oversize: %d frames", ngot);
}

// ---- metrics ----

static char *slurp(const char *path, size_t *n)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    *n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *b = malloc(*n + 1);
    if (fread(b, 1, *n, f) != *n) { fclose(f); free(b); return NULL; }
    b[*n] = 0;
    fclose(f);
    return b;
}

static const mstation_t *find(const model_t *m, const char *call, const char *mode)
{
    for (int i = 0; i < m->n_mstations; i++)
        if (!strcmp(m->mstations[i].call, call) && !strcmp(m->mstations[i].mode, mode)) return &m->mstations[i];
    return NULL;
}

// Replace the value on the first line starting with `prefix`.
static char *bump(const char *text, const char *prefix, const char *newval)
{
    char pat[160];
    snprintf(pat, sizeof pat, "\n%s", prefix);
    const char *p = strstr(text, pat);
    if (!p) return NULL;
    p++;
    const char *eol = strchr(p, '\n');
    const char *sp = eol;
    while (sp > p && sp[-1] != ' ') sp--;
    size_t n = strlen(text) + strlen(newval) + 1;
    char *o = malloc(n);
    size_t a = sp - text;
    memcpy(o, text, a);
    strcpy(o + a, newval);
    strcat(o, eol);
    return o;
}

static void test_metrics(const char *path)
{
    size_t n;
    char *txt = slurp(path, &n);
    CHECK(txt != NULL, "cannot read %s", path);
    if (!txt) return;
    static prom_snap_t s;
    int u = prom_parse(txt, n, &s);
    printf("metrics: %d samples understood, %d stations, %d globals\n", u, s.n, s.globals);
    CHECK(s.n == 14, "stations %d", s.n);
    CHECK(s.globals == 8, "globals %d", s.globals);
    CHECK(fabs(s.rt_ratio - 1.000003) < 1e-9, "rt %f", s.rt_ratio);
    CHECK(s.tx_stalls == 167, "stalls %f", s.tx_stalls);
    CHECK(s.uncounted == 408, "uncounted %f", s.uncounted);

    static metrics_state_t st;
    metrics_state_init(&st);
    static model_t m;
    uint32_t t = 100000;
    metrics_apply(&st, &s, t, &m);
    const mstation_t *agm = find(&m, "GB7AAA", "bpsk300-il2pc");
    CHECK(agm != NULL, "GB7AAA missing");
    if (agm) {
        CHECK(agm->frames == 295, "frames %u", agm->frames);
        CHECK(agm->subch == 2, "subch %u", agm->subch);
        CHECK(agm->heard_ms == 0, "heard on first poll");
        CHECK(!isnan(agm->snr_mean), "snr mean NaN");
        printf("GB7AAA: frames %u bytes %u corrected %u snr last %.1f mean %.2f offset %.1f\n", agm->frames,
               agm->bytes, agm->corrected, agm->snr_last, agm->snr_mean, agm->offset_last);
    }
    const mstation_t *bpq = find(&m, "GB7CCC", "afsk300-il2pc");
    CHECK(bpq && bpq->subch == 0, "GB7CCC afsk subch");
    const mstation_t *gm = find(&m, "GM0KKK", "ms110d-wn4");
    CHECK(gm && gm->subch == 4, "GM0KKK subch");
    CHECK(m.n_mstations == 14, "model stations %d", m.n_mstations);
    CHECK(m.tx_stalls == 167 && m.input_losses == 1 && m.uncounted == 408, "globals into model");
    CHECK(m.tx_stalls_1h == 0, "stalls_1h on first poll %u", m.tx_stalls_1h);
    for (int i = 0; i < m.n_mstations; i++)
        printf("  %-8s %-16s sub %u frames %5u snr %6.1f mean %6.2f off %7.1f corr %u\n", m.mstations[i].call,
               m.mstations[i].mode, m.mstations[i].subch, m.mstations[i].frames, m.mstations[i].snr_last,
               m.mstations[i].snr_mean, m.mstations[i].offset_last, m.mstations[i].corrected);

    // Second poll 10 s later: GB7AAA heard (+3 frames, +40 corrected), 2 new stalls,
    // receive loop busy for 4 of the 10 s.
    char *t2 = bump(txt, "pdn_station_frames_total{station=\"GB7AAA\",mode=\"bpsk300-il2pc\"}", "298");
    char buf[64];
    snprintf(buf, sizeof buf, "%u", agm ? agm->corrected + 40 : 40);
    char *t3 = bump(t2, "pdn_station_corrected_bytes_total{station=\"GB7AAA\"", buf);
    char *t4 = bump(t3, "pdn_transmit_stalls_total", "169");
    char *t5 = bump(t4, "pdn_receive_seconds_total", "77062.004791397");
    char *t6 = bump(t5, "pdn_receive_busy_seconds_total", "29100.644755923");
    prom_parse(t6, strlen(t6), &s);
    metrics_apply(&st, &s, t + 10000, &m);
    agm = find(&m, "GB7AAA", "bpsk300-il2pc");
    CHECK(agm && agm->heard_ms == t + 10000, "heard_ms not set");
    CHECK(agm && agm->corrected_10m == 40, "corrected_10m %u", agm ? agm->corrected_10m : 0);
    const mstation_t *bex = find(&m, "GB7BBB", "bpsk300-il2pc");
    CHECK(bex && bex->heard_ms == 0, "GB7BBB heard without new frames");
    CHECK(m.tx_stalls_1h == 2, "tx_stalls_1h %u", m.tx_stalls_1h);
    CHECK(fabsf(m.busy_pct - 40.0f) < 0.01f, "busy %.3f", m.busy_pct);

    // Much later (> 10 min, < 1 h) with no change: the 10-minute window empties, the hour does not.
    for (int i = 2; i < 70; i++) metrics_apply(&st, &s, t + i * 10000, &m);
    agm = find(&m, "GB7AAA", "bpsk300-il2pc");
    CHECK(agm && agm->corrected_10m == 0, "corrected_10m after 11 min %u", agm ? agm->corrected_10m : 0);
    CHECK(m.tx_stalls_1h == 2, "tx_stalls_1h after 11 min %u", m.tx_stalls_1h);
    CHECK(agm && agm->heard_ms == t + 10000, "heard_ms kept");
    // After more than an hour the stalls drop out.
    for (int i = 70; i < 380; i++) metrics_apply(&st, &s, t + i * 10000, &m);
    CHECK(m.tx_stalls_1h == 0, "tx_stalls_1h after 63 min %u", m.tx_stalls_1h);

    // Modem restart: counters fall back; treat as fresh, not negative.
    char *r = bump(txt, "pdn_transmit_stalls_total", "1");
    prom_parse(r, strlen(r), &s);
    metrics_apply(&st, &s, t + 381 * 10000, &m);
    CHECK(m.tx_stalls_1h <= 1, "after restart %u", m.tx_stalls_1h);

    // Robustness: empty, junk and NaN values.
    CHECK(prom_parse("", 0, &s) == 0, "empty");
    const char *junk = "pdn_station_snr_db_last{station=\"X1\",mode=\"m\"} NaN\n"
                       "pdn_station_frames_with_snr_total{station=\"X1\",mode=\"m\"} 0\n"
                       "pdn_station_info{station=\"X1\",mode=\"m\",sub_channel=\"3\"} 1\n"
                       "garbage line\n{\n pdn_\npdn_receive_behind 1";
    prom_parse(junk, strlen(junk), &s);
    CHECK(s.n == 1 && s.st[0].subch == 3 && !s.st[0].has_snr_last, "junk parse");
    CHECK(s.behind == 1, "last line without newline");
    free(txt); free(t2); free(t3); free(t4); free(t5); free(t6); free(r);
}

int main(int argc, char **argv)
{
    test_kiss();
    test_metrics(argc > 1 ? argv[1] : "metrics_sample.txt");
    printf("%d checks, %d failed\n", checks, fails);
    return fails ? 1 : 0;
}
