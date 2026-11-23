// Host test of the 2.4 GHz scope's spectrum maths: a tone at a known RF
// frequency, captured the way the receiver delivers it (RF above the LO at
// negative frequency, 10-bit I and Q packed in a word), must come out in the
// column for that frequency in every span, wherever the span is centred, and
// a strong carrier just outside a single-capture span must not fold into it.
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "scope_stitch.h"

#define N 1024
#define PAIRS (16380 / N * N)

static int fails;
#define CHECK(c, ...) \
    do { \
        if (!(c)) { \
            fails++; \
            printf("FAIL %s:%d: ", __FILE__, __LINE__); \
            printf(__VA_ARGS__); \
            printf("\n"); \
        } \
    } while (0)

// A plain radix-2 FFT, natural order out.
static void fft(float *d, int n)
{
    for (int i = 1, j = 0; i < n; i++) {
        int bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) {
            float tr = d[2 * i], ti = d[2 * i + 1];
            d[2 * i] = d[2 * j], d[2 * i + 1] = d[2 * j + 1];
            d[2 * j] = tr, d[2 * j + 1] = ti;
        }
    }
    for (int len = 2; len <= n; len <<= 1) {
        double ang = -2 * M_PI / len;
        for (int i = 0; i < n; i += len)
            for (int k = 0; k < len / 2; k++) {
                float wr = (float)cos(ang * k), wi = (float)sin(ang * k);
                float *a = d + 2 * (i + k), *b = d + 2 * (i + k + len / 2);
                float tr = b[0] * wr - b[1] * wi, ti = b[0] * wi + b[1] * wr;
                b[0] = a[0] - tr, b[1] = a[1] - ti;
                a[0] += tr, a[1] += ti;
            }
    }
}

static uint32_t s_iq[PAIRS];
static float s_win[N], s_work[2 * N], s_pow[N];
static unsigned s_seed = 1;
static float noise(void)
{
    s_seed = s_seed * 1103515245u + 12345u;
    return ((s_seed >> 8) & 0xffff) / 65536.0f - 0.5f;
}

// What the receiver hands over when tuned to lo_mhz with a carrier at rf_mhz:
// the carrier at minus (rf - lo), a little noise and a DC offset. A carrier
// outside the capture's width is taken out by the analogue filter; or, with
// s_filter_half set, the filter passes that many MHz either side whatever the
// rate, and a rate too low for it folds the carrier back in, as the receiver's
// plain dropping of samples does.
static float s_filter_half;
static void capture(float lo_mhz, float rf_mhz, float fs, bool mirrored)
{
    double w = -2 * M_PI * (rf_mhz - lo_mhz) / fs;
    if (mirrored) w = -w;
    bool heard = fabsf(rf_mhz - lo_mhz) < (s_filter_half > 0 ? s_filter_half : fs / 2 - 1);
    for (unsigned j = 0; j < PAIRS; j++) {
        float i = 7 + 3 * noise(), q = -5 + 3 * noise();
        if (heard) i += 200 * (float)cos(w * j), q += 200 * (float)sin(w * j);
        s_iq[j] = ((uint32_t)lrintf(i) & 1023u) | (((uint32_t)lrintf(q) & 1023u) << 10);
    }
}

static int peak_col(const float *row, float *over_db)
{
    int best = 0;
    for (int c = 1; c < SCOPE_COLS; c++)
        if (row[c] > row[best]) best = c;
    // Against the strongest column more than 8 columns away.
    float other = 0;
    for (int c = 0; c < SCOPE_COLS; c++)
        if (abs(c - best) > 8 && row[c] > other) other = row[c];
    *over_db = 10 * log10f(row[best] / (other + 1e-9f));
    return best;
}

// One whole sweep of a carrier at rf_mhz; the stitched row in out.
static void sweep(const scope_sweep_t *p, float rf_mhz, float *out, bool mirrored)
{
    static float colp[SCOPE_TILES_MAX * SCOPE_COLS];
    bool have[SCOPE_TILES_MAX] = {0};
    int clip = 0, peak = 0;
    memset(colp, 0, sizeof colp);
    for (int k = 0; k < p->tiles; k++) {
        capture(p->centre[k], rf_mhz, p->fs, mirrored);
        memset(s_pow, 0, sizeof s_pow);
        int segs = scope_spectrum_add(s_pow, s_iq, PAIRS, N, s_win, s_work, fft, &clip, &peak);
        CHECK(segs == PAIRS / N, "segments %d", segs);
        int c0, c1;
        scope_sweep_tile(p, k, s_pow, 1.0f / segs, NULL, colp + k * SCOPE_COLS, &c0, &c1);
        have[k] = true;
    }
    scope_sweep_blend(p, colp, have, 0, SCOPE_COLS, out);
}

