// Host tests for rx_audio.c: block parsing, resampling and the fill
// controller against synthetic arrival timing (jitter and clock drift).
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "rx_audio.h"

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

static uint32_t rng = 1;
static float frand(void) { rng = rng * 1664525u + 1013904223u; return (rng >> 8) / 16777216.0f; }

static void test_block(void)
{
    uint8_t m[12] = {0x02, 0, 0, 0, 0x34, 0x12, 0xff, 0xff, 0x00, 0x80, 0x01, 0x00};
    const uint8_t *pcm;
    CHECK(rxa_block(m, 12, &pcm) == 4 && pcm == m + 4, "block");
    static int16_t buf[64];
    rxa_t a;
    rxa_init(&a, buf, 64);
    rxa_reset(&a, 48000, 48000, 8);
    rxa_push_le(&a, pcm, 4);
    CHECK(a.ring[0] == 0x1234 && a.ring[1] == -1 && a.ring[2] == -32768 && a.ring[3] == 1, "little endian");
    CHECK(rxa_block(m, 11, &pcm) == -1, "odd length");
    m[0] = 0x01;
    CHECK(rxa_block(m, 12, &pcm) == -1, "a spectrum line");
    CHECK(rxa_block(m, 3, &pcm) == -1, "short");
}

typedef struct {
    double secs, drift_ppm, jitter_ms;
    uint32_t in_rate, block;
    double gap_at, gap_len;  // a stall in arrivals
} sim_t;

// Runs the sender at in_rate * (1 + drift) in blocks with random lateness,
// and the DAC at exactly 48 kHz in 5 ms pulls. Returns the fill (in ms)
// range over the last half and fills the counters.
static void run(const sim_t *s, rxa_t *a, double *min_ms, double *max_ms, double *final_ms, int *real_pct)
{
    const uint32_t out = 48000, pull = 240;
    static int16_t chunk[48000], o[240];
    for (uint32_t i = 0; i < s->block; i++) chunk[i] = (int16_t)(8000 * sin(i * 0.05));
    rxa_reset(a, s->in_rate, out, s->in_rate * 3 / 10);
    double t = 0, next_block = 0, sent = 0;
    double in_rate = s->in_rate * (1 + s->drift_ppm * 1e-6);
    long real = 0, total = 0;
    *min_ms = 1e9, *max_ms = -1;
    for (double tp = 0; tp < s->secs; tp += (double)pull / out) {
        // Blocks due by now (each is late by up to jitter).
        while (next_block <= tp) {
            double when = sent / in_rate;  // when the block's last sample was made
            double late = frand() * s->jitter_ms / 1000;
            bool stalled = s->gap_len > 0 && when >= s->gap_at && when < s->gap_at + s->gap_len;
            if (stalled) late += s->gap_at + s->gap_len - when;
            next_block = when + late;
            if (next_block > tp) break;
            rxa_push(a, chunk, s->block);
            sent += s->block;
            next_block = sent / in_rate;
        }
        int r = rxa_pull(a, o, pull);
        total += pull;
        if (tp > s->secs / 2) {
            real += r;
            double ms = rxa_fill(a) * 1000.0 / s->in_rate;
            if (ms < *min_ms) *min_ms = ms;
            if (ms > *max_ms) *max_ms = ms;
            *final_ms = ms;
        }
        t = tp;
    }
    (void)t;
    *real_pct = (int)(100.0 * real / (total / 2.0) + 0.5);
}

static void test_drift(void)
{
    static int16_t ring[48000 * 2];
    rxa_t a;
    rxa_init(&a, ring, sizeof ring / sizeof ring[0]);
    double mn, mx, fin;
    int pct;
    sim_t fast = {600, 150, 60, 48000, 1024, 0, 0};
    run(&fast, &a, &mn, &mx, &fin, &pct);
    printf("48k +150 ppm, 60 ms jitter: fill %.0f..%.0f ms (end %.0f), corr %+.5f, underruns %u trims %u, real %d%%\n",
           mn, mx, fin, a.corr, a.underruns, a.trims, pct);
    CHECK(a.underruns == 0 && a.trims == 0 && mn > 200 && mx < 400 && pct == 100, "fast sender held");
    sim_t slow = {600, -150, 60, 48000, 1024, 0, 0};
    run(&slow, &a, &mn, &mx, &fin, &pct);
    printf("48k -150 ppm, 60 ms jitter: fill %.0f..%.0f ms (end %.0f), corr %+.5f, underruns %u trims %u, real %d%%\n",
           mn, mx, fin, a.corr, a.underruns, a.trims, pct);
    CHECK(a.underruns == 0 && mn > 200 && mx < 400 && pct == 100, "slow sender held");
    sim_t r12 = {300, 80, 100, 12000, 512, 0, 0};
    run(&r12, &a, &mn, &mx, &fin, &pct);
    printf("12k +80 ppm, 100 ms jitter: fill %.0f..%.0f ms, underruns %u, real %d%%\n", mn, mx, a.underruns, pct);
    CHECK(a.underruns == 0 && mn > 150 && mx < 450 && pct == 100, "12 kHz upsampled and held");
    // A 1 s stall halfway: one underrun, silence, refill, then back near target.
    sim_t stall = {200, 50, 40, 48000, 1024, 150, 1.0};
    run(&stall, &a, &mn, &mx, &fin, &pct);
    printf("1 s stall at 150 s: underruns %u trims %u, fill end %.0f ms, real %d%%\n", a.underruns, a.trims, fin, pct);
    CHECK(a.underruns == 1 && fin > 200 && fin < 400 && pct >= 97 && pct < 100, "stall recovered");
}

static void test_resample(void)
{
    // 12 kHz ramp to 48 kHz: four outputs per input, interpolated.
    static int16_t ring[1024], in[256], out[400];
    rxa_t a;
    rxa_init(&a, ring, 1024);
    rxa_reset(&a, 12000, 48000, 100);
    for (int i = 0; i < 256; i++) in[i] = (int16_t)(i * 100);
    rxa_push(&a, in, 256);
    int r = rxa_pull(&a, out, 400);
    CHECK(r == 400, "pulled %d", r);
    bool mono = true;
    for (int i = 1; i < 400; i++) mono &= out[i] >= out[i - 1];
    CHECK(mono && out[0] == 0 && abs(out[4] - 100) <= 1 && abs(out[6] - 150) <= 1, "x4 interpolation %d %d %d", out[0],
          out[4], out[6]);
    // Below target nothing plays.
    rxa_reset(&a, 48000, 48000, 500);
    rxa_push(&a, in, 256);
    CHECK(rxa_pull(&a, out, 100) == 0 && out[0] == 0, "buffering");
}

int main(void)
{
    test_block();
    test_resample();
    test_drift();
    printf("rx audio: %d checks, %d failed\n", checks, fails);
    return fails ? 1 : 0;
}
