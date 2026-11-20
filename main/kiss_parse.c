#include "kiss_parse.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

#define FEND 0xC0
#define FESC 0xDB
#define TFEND 0xDC
#define TFESC 0xDD

void kiss_dec_init(kiss_dec_t *d) { memset(d, 0, sizeof *d); }

void kiss_dec_feed(kiss_dec_t *d, const uint8_t *p, size_t n, kiss_frame_cb_t cb, void *ctx)
{
    for (size_t i = 0; i < n; i++) {
        uint8_t c = p[i];
        if (c == FEND) {
            if (d->in_frame && d->n > 0 && !d->overflow) cb(ctx, d->buf, d->n);
            d->in_frame = true;
            d->n = 0;
            d->esc = false;
            d->overflow = false;
            continue;
        }
        if (!d->in_frame) continue;
        if (d->esc) {
            d->esc = false;
            if (c == TFEND) c = FEND;
            else if (c == TFESC) c = FESC;
            // Anything else after FESC is a protocol error; keep the byte as is.
        } else if (c == FESC) {
            d->esc = true;
            continue;
        }
        if (d->n < sizeof d->buf) d->buf[d->n++] = c;
        else d->overflow = true;
    }
}

// One 7-byte AX.25 address into "CALL" or "CALL-SSID". False if not printable.
static bool ax25_addr(const uint8_t *a, char *out, size_t outn)
{
    char call[7];
    int k = 0;
    for (int i = 0; i < 6; i++) {
        char c = (char)(a[i] >> 1);
        if (c == ' ') continue;
        if (!isalnum((unsigned char)c)) return false;
        call[k++] = c;
    }
    call[k] = 0;
    if (k == 0) return false;
    int ssid = (a[6] >> 1) & 0x0F;
    if (ssid) snprintf(out, outn, "%s-%d", call, ssid);
    else snprintf(out, outn, "%s", call);
    return true;
}

static bool same_base_call(const char *a, const char *b)
{
    while (*a && *a != '-' && *b && *b != '-') {
        if (toupper((unsigned char)*a) != toupper((unsigned char)*b)) return false;
        a++, b++;
    }
    return (!*a || *a == '-') && (!*b || *b == '-');
}

bool kiss_frame_decode(const uint8_t *f, size_t n, const char *own_call, kiss_frame_t *out)
{
    if (n < 1) return false;
    uint8_t cmd = f[0] & 0x0F, port = f[0] >> 4;
    if (cmd != 0) return false;
    const uint8_t *ax = f + 1;
    size_t len = n - 1;
    if (len < 15) return false;  // two addresses and a control byte
    memset(out->src, 0, sizeof out->src);
    memset(out->dst, 0, sizeof out->dst);
    if (!ax25_addr(ax, out->dst, sizeof out->dst)) return false;
    if (!ax25_addr(ax + 7, out->src, sizeof out->src)) return false;
    out->len = (uint16_t)len;
    out->subch = port;
    out->tx = own_call && own_call[0] && same_base_call(out->src, own_call);
    return true;
}
