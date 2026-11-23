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
const char *ui_current_type(void);   // the page showing now, e.g. "hosts" ("scope" in the scope); any task
// For the overview page: each page in the list, how it is doing as of the
// last status check (once a second), and why ("" when all is well).
const page_t *ui_page(int index);
alert_level_t ui_page_level(int index);
const char *ui_page_why(int index);

// Platform hooks, weak no-ops by default: the device makes a sound, buzzes and
// lights the LED ring; the simulator logs.
void platform_alert(alert_level_t level, const char *page_title, const char *why);
void platform_page_changed(int index, int count);
void platform_alert_cleared(void);
// System menu (long press anywhere): sleep until a touch or a turn of the
// bezel, restart, and screen brightness 5..100 %.
void platform_sleep(void);
void platform_restart(void);
void platform_set_brightness(int pct);
int platform_get_brightness(void);  // acknowledged by a tap, or the condition went away
