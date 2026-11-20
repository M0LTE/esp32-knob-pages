// pdn-soundmodem Prometheus text: parse one scrape, then fold successive
// scrapes into the modem_* fields of the model. No ESP-IDF includes, so the
// host test in desk/test/ builds it too.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "model.h"

#define PROM_MAX_STATIONS 40

typedef struct {
    char call[12];
    char mode[24];
    int subch;                       // -1 if no pdn_station_info line
    double frames, bytes, corrected;
    double snr_sum, snr_n, snr_last;
    double off_n, off_last;
    bool has_snr_last, has_off_last;
} prom_station_t;

typedef struct {
    prom_station_t st[PROM_MAX_STATIONS];
    int n;
    double rt_ratio, behind, input_losses, tx_stalls, tx_stall_s, uncounted, busy_s, recv_s;
    int globals;                     // how many of the global metrics were seen
} prom_snap_t;

// Parses the scrape; returns the number of pdn_ samples understood.
int prom_parse(const char *text, size_t n, prom_snap_t *out);

#define MST_HIST 64     // per-station samples, enough for 10 min at one poll per 10 s
#define MG_HIST 400     // global samples, enough for 1 h at one poll per 10 s
#define MST_TRACK 48

typedef struct {
    char call[12];
    char mode[24];
    double frames;
    bool seen_before;
    uint32_t heard_ms;
    uint32_t last_seen_ms;
    uint32_t t[MST_HIST];
    double corr[MST_HIST];
    int head, n;
} mtrack_t;

typedef struct {
    mtrack_t tr[MST_TRACK];
    uint32_t gt[MG_HIST];
    double g_losses[MG_HIST], g_stalls[MG_HIST];
    int ghead, gn;
    double prev_busy, prev_recv;
    bool have_prev;
} metrics_state_t;

void metrics_state_init(metrics_state_t *st);
// Folds one good scrape into the model's modem fields (caller holds the model lock).
// Does not touch modem_name, modem_up or modem_poll_ms.
void metrics_apply(metrics_state_t *st, const prom_snap_t *snap, uint32_t now_ms, model_t *m);