static void test_span(int centre, int span, float fs, float in, float out_mhz)
{
    scope_sweep_t p;
    scope_sweep_plan(&p, centre, span, N, fs, in, out_mhz);
    float lo = centre - span / 2.0f, col_mhz = span / (float)SCOPE_COLS;
    // A column shows its middle; a carrier can sit half a column from that, and an FFT bin either way.
    float tol = col_mhz / 2 + fs / N + 0.01f;
    // Every column is covered by the clean part of at least one tile.
    for (int c = 0; c < SCOPE_COLS; c++) {
        float w = 0;
        for (int k = 0; k < p.tiles; k++) w += scope_sweep_weight(&p, k, c);
        CHECK(w > 0.2f, "span %d at %d: column %d has weight %.2f", span, centre, c, w);
    }
    for (int k = 0; k < p.tiles; k++)
        CHECK(p.centre[k] >= 2200 && p.centre[k] <= 2700, "tile %d at %d MHz", k, p.centre[k]);
    static float row[SCOPE_COLS];
    float worst = 0;
    // Wi-Fi channels 1, 6, 11 and 13, the ISM band edges, and a fine walk
    // across the whole row (so every seam and every tile centre is crossed).
    float marks[] = {2412, 2437, 2462, 2472, 2400, 2483.5f};
    int nmarks = sizeof marks / sizeof marks[0], steps = (int)((span - 1.0f) / 0.7f);
    for (int i = 0; i < nmarks + steps; i++) {
        float f = i < nmarks ? marks[i] : lo + 0.5f + (i - nmarks) * 0.7f;
        if (f < lo + 0.5f || f > lo + span - 0.5f) continue;
        sweep(&p, f, row, false);
        float over;
        int c = peak_col(row, &over);
        float shown = lo + (c + 0.5f) * col_mhz, err = shown - f;
        if (fabsf(err) > worst) worst = fabsf(err);
        CHECK(fabsf(err) <= tol, "span %d at %d: %.2f MHz shown at %.2f (column %d)", span, centre, f, shown, c);
        CHECK(over > 20, "span %d at %d: %.2f MHz only %.1f dB over the rest of the row", span, centre, f, over);
        if (i < nmarks) printf("  span %3d centre %d: %.1f MHz -> column %3d = %.2f MHz (%+.2f)\n", span, centre, f, c, shown, err);
    }
    printf("span %d centre %d: %d tiles %d..%d, worst placement error %.2f MHz (allowed %.2f, a column is %.2f)\n", span, centre,
           p.tiles, p.centre[0], p.centre[p.tiles - 1], worst, tol, col_mhz);
    // The test can tell: a receiver with the sidebands the other way round
    // puts a channel 1 carrier somewhere else.
    sweep(&p, 2412, row, true);
    float over;
    int c = peak_col(row, &over);
    if (2412 > lo + 3 && 2412 < lo + span - 3)
        CHECK(fabsf(lo + (c + 0.5f) * col_mhz - 2412) > 1, "span %d at %d: a mirrored capture went unnoticed", span, centre);
}

// One row of a span taken as a single capture at fs; the strongest column and
// how far it stands over the row's middle value, dB.
static int single_row(int centre, int span, float fs, float rf_mhz, float *over_db)
{
    static float col[SCOPE_COLS], sorted[SCOPE_COLS];
    int clip = 0, peak = 0, best = 0;
    capture(centre, rf_mhz, fs, false);
    memset(s_pow, 0, sizeof s_pow);
    scope_spectrum_add(s_pow, s_iq, PAIRS, N, s_win, s_work, fft, &clip, &peak);
    scope_single_cols(s_pow, N, fs, span, col);
    memcpy(sorted, col, sizeof col);
    for (int i = 1; i < SCOPE_COLS; i++)  // insertion sort
        for (int j = i; j > 0 && sorted[j] < sorted[j - 1]; j--) {
            float t = sorted[j];
            sorted[j] = sorted[j - 1], sorted[j - 1] = t;
        }
    for (int c = 1; c < SCOPE_COLS; c++)
        if (col[c] > col[best]) best = c;
    *over_db = 10 * log10f(col[best] / (sorted[SCOPE_COLS / 2] + 1e-9f));
    return best;
}

