// Pager: owns the pages, moves between them, routes input, raises alerts.
#pragma once

#include "page.h"

typedef struct {
    const char *type;
    const char *title;
} ui_page_cfg_t;

void ui_init(const ui_page_cfg_t *pages, int n);
void ui_knob(int delta);          // from the bezel, any task (queued)
void ui_goto(int index, bool animate);
int ui_page_count(void);

// Platform hooks, weak no-ops by default: the device makes a sound, buzzes and
// lights the LED ring; the simulator logs.
void platform_alert(alert_level_t level, const char *page_title, const char *why);
void platform_page_changed(int index, int count);
void platform_alert_cleared(void);  // acknowledged by a tap, or the condition went away
