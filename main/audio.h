#pragma once

#include <stdbool.h>
#include <stdint.h>

void audio_init(void);
// Queue a two-tone ping with an exponential decay. vol is 0..1. While a
// stream plays, chimes mix over it and duck it.
void audio_play(float f1, float f2, float ms, float vol);

// Received audio (one stream at a time). start begins buffering at in_rate
// (it fades in once ~300 ms are queued); push adds s16 little-endian mono
// samples; stop fades out and goes quiet (the amp is turned off when nothing
// plays). volume is 0..100 on a dB scale, 0 = mute.
void audio_stream_start(uint32_t in_rate);
void audio_stream_push(const uint8_t *le, uint32_t n_samples);
void audio_stream_stop(void);
void audio_stream_volume(int v);
bool audio_stream_playing(void);
float audio_stream_dbfs(void);          // recent input level, NAN if no stream
void audio_stream_stats(uint32_t *fill_ms, uint32_t *underruns, uint32_t *trims, float *corr);
