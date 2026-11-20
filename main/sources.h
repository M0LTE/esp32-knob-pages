// Data sources. Each runs on its own task, reads its settings and writes
// g_model under model_lock(). They follow settings changes by themselves.
#pragma once

#include <stdint.h>

void src_ping_start(void);
void src_kiss_start(void);
void src_metrics_start(void);
void src_energy_start(void);
void src_rooms_start(void);
void src_adsb_start(void);

// Counters for the status line.
extern volatile uint32_t g_kiss_frames, g_kiss_bytes, g_metrics_ok, g_metrics_fail;
extern volatile int g_kiss_connects;
extern volatile uint32_t g_energy_ok, g_energy_fail;
extern volatile int g_energy_state;  // 0 not configured, 1 failing, 2 up
extern volatile uint32_t g_rooms_ok, g_rooms_fail;
extern volatile int g_rooms_n;
extern volatile uint32_t g_adsb_ok, g_adsb_fail, g_adsb_routes;
extern volatile int g_adsb_state;  // 0 off or no home, 1 failing, 2 up
