// Device settings: one JSON document in NVS, defaults compiled in.
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "rooms_parse.h"

#define SET_MAX_PAGES 8
#define SET_MAX_HOSTS 16

typedef struct {
    char type[12];
    char title[24];
} set_page_t;

typedef struct {
    char name[20];
    char addr[48];
} set_host_t;

// Home Assistant entities the energy source reads, in this order.
enum {
    EN_PV, EN_LOAD, EN_GRID_IN, EN_GRID_OUT, EN_BATT, EN_SOC_PCT, EN_PLAN, EN_RATES, EN_RATES_EXPORT,
    EN_STATUS, EN_PV_TODAY, EN_IMPORT_TODAY, EN_COST_TODAY, EN_COUNT
};
extern const char *const energy_keys[EN_COUNT];  // names used in settings, e.g. "pv_w"

typedef struct {
    char ssid[33];
    char pass[65];
    char hostname[32];
    char admin_pass[33];

    set_page_t pages[SET_MAX_PAGES];
    int n_pages;
    set_host_t hosts[SET_MAX_HOSTS];
    int n_hosts;

    char kiss_host[48];
    int kiss_port;
    char kiss_name[20];

    char metrics_url[128];
    char metrics_name[20];

    char energy_url[96];
    char energy_token[300];  // secret: never shown or logged
    int energy_poll_s;
    char energy_ent[EN_COUNT][64];

    char brain_url[96];          // climate controller (rooms)
    room_cfg_t rooms[MODEL_MAX_ROOMS];
    int n_rooms;

    bool adsb_enabled;
    float home_lat, home_lon;    // NAN until set; never shown in settings.json
    float adsb_radius_km;

    bool sound;
    int volume;              // 0..100
    bool quiet;              // quiet hours enabled: no sound between quiet_from and quiet_to
    char quiet_from[6];      // "HH:MM" local time
    char quiet_to[6];
} settings_t;

void settings_init(void);                 // NVS up, load or fall back to defaults
void settings_get(settings_t *out);       // copy of the current settings
bool settings_save(const settings_t *s);  // validate-free store; bumps the generation
uint32_t settings_gen(void);              // changes whenever settings are saved
void settings_defaults(settings_t *s);
char *settings_to_json(const settings_t *s, bool with_secrets);  // malloc'd
bool settings_from_json(const char *json, settings_t *s);        // fills over what is there
const char *settings_ap_pass(void);       // set-up AP password, made once per device

// True if sound is allowed now (sound on and outside quiet hours).
bool settings_sound_allowed(const settings_t *s);
// The same for the current settings, without copying them (they are big):
// returns the volume 0..100 if sound is allowed now, else 0. any_time ignores
// the sound switch and quiet hours (for the test button).
int settings_alert_volume(bool any_time);