// The spans taken as one capture: placement across the row, and nothing from
// outside the row folded into it.
static void test_single(int centre, int span)
{
    float fs = scope_single_fs(span), lo = centre - span / 2.0f, col_mhz = span / (float)SCOPE_COLS;
    float tol = col_mhz / 2 + fs / N + 0.01f, worst = 0, over;
    s_filter_half = 0;
    for (float f = lo + 0.3f; f < lo + span - 0.3f; f += span / 53.0f) {
        if (fabsf(f - centre) < 0.2f) continue;  // the LO notch
        int c = single_row(centre, span, fs, f, &over);
        float shown = lo + (c + 0.5f) * col_mhz, err = shown - f;
        if (fabsf(err) > worst) worst = fabsf(err);
        CHECK(fabsf(err) <= tol, "span %d at %d: %.2f MHz shown at %.2f (column %d)", span, centre, f, shown, c);
        CHECK(over > 20, "span %d at %d: %.2f MHz only %.1f dB over the row", span, centre, f, over);
    }
    printf("span %d centre %d: one capture at %.0f MS/s, worst placement error %.3f MHz (allowed %.3f)\n", span, centre, fs, worst, tol);
    if (span != 40) return;
    // The 40 MHz span has the analogue filter open past its edges, so a
    // carrier 25 MHz from the centre (channel 6 seen from channel 1) reaches
    // the converter. At the span's rate it must stay out of the row; and the
    // test can tell: at 40 MS/s it comes back 15 MHz the other side.
    s_filter_half = 38;
    for (int side = -1; side <= 1; side += 2) {
        float f = centre + side * 25.0f;
        single_row(centre, span, fs, f, &over);
        CHECK(over < 10, "span 40 at %d: a carrier at %.0f MHz folds into the row, %.1f dB over it", centre, f, over);
        int c = single_row(centre, span, 40, f, &over);
        float shown = lo + (c + 0.5f) * col_mhz;
        CHECK(over > 20 && fabsf(shown - (f - side * 40.0f)) < 0.5f, "span 40 at %d: no fold at 40 MS/s (%.2f MHz, %.1f dB)", centre, shown, over);
    }
    s_filter_half = 0;
}

int main(int argc, char **argv)
{
    float fs = argc > 1 ? (float)atof(argv[1]) : SCOPE_WIDE_FS;
    float in = argc > 2 ? (float)atof(argv[2]) : SCOPE_WIDE_IN, out = argc > 3 ? (float)atof(argv[3]) : SCOPE_WIDE_OUT;
    for (int k = 0; k < N; k++) s_win[k] = 0.5f - 0.5f * (float)cos(2 * M_PI * k / N);
    // The 100 MHz span where it opens, and both spans up and down the range.
    static const int centres[] = {2442, 2437, 2425, 2412, 2462, 2250, 2650, 2431, 2449};
    for (unsigned i = 0; i < sizeof centres / sizeof centres[0]; i++) {
        test_span(centres[i], 100, fs, in, out);
        test_span(centres[i], 60, fs, in, out);
    }
    for (unsigned i = 0; i < 5; i++)
        for (int span = 40; span >= 5; span /= 2) test_single(centres[i], span);
    CHECK(scope_single_fs(40) == 80 && scope_single_fs(20) == 40 && scope_single_fs(10) == 16 && scope_single_fs(5) == 16, "single-capture rates");
    // A sweep in progress: only the columns of the tiles captured so far.
    scope_sweep_t p;
    scope_sweep_plan(&p, 2442, 100, N, fs, in, out);
    static float colp[SCOPE_TILES_MAX * SCOPE_COLS], row[SCOPE_COLS];
    bool have[SCOPE_TILES_MAX] = {0};
    for (int i = 0; i < SCOPE_TILES_MAX * SCOPE_COLS; i++) colp[i] = 1;
    have[0] = true;
    scope_sweep_blend(&p, colp, have, 0, SCOPE_COLS, row);
    CHECK(row[20] == 1 && row[SCOPE_COLS - 1] == 0, "a part sweep: %.1f %.1f", row[20], row[SCOPE_COLS - 1]);
    printf(fails ? "%d FAILED\n" : "scope: all passed\n", fails);
    return fails != 0;
}
