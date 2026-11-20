// Home Assistant REST state parsing and Predbat schedule resampling for the
// energy source. No ESP-IDF includes and no allocation, so the host test in
// desk/test/ builds it too. Works straight on the response text (one entity's
// JSON from GET /api/states/<entity_id>).
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    uint32_t t;   // unix seconds
    float v;
} ha_point_t;

// ISO 8601 time with an offset ("2026-11-20T13:05:00+0000", "+00:00", "Z",
// optional fractional seconds) to unix seconds. 0 if it cannot be read.
uint32_t ha_parse_time(const char *s, size_t n);

// The entity's state as text (top-level "state"). False if absent.
bool ha_state_str(const char *json, char *out, size_t outn);
// The state as a number; NAN if absent, "unavailable", "unknown" or not numeric.
float ha_state_num(const char *json);
// A numeric attribute (attributes.<key>); NAN if absent or not numeric.
float ha_attr_num(const char *json, const char *key);
// attributes.results {"<ISO time>": number, ...} into pts, sorted by time.
// Returns the number of points (at most max); entries that are not numbers are skipped.
int ha_results(const char *json, ha_point_t *pts, int max);

// Carry-forward resample: out[i] = value of the last point at or before
// start + i * step, NAN before the first point.
void ha_resample(const ha_point_t *pts, int n, uint32_t start, uint32_t step, int slots, float *out);

// The first step after `now` whose value differs from `cur` (pts sorted).
// False if there is none.
bool ha_next_change(const ha_point_t *pts, int n, uint32_t now, float cur, float *val, uint32_t *at);
