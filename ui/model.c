#include "model.h"

#include <string.h>

model_t g_model;

void model_host_sample(host_t *h, float rtt_ms, uint32_t now_ms)
{
    if (h->hist_n < HOST_HIST) {
        h->hist[h->hist_n++] = rtt_ms;
    } else {
        memmove(h->hist, h->hist + 1, sizeof(float) * (HOST_HIST - 1));
        h->hist[HOST_HIST - 1] = rtt_ms;
    }
    int lost = 0;
    for (int i = 0; i < h->hist_n; i++) lost += h->hist[i] < 0;
    h->loss_pct = (uint8_t)(lost * 100 / h->hist_n);

    // Down after three consecutive losses, up on the first reply.
    host_state_t s = h->state;
    if (rtt_ms >= 0) {
        h->rtt_ms = rtt_ms;
        s = HOST_UP;
    } else if (h->hist_n >= 3 && h->hist[h->hist_n - 1] < 0 && h->hist[h->hist_n - 2] < 0 &&
               h->hist[h->hist_n - 3] < 0) {
        s = HOST_DOWN;
    }
    if (s != h->state) {
        h->state = s;
        h->changed_ms = now_ms;
    }
    h->sample_ms = now_ms;
}

void model_kiss_push(const kiss_frame_t *f)
{
    g_model.frames[g_model.frames_head] = *f;
    g_model.frames_head = (g_model.frames_head + 1) % KISS_FRAMES;
    if (g_model.frames_n < KISS_FRAMES) g_model.frames_n++;
    g_model.frames_seq++;
}

const kiss_frame_t *model_kiss_get(int age)
{
    if (age < 0 || age >= g_model.frames_n) return NULL;
    int i = (g_model.frames_head - 1 - age + KISS_FRAMES) % KISS_FRAMES;
    return &g_model.frames[i];
}
