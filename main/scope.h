// 2.4 GHz scope: the knob's own Wi-Fi radio as a receive-only spectrum
// analyser (see scope.c and NOTICE). It runs in a boot of its own: entering
// restarts the knob into a scope boot without Wi-Fi, the web server or any
// source; leaving restarts it back to the normal pages.
#pragma once

#include <stdbool.h>
#include <stdint.h>

// First thing in app_main. Decides whether this is a scope boot; in a normal
// boot it gives the capture memory back to the heap.
void scope_boot_check(void);
bool scope_boot(void);
// The previous boot was a scope boot, or left for one: start quickly.
bool scope_fast_boot(void);

// Normal boot: restart into the scope (the UI asked through g_model.sc_want).
void scope_enter(void);
// Test hook (admin): scope with a fixed centre and span, back after secs;
// usb_off runs it with the USB port switched off, as the menu's TURN USB OFF.
void scope_test(int secs, int mhz, int span, bool usb_off);
// What the last test run measured, kept across the restart back (not across
// power-off): each column's mean and lowest level over the run, 0.4 dB a step.
typedef struct {
    uint32_t magic;
    uint16_t mhz, span, rows;
    uint8_t usb_off, pad;
    uint8_t avg[360], min[360];
} scope_result_t;
const scope_result_t *scope_test_result(void);  // NULL if there is none

#ifdef SCOPE_FAULT_TEST
// Bench check only (-DSCOPE_FAULT_TEST=1): the next test run ends in a crash
// (1) or a hang for the watchdog (2) instead of the restart back, to prove
// that a scope boot with USB off still comes back to the pages with USB on.
void scope_test_fault(int kind);
uint32_t scope_usb_debug(void);  // the port's CONF0 (low 16 bits) and the PLL's USB clock enable (bit 16)
#endif

// USB back on if the scope switched it off (before any restart).
void scope_usb_restore(void);

// Scope boot: bring up the radio and the capture task, then watch for the
// UI leaving (or the test time running out) and restart. Never returns.
void scope_run(void);
