// The 2.4 GHz scope's spectrum maths, free of any device header so the host
// tests can run it (test/test_scope.c): raw capture words to a power
// spectrum, and the swept spans' tiles to one 360-column row.
#pragma once

#include <stdbool.h>
#include <stdint.h>

#define SCOPE_COLS 360
#define SCOPE_TILES_MAX 8

// The swept spans: tiles at 80 MS/s, each used from 2 to 20 MHz either side of
// its centre (clear of the LO notch, inside the analogue filter's flat part).
#define SCOPE_WIDE_FS 80
#define SCOPE_WIDE_IN 2.0f
#define SCOPE_WIDE_OUT 20.0f

// 10-bit signed I (bits 0..9) and Q (bits 10..19) of one capture word.
static inline int scope_s10(uint32_t v) { return (int)(v << 22) >> 22; }

// Window, FFT and add the power of each n-pair segment of a capture to pow
// (n floats, index 0 is -fs/2, index n/2 is the tuned centre, higher index is
// higher RF). win is n floats, work 2n floats; fft is an in-place complex FFT
// of n points (natural order out). Returns the segments added. clip counts
// samples near full scale, peak holds the largest magnitude seen.
int scope_spectrum_add(float *pow, const volatile uint32_t *iq, unsigned pairs, int n, const float *win, float *work,
                       void (*fft)(float *data, int n), int *clip, int *peak);

// The capture rate, MS/s, for a span taken as one capture (40 MHz and under).
// The receiver's 40 MS/s is its 80 MS/s stream with every other sample
// dropped and nothing filtered first, so what lies 20 to 40 MHz from the
// centre folds back in. The analogue filter only stops that when it is set
// narrow, as for the 20 MHz span; the 40 MHz span needs the filter open to
// 20 MHz either side, so it is taken at 80 MS/s and the middle half is shown.
// The 16 MS/s rate has a filter of its own in front (nothing folds, measured).
int scope_single_fs(int span);

// One row of a single-capture span: notch the LO leakage at the centre of pow
// (n bins at fs MS/s, index n/2 the tuned centre; pow is changed), then the
// strongest bin under each of the SCOPE_COLS columns across span MHz.
void scope_single_cols(float *pow, int n, float fs, int span, float *col);

// A swept span: tiles of fs MHz captured at stepped centres. Each is used only
// between `in` and `out` MHz from its own centre.
typedef struct {
    int n;        // FFT length
    float fs;     // capture rate, MS/s
    float lo;     // left edge of the row, MHz
    float span;   // width of the row, MHz
    float in, out;
    int tiles;
    int centre[SCOPE_TILES_MAX];  // MHz
} scope_sweep_t;

void scope_sweep_plan(scope_sweep_t *p, int mhz, int span, int n, float fs, float in, float out);

// The weight tile k's bins get in column col: none near its centre or past
// its clean width, ramping in and out so neighbours blend over their overlap.
float scope_sweep_weight(const scope_sweep_t *p, int k, int col);

// Tile k's part of the row. For every column it covers: colp[col] is the
// strongest bin under the column, of pow[bin] * scale * corr[bin] (corr may
// be NULL). Columns it does not cover are left alone. Returns the first and
// one past the last column covered in *c0 and *c1.
void scope_sweep_tile(const scope_sweep_t *p, int k, const float *pow, float scale, const float *corr, float *colp, int *c0,
                      int *c1);

// Blend the tiles' columns c0..c1-1 into linear power out[col]. colp is
// tiles rows of SCOPE_COLS; have[k] says tile k has been captured.
void scope_sweep_blend(const scope_sweep_t *p, const float *colp, const bool *have, int c0, int c1, float *out);
