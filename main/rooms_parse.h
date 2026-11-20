// Rooms: the settings lines that name them, which target helper is in force
// at a given time, and a climate controller's /api/state (per-zone
// "decisions", see test/brain_state_sample.json). No ESP-IDF includes (cJSON
// only), so the host test in desk/test/ builds it too.
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "model.h"

typedef struct {
    char id[16];          // the controller's zone_id
    char name[20];        // shown on the page
    char helper[64];      // HA input_number the knob writes
    char night[64];       // optional helper used inside the night window
    int16_t night_from;   // minutes after local midnight, -1 = no night helper
    int16_t night_to;
} room_cfg_t;

// One room per line: zone_id, display name (underscores shown as spaces),
// helper entity, then optionally a night helper and HH:MM-HH:MM. Returns the
// number of rooms read; bad lines are skipped.
int rooms_cfg_parse(const char *text, room_cfg_t *out, int max);
// Back to one line (as rooms_cfg_parse reads it), for the settings page.
void rooms_cfg_line(const room_cfg_t *r, char *out, int n);

// The helper in force at minute_of_day (0..1439, local). The window runs from
// night_from up to but not including night_to, and may cross midnight.
const char *rooms_helper_at(const room_cfg_t *r, int minute_of_day);

// Fills out[0..n-1] (in configured order) from the controller's /api/state.
// Zones it does not report keep their id and name with NAN readings and
// an empty status. Leaves t_min/t_max/t_step alone. Returns the number of
// configured rooms it reported, or -1 if the JSON is unreadable.
int rooms_parse_state(const char *json, const room_cfg_t *cfg, int n, room_t *out);
