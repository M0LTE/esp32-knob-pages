// KISS de-framing and AX.25 address parsing. No ESP-IDF includes, so the host
// test in desk/test/ builds it too.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "model.h"

#define KISS_MAX_FRAME 1100

typedef struct {
    uint8_t buf[KISS_MAX_FRAME];
    size_t n;
    bool in_frame, esc, overflow;
} kiss_dec_t;

typedef void (*kiss_frame_cb_t)(void *ctx, const uint8_t *frame, size_t n);

void kiss_dec_init(kiss_dec_t *d);
// Feed bytes from the stream; cb gets each complete unescaped frame, starting
// with the KISS command byte. Empty and oversized frames are dropped.
void kiss_dec_feed(kiss_dec_t *d, const uint8_t *p, size_t n, kiss_frame_cb_t cb, void *ctx);

// Turn one unescaped KISS frame into a model frame. False for anything that is
// not a data frame with a sane AX.25 address field. own_call is the station's
// call; frames from it (any SSID) are marked tx. Sets src, dst, len, subch, tx.
bool kiss_frame_decode(const uint8_t *f, size_t n, const char *own_call, kiss_frame_t *out);
