// Received audio between the network and the DAC: a single-producer,
// single-consumer ring, a linear-interpolating resampler from the stream's
// rate to the DAC's, and a fill controller that holds the ring near a target
// by nudging the resampling ratio a little (never more than RXA_MAX_CORR).
// On underrun it plays silence and refills to the target before resuming.
// No ESP-IDF includes, so the host test in test/ builds it too.
#pragma once

#include <stdbool.h>
#include <stdint.h>

#define RXA_MAX_CORR 0.005f   // at most 0.5 % faster or slower

typedef struct {
    int16_t *ring;
    uint32_t cap;
    volatile uint32_t w, r;   // free-running counts; producer owns w, consumer owns r
    // consumer side
    uint32_t in_rate, out_rate, target;
    bool playing;
    float frac;               // position between prev and cur, 0..1
    int16_t prev, cur;
    float fill_ema, corr;
    uint32_t underruns, trims, overflows;
} rxa_t;

void rxa_init(rxa_t *a, int16_t *buf, uint32_t cap);
// Start a stream (consumer side, or before either side runs). target is in
// input samples.
void rxa_reset(rxa_t *a, uint32_t in_rate, uint32_t out_rate, uint32_t target);
// Producer: add samples. If the ring is full the newest are dropped.
void rxa_push(rxa_t *a, const int16_t *s, uint32_t n);
// Producer: samples from a little-endian byte stream (the network form).
void rxa_push_le(rxa_t *a, const uint8_t *p, uint32_t n_samples);
uint32_t rxa_fill(const rxa_t *a);
// Consumer: n output samples at out_rate. Returns how many were real audio
// (the rest are silence: buffering or underrun).
int rxa_pull(rxa_t *a, int16_t *out, int n);
// Parses a station page audio block: [0x02][3 bytes padding][s16 LE ...].
// Returns the sample count and points *pcm at the samples, or -1 if it is not one.
int rxa_block(const uint8_t *msg, uint32_t len, const uint8_t **pcm);
