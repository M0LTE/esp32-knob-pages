// ADS-B: readsb re-api JSON (adsb.oarc.uk) into aircraft, merging polls into
// the tracked set with trails, and adsbdb route answers. No ESP-IDF includes
// (cJSON only), so the host test in desk/test/ builds it too.
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "model.h"

#define ADSB_FORGET_MS 60000   // drop aircraft not reported for this long

// The query box round a point: south, north, west, east in degrees.
void adsb_box(float lat, float lon, float radius_km, float box[4]);
// Distance between two points, km (equirectangular; fine at these ranges).
float adsb_dist_km(float lat1, float lon1, float lat2, float lon2);

// Parses one re-api answer ({"ac":[...]}) into out (at most max), every
// aircraft that has a position, with seen_ms = now_ms and no trail or route.
// Returns the count, or -1 if the JSON is unreadable.
int adsb_parse(const char *json, aircraft_t *out, int max, uint32_t now_ms);

// Folds fresh (one poll) into cur (the tracked set, *n_cur entries, room for
// MODEL_MAX_AC): keeps trails and routes by hex, appends a trail point when an
// aircraft moved, forgets ones unseen for ADSB_FORGET_MS, and leaves cur
// sorted nearest to home first, at most MODEL_MAX_AC. scratch needs room for
// MODEL_MAX_AC + n_fresh aircraft.
void adsb_merge(aircraft_t *cur, int *n_cur, const aircraft_t *fresh, int n_fresh, float home_lat, float home_lon,
                uint32_t now_ms, aircraft_t *scratch);

typedef struct {
    char orig[5], dest[5];
    char orig_city[20], dest_city[20];
} adsb_route_t;

// An adsbdb /v0/callsign answer: 1 = route found (filled), 0 = no route
// (404 or "unknown callsign"), -1 = could not tell (other status, bad JSON).
int adsbdb_parse(const char *json, int http_status, adsb_route_t *r);

// Callsign as adsbdb wants it: trimmed, upper case, letters and digits only.
// False if nothing usable is left.
bool adsb_callsign_key(const char *flight, char *out, int n);
