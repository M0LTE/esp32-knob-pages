#pragma once
#include <stdbool.h>
#include <stdint.h>

// DRV2605L haptic driver with an LRA. Effects are from the TI ROM library.
#define HAPTIC_STRONG_CLICK 1
#define HAPTIC_SHARP_CLICK 4
#define HAPTIC_SHARP_TICK 24
#define HAPTIC_BUZZ 47

bool haptic_init(void);
void haptic_play(uint8_t effect);
