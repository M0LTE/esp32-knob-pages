// A page is a self-contained module: it builds its own LVGL objects inside the
// root it is given, draws however it likes, and reacts to input. Adding a page
// type means writing one of these and listing it in pages.c.
#pragma once

#include <stdbool.h>
#include "lvgl.h"

// NOTICE is a passing event worth a look (an aircraft about to pass over):
// a soft chime and a jump to the page, cleared on its own when it is over.
typedef enum { ALERT_NONE = 0, ALERT_NOTICE, ALERT_WARN, ALERT_CRIT } alert_level_t;

typedef struct page page_t;

typedef struct {
    const char *type;      // stable id used in settings, e.g. "hosts"
    const char *label;     // shown in the page header
    void (*create)(page_t *p, lv_obj_t *root);
    void (*destroy)(page_t *p);
    // Called every frame while visible (and at a slower rate while hidden, so
    // pages can keep their own state current). dt in ms.
    void (*tick)(page_t *p, uint32_t now_ms, uint32_t dt_ms, bool visible);
    void (*tap)(page_t *p, int x, int y);
    // Optional: the bezel turned while this page is showing. Return true if the
    // page used it (e.g. while editing a value); false lets it change page.
    bool (*knob)(page_t *p, int delta);
    // What the page thinks of the world right now, for alerts and the overview.
    alert_level_t (*status)(page_t *p, char *why, int why_len);
} page_type_t;

struct page {
    const page_type_t *type;
    char title[24];
    lv_obj_t *root;
    void *state;           // page-private
};

const page_type_t *page_type_find(const char *type);
