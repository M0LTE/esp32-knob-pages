#include "rx_audio.h"

#include <string.h>

void rxa_init(rxa_t *a, int16_t *buf, uint32_t cap)
{
    memset(a, 0, sizeof *a);
    a->ring = buf;
    a->cap = cap;
}

void rxa_reset(rxa_t *a, uint32_t in_rate, uint32_t out_rate, uint32_t target)
{
    a->in_rate = in_rate;
    a->out_rate = out_rate;
    a->target = target < a->cap / 2 ? target : a->cap / 2;
    a->r = a->w;  // empty
    a->playing = false;
    a->frac = 0;
    a->prev = a->cur = 0;
    a->fill_ema = 0;
    a->corr = 0;
}

uint32_t rxa_fill(const rxa_t *a) { return a->w - a->r; }

void rxa_push(rxa_t *a, const int16_t *s, uint32_t n)
{
    uint32_t room = a->cap - (a->w - a->r);
    if (n > room) {
        a->overflows++;
        n = room;
    }
    for (uint32_t i = 0; i < n; i++) a->ring[(a->w + i) % a->cap] = s[i];
    __atomic_store_n(&a->w, a->w + n, __ATOMIC_RELEASE);
}

void rxa_push_le(rxa_t *a, const uint8_t *p, uint32_t n)
{
    uint32_t room = a->cap - (a->w - a->r);
    if (n > room) {
        a->overflows++;
        n = room;
    }
    uint32_t w = a->w;
    for (uint32_t i = 0; i < n; i++) a->ring[(w + i) % a->cap] = (int16_t)(p[2 * i] | p[2 * i + 1] << 8);
    __atomic_store_n(&a->w, w + n, __ATOMIC_RELEASE);
}

static inline bool take(rxa_t *a, int16_t *v)
{
    uint32_t w = __atomic_load_n(&a->w, __ATOMIC_ACQUIRE);
    if (w == a->r) return false;
    *v = a->ring[a->r % a->cap];
    __atomic_store_n(&a->r, a->r + 1, __ATOMIC_RELEASE);
    return true;
}

int rxa_pull(rxa_t *a, int16_t *out, int n)
{
    uint32_t fill = rxa_fill(a);
    if (!a->playing) {
        if (fill < a->target || !a->target) {
            memset(out, 0, sizeof *out * n);
            return 0;
        }
        a->playing = true;
        a->fill_ema = (float)fill;
        a->frac = 0;
        take(a, &a->prev);
        take(a, &a->cur);
    }
    // Far too much queued (the sender caught up after a stall): drop back to target.
    if (fill > 3 * a->target) {
        __atomic_store_n(&a->r, a->r + (fill - a->target), __ATOMIC_RELEASE);
        fill = a->target;
        a->fill_ema = (float)fill;
        a->trims++;
    }
    // Hold the fill near target: smooth it (time constant a few hundred ms) and
    // consume a touch faster when above, slower when below.
    float k = (float)n / a->out_rate / 0.25f;
    a->fill_ema += (k > 1 ? 1 : k) * ((float)fill - a->fill_ema);
    float err = (a->fill_ema - a->target) / a->target;
    float corr = 0.02f * err;
    if (corr > RXA_MAX_CORR) corr = RXA_MAX_CORR;
    if (corr < -RXA_MAX_CORR) corr = -RXA_MAX_CORR;
    a->corr = corr;
    float step = (float)a->in_rate / a->out_rate * (1 + corr);

    int i;
    for (i = 0; i < n; i++) {
        while (a->frac >= 1) {
            a->prev = a->cur;
            if (!take(a, &a->cur)) {
                // Ran dry: silence from here, and refill to target before resuming.
                a->playing = false;
                a->underruns++;
                memset(out + i, 0, sizeof *out * (n - i));
                return i;
            }
            a->frac -= 1;
        }
        out[i] = (int16_t)(a->prev + (a->cur - a->prev) * a->frac);
        a->frac += step;
    }
    return i;
}

int rxa_block(const uint8_t *msg, uint32_t len, const uint8_t **pcm)
{
    if (len < 4 || msg[0] != 0x02 || (len - 4) % 2) return -1;
    *pcm = msg + 4;
    return (int)((len - 4) / 2);
}
