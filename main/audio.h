#pragma once

void audio_init(void);
// Queue a two-tone ping with an exponential decay. vol is 0..1.
void audio_play(float f1, float f2, float ms, float vol);
