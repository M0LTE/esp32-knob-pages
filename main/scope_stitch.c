// The 2.4 GHz scope's spectrum maths (see scope_stitch.h).
#include "scope_stitch.h"

#include <math.h>

// The receiver puts RF above the LO at negative frequency, so Q is negated.
int scope_spectrum_add(float *pow, const volatile uint32_t *iq, unsigned pairs, int n, const float *win, float *work,
                       void (*fft)(float *data, int n), int *clip, int *peak)
{
    // DC (LO leakage) of this capture, removed before the window.
    int32_t si = 0, sq = 0;
    for (unsigned j = 0; j < pairs; j++) {
        uint32_t w = iq[j];
        int i = scope_s10(w), q = scope_s10(w >> 10);
        si += i;
        sq += q;
        int a = i < 0 ? -i : i, b = q < 0 ? -q : q;
        if (a > *peak) *peak = a;
        if (b > *peak) *peak = b;
        if (a >= 500 || b >= 500) (*clip)++;
    }
    float di = (float)si / pairs, dq = (float)sq / pairs;
    int segs = 0;
    for (unsigned seg = 0; seg + n <= pairs; seg += n, segs++) {
        for (int k = 0; k < n; k++) {
            uint32_t w = iq[seg + k];
            work[2 * k] = (scope_s10(w) - di) * win[k];
            work[2 * k + 1] = -(scope_s10(w >> 10) - dq) * win[k];
        }
        fft(work, n);
        for (int k = 0; k < n; k++) {
            int d = (k + n / 2) & (n - 1);  // shifted: index 0 is -fs/2
            pow[d] += work[2 * k] * work[2 * k] + work[2 * k + 1] * work[2 * k + 1];
        }
    }
    return segs;
}

void scope_sweep_plan(scope_sweep_t *p, int mhz, int span, int n, float fs, float in, float out)
{
    p->n = n;
    p->fs = fs;
    p->lo = mhz - span / 2.0f;
    p->span = span;
    p->in = in;
    p->out = out;
    // First and last tiles reach 1 MHz past the edges; the rest are spread
    // evenly, never further apart than the clean width allows.
    float reach = out - 1.0f, step_max = out - in;
    int t = 1 + (int)ceilf((span - 2 * reach) / step_max);
    if (t < 1) t = 1;
    if (t > SCOPE_TILES_MAX) t = SCOPE_TILES_MAX;
    float step = t > 1 ? (span - 2 * reach) / (t - 1) : 0;
    for (int k = 0; k < t; k++) p->centre[k] = (int)lrintf(p->lo + reach + k * step);
    p->tiles = t;
}

static float col_mid(const scope_sweep_t *p, int col) { return p->lo + (col + 0.5f) * p->span / SCOPE_COLS; }

float scope_sweep_weight(const scope_sweep_t *p, int k, int col)
{
    float a = fabsf(col_mid(p, col) - p->centre[k]);
    if (a < p->in || a > p->out) return 0;
    float up = fminf(1, a - p->in), down = fminf(1, (p->out - a) / 4.0f);
    return up * down;
}

void scope_sweep_tile(const scope_sweep_t *p, int k, const float *pow, float scale, const float *corr, float *colp, int *c0,
                      int *c1)
{
    int n = p->n;
    float bin_mhz = p->fs / n, col_mhz = p->span / SCOPE_COLS;
    *c0 = SCOPE_COLS;
    *c1 = 0;
    for (int col = 0; col < SCOPE_COLS; col++) {
        if (scope_sweep_weight(p, k, col) <= 0) continue;
        float f0 = p->lo + col * col_mhz, f1 = f0 + col_mhz;
        // Bin b is centred on (b - n/2) bins from the tile's centre.
        int b0 = n / 2 + (int)floorf((f0 - p->centre[k]) / bin_mhz + 0.5f);
        int b1 = n / 2 + (int)floorf((f1 - p->centre[k]) / bin_mhz + 0.5f);
        if (b1 <= b0) b1 = b0 + 1;
        if (b0 < 0 || b1 > n) continue;
        float m = 0;
        for (int b = b0; b < b1; b++) {
            float x = pow[b] * scale * (corr ? corr[b] : 1.0f);
            if (x > m) m = x;
        }
        colp[col] = m;
        if (col < *c0) *c0 = col;
        if (col + 1 > *c1) *c1 = col + 1;
    }
}

void scope_sweep_blend(const scope_sweep_t *p, const float *colp, const bool *have, int c0, int c1, float *out)
{
    for (int col = c0; col < c1; col++) {
        float acc = 0, wsum = 0;
        for (int k = 0; k < p->tiles; k++) {
            if (!have[k]) continue;
            float w = scope_sweep_weight(p, k, col);
            if (w <= 0) continue;
            acc += w * colp[k * SCOPE_COLS + col];
            wsum += w;
        }
        out[col] = wsum > 0 ? acc / wsum : 0;
    }
}
