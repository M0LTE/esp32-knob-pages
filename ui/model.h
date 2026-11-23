// Live data the pages draw from. Sources (ping, KISS tap, ...) write it on
// their own tasks under model_lock(); pages read it under the same lock.
// Kept free of ESP-IDF types so the simulator can fill it with fake data.
#pragma once

#include <stdbool.h>
#include <stdint.h>

#define MODEL_MAX_HOSTS 16
#define HOST_HIST 60        // one sample per ping round
#define KISS_FRAMES 96

typedef enum { HOST_UNKNOWN = 0, HOST_UP, HOST_DOWN } host_state_t;

typedef struct {
    char name[20];
    char addr[48];
    host_state_t state;
    float rtt_ms;            // last good round trip
    float hist[HOST_HIST];   // round trips, oldest first; <0 = lost
    uint8_t hist_n;
    uint8_t loss_pct;        // over the history window
    uint32_t changed_ms;     // when state last changed
    uint32_t sample_ms;      // when the last sample landed
} host_t;

typedef struct {
    uint32_t t_ms;           // model clock when heard
    uint32_t wall;           // unix seconds, 0 if not known
    uint8_t tx;              // 1 = sent by the station, 0 = heard
    uint8_t subch;
    uint16_t len;
    char src[10];
    char dst[10];
} kiss_frame_t;

#define MODEL_MAX_MSTATIONS 24
#define ENERGY_SLOTS 96      // 24 h of 15-minute slots
#define MODEL_MAX_ROOMS 8
#define MODEL_MAX_AC 48
#define WF_W 360             // waterfall row width, one byte per screen column
#define WF_ROWS 64           // rows queued for the page (it keeps its own image)
#define WF_MAX_BANDS 8
#define WF_TAGS 24

// A modem's band on the waterfall, from the station page's "config" message.
typedef struct {
    float lo_hz, hi_hz;      // audio Hz
    uint8_t sub;
    char name[16];           // modeName, e.g. "BPSK300 IL2Pc"
} wf_band_t;

// A decoded frame, placed on the waterfall rows that carried its burst.
typedef struct {
    uint32_t row;            // waterfall row (wf_row_seq numbering) where the burst started
    uint16_t rows;           // how many rows it spans (at least 1)
    uint8_t sub;
    uint8_t tx;              // our own transmission
    float snr_db;            // NAN if none
    char from[10], to[10];
} wf_tag_t;
#define AC_TRAIL 10

// One aircraft from the readsb re-api JSON (adsb.oarc.uk, box round home).
typedef struct {
    char hex[8];             // ICAO 24-bit address, lower case
    char flight[10];         // callsign, trimmed, "" if none
    char reg[12];            // "r"
    char type[6];            // "t", e.g. "A20N"
    char desc[32];           // "desc", e.g. "AIRBUS A-320neo"
    char op[32];             // "ownOp"
    char cat[4];             // "category", e.g. "A3"; "" if none
    float lat, lon;          // NAN if no position
    float alt_ft;            // alt_baro; NAN if unknown; 0 on the ground
    float gs_kt, track, rate_fpm;  // NAN if unknown
    uint32_t seen_ms;        // model clock of the poll that last carried it
    // Route, from api.adsbdb.com by callsign (cached on the device); "" if unknown.
    char orig[5], dest[5];   // ICAO airport codes
    char orig_city[20], dest_city[20];
    // Recent positions, oldest first, one per poll that moved it.
    float trail_lat[AC_TRAIL], trail_lon[AC_TRAIL];
    uint8_t trail_n;
} aircraft_t;

// One room as the climate controller reports it (GET /api/state, "decisions").
typedef struct {
    char id[16];             // controller zone_id, e.g. "living"
    char name[20];           // display name from settings, e.g. "Living room"
    char status[12];         // brain status: "heating", "cooling", "holding", ...
    float target;            // the brain's current target, C
    float temp;              // room temperature (brain "pv"), C
    float humidity;          // %RH, NAN if none
    char ac_mode[10];        // A/C now: "off", "cool", "heat", "fan_only", "" if none
    float ac_setpoint;       // NAN if off or none
    int8_t valves;           // radiator valves: -1 none, 0 shut, 1 open
    bool heat_demand;        // brain wants heat in this room
    char why[72];            // first line of the brain's reasoning
    float t_min, t_max, t_step;  // limits of the target helper being written (from HA)
} room_t;

