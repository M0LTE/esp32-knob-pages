// 2.4 GHz scope: the knob's own Wi-Fi radio as a receive-only spectrum
// analyser (see scope.c and NOTICE). It runs in a boot of its own: entering
// restarts the knob into a scope boot without Wi-Fi, the web server or any
// source; leaving restarts it back to the normal pages.
#pragma once

#include <stdbool.h>

// First thing in app_main. Decides whether this is a scope boot; in a normal
// boot it gives the capture memory back to the heap.
void scope_boot_check(void);
bool scope_boot(void);
// The previous boot was a scope boot, or left for one: start quickly.
bool scope_fast_boot(void);

// Normal boot: restart into the scope (the UI asked through g_model.sc_want).
void scope_enter(void);
// Test hook (admin): scope with a fixed centre and span, back after secs.
void scope_test(int secs, int mhz, int span);

// Scope boot: bring up the radio and the capture task, then watch for the
// UI leaving (or the test time running out) and restart. Never returns.
void scope_run(void);
