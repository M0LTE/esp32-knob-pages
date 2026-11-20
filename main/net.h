// Wi-Fi: station from settings, or the set-up access point if there are no
// credentials or the station cannot join at boot. SNTP and local time.
#pragma once

#include <stdbool.h>
#include "settings.h"

#define SETUP_AP_SSID "DESK-KNOB-SETUP"
#define SETUP_AP_IP "192.168.4.1"

typedef enum { NET_STA, NET_AP } net_mode_t;

// Blocks for up to join_ms waiting for the station to get an address.
net_mode_t net_start(const settings_t *s, int join_ms);
bool net_up(void);              // station has an address
const char *net_ip(void);       // station address, "" if none
int net_rssi(void);
net_mode_t net_mode(void);