// One station as pdn-soundmodem's Prometheus metrics describe it.
typedef struct {
    char call[12];
    char mode[24];           // e.g. "bpsk300-il2pc"
    uint8_t subch;
    uint32_t frames;         // pdn_station_frames_total
    uint32_t bytes;
    uint32_t corrected;      // pdn_station_corrected_bytes_total
    float snr_last;          // dB, NAN if none
    float snr_mean;          // dB over its lifetime, NAN if none
    float offset_last;       // Hz, NAN if none
    uint32_t heard_ms;       // model clock when frames last went up; 0 = not since we started
    uint32_t corrected_10m;  // repaired bytes in the last ten minutes
} mstation_t;

typedef struct {
    host_t hosts[MODEL_MAX_HOSTS];
    int n_hosts;

    char kiss_name[20];      // e.g. "GB7XYZ"
    bool kiss_up;
    uint32_t kiss_since_ms;
    kiss_frame_t frames[KISS_FRAMES];  // ring buffer
    int frames_head;         // next write index
    int frames_n;
    uint32_t frames_seq;     // total ever seen, for "new frame" detection

    // pdn-soundmodem metrics (Prometheus), polled.
    char modem_name[20];
    bool modem_up;           // last poll succeeded
    uint32_t modem_poll_ms;  // model clock of the last good poll
    mstation_t mstations[MODEL_MAX_MSTATIONS];
    int n_mstations;
    float rt_ratio;          // pdn_receive_realtime_ratio
    float busy_pct;          // receive loop busy share over the last poll interval, 0..100
    bool behind;             // pdn_receive_behind
    uint32_t input_losses;   // pdn_receive_input_losses_total
    uint32_t input_losses_1h;
    uint32_t tx_stalls;      // pdn_transmit_stalls_total
    float tx_stall_s;
    uint32_t tx_stalls_1h;   // stalls in the last hour, from successive polls
    uint32_t uncounted;      // pdn_frames_uncounted_total

    // Home energy, from Home Assistant (Solax inverter, Octopus, Predbat), polled.
    bool energy_up;          // last poll succeeded
    uint32_t energy_poll_ms;
    float pv_w;              // solar generation, W
    float load_w;            // house consumption, W
    float grid_w;            // + importing, - exporting, W
    float batt_w;            // + charging, - discharging, W
    float soc_pct;           // battery state of charge, %
    float soc_kwh, soc_max_kwh;
    float rate_p;            // import rate now, p/kWh
    float rate_next_p;       // next different import rate, p/kWh (NAN if unknown)
    uint32_t rate_next_wall; // unix seconds when it starts (0 if unknown)
    float export_rate_p;
    char plan_status[24];    // Predbat's state, e.g. "Demand", "Charging"
    float pv_today_kwh, import_today_kwh, cost_today_p;
    // Predbat's plan for the next 24 hours from plan_start_wall, in 15-minute
    // slots: planned battery kWh (NAN if none) and import rate in p/kWh.
    uint32_t plan_start_wall;
    float plan_soc_kwh[ENERGY_SLOTS];
    float plan_rate_p[ENERGY_SLOTS];

    // Rooms, from the climate controller, polled.
    bool climate_up;
    uint32_t climate_poll_ms;
    room_t rooms[MODEL_MAX_ROOMS];
    int n_rooms;
    // A target change asked for on the knob. The page sets want; the device
    // writes the room's active target helper in Home Assistant and sets result.
    struct {
        uint32_t seq;        // bumped by the page for each new request
        char room[16];       // zone id
        float value;
        uint32_t done_seq;   // set by the device when it has handled seq
        int8_t result;       // 1 written, -1 failed (valid when done_seq == seq)
    } room_set;

    // Aircraft near home, from the readsb API, polled.
    bool adsb_up;
    uint32_t adsb_poll_ms;
    float home_lat, home_lon;   // NAN until set on the settings page
    float adsb_radius_km;       // the area polled and drawn
    aircraft_t ac[MODEL_MAX_AC];
    int n_ac;

    // Live waterfall from a pdn-soundmodem station page WebSocket. Each row is one
    // screen width of the span wf_lo_hz..wf_hi_hz, a byte per column on the
    // station's scale (dB = -100 + byte * 100 / 255), the maximum over the
    // spectrum lines folded into it. Row k lives at wf_rows[k % WF_ROWS].
    bool wf_up;
    char wf_name[20];
    double wf_dial_hz;          // radio dial, 0 if unknown
    bool wf_lsb;                // RF = dial - audio when true
    float wf_lo_hz, wf_hi_hz;   // audio span the rows cover
    float wf_rows_per_s;
    wf_band_t wf_bands[WF_MAX_BANDS];
    int wf_n_bands;
    uint32_t wf_row_seq;        // rows produced so far
    uint8_t wf_rows[WF_ROWS][WF_W];
    uint8_t wf_row_tx[WF_ROWS]; // 1 if our own transmission is in that row
    uint8_t wf_spec[WF_W];      // the latest spectrum line, same scale, for the trace
    wf_tag_t wf_tags[WF_TAGS];  // ring, newest at (wf_tag_seq - 1) % WF_TAGS
    uint32_t wf_tag_seq;
    // Listening: the page sets wf_listen and wf_volume; the device asks the
    // station for its receive audio while wf_listen is true, plays it, and
    // reports back.
    bool wf_listen;
    uint8_t wf_volume;          // 0..100
    bool wf_audio_on;           // audio is arriving and playing
    float wf_audio_dbfs;        // recent audio level, for a meter; NAN if none

    // GPS-disciplined oscillator, from a JSON status message on MQTT
    // (the gpsdo-cyd "state" document). gps_seq counts messages received.
    bool gps_up;                // MQTT connected and a message seen recently
    uint32_t gps_seq;
    uint32_t gps_msg_ms;        // model clock of the latest message
    char gps_name[20];
    char gps_state[16];         // e.g. "LOCKED", "HOLDOVER", "WARMUP"
    bool gps_locked, gps_ref_ok, gps_pps, gps_dac_near_limit;
    uint32_t gps_locked_s, gps_lock_losses, gps_missed_pps, gps_since_missed_s;
    uint32_t gps_lock_counter, gps_lock_target;
    float gps_tic_ns;           // phase error, ns (NAN if unknown)
    float gps_ocxo_c, gps_ambient_c;
    float gps_ppb_5m, gps_ppb_30m;
    int32_t gps_dac;
    char gps_fix[8];            // "3D", "2D", "NONE"
    int gps_sats_used, gps_sats_view;
    float gps_hdop;
    char gps_alarms[64];        // alarm names joined by commas, "" if none

    // Doorbell: rings from a Home Assistant binary sensor, and camera frames
    // fetched as JPEG and decoded by the device while door_want_video is set.
    // The device keeps the newest decoded frame at door_frame (door_w x
    // door_h, RGB565 in the display's own byte order) and bumps
    // door_frame_seq; the page copies it under the lock.
    bool door_up;               // configured and Home Assistant answering
    char door_name[20];         // e.g. "FRONT DOOR"
    uint32_t door_ring_seq;     // rings seen since start
    uint32_t door_ring_wall;    // unix time of the latest ring, 0 if unknown
    bool door_want_video;       // set by the page while it is showing
    uint16_t door_w, door_h;
    uint16_t *door_frame;       // NULL until a frame has been decoded
    uint32_t door_frame_seq;
    float door_fps;

    // 2.4 GHz scope: the knob's own radio as a receiver. While sc_active the
    // device has Wi-Fi off and every network source stopped, and fills rows
    // like the waterfall's: one byte per screen column over the span, on a
    // relative scale (higher = stronger; the page auto-levels).
    bool sc_want;               // UI: enter (true) or leave (false) scope mode
    bool sc_active;             // device: capturing now
    bool sc_usb_off;            // UI: USB port switched off for this scope session (its 48 MHz clock is heard on 2448 and 2352 MHz)
    float sc_center_mhz_want, sc_span_mhz_want;   // UI asks
    float sc_center_mhz, sc_span_mhz;             // device tuned
    float sc_rows_per_s;
    uint32_t sc_row_seq;
    uint8_t sc_rows[WF_ROWS][WF_W];
    uint8_t sc_spec[WF_W];
    char sc_status[32];         // e.g. "capturing", "Wi-Fi stopping", an error
} model_t;

extern model_t g_model;
void model_lock(void);
void model_unlock(void);

// Helpers that keep the history arithmetic in one place (callers hold the lock).
void model_host_sample(host_t *h, float rtt_ms, uint32_t now_ms);  // rtt < 0 = lost
void model_kiss_push(const kiss_frame_t *f);
const kiss_frame_t *model_kiss_get(int age);  // 0 = newest
