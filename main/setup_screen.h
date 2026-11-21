// Screens shown before the pages: joining Wi-Fi, and the set-up AP instructions.
#pragma once

void setup_screen_joining(const char *ssid);
void setup_screen_joined(const char *ip, const char *hostname);
void setup_screen_ap(const char *ssid, const char *pass, const char *ip, const char *why);
// Leaves an empty black screen active for ui_init().
void setup_screen_clear(void);
// An update in progress, over whatever is on screen; pct < 0 removes it.
void setup_screen_update(int pct, const char *line);
