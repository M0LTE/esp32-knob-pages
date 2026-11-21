// Updates over Wi-Fi and the checks around them: the image is streamed into
// the other app slot, and a new image only stays if it proves itself healthy
// (the bootloader rolls back otherwise).
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

extern volatile bool g_ota_active;   // an upload is being written: heavy sources pause

// At start-up: logs the running slot and state, and if this image is new and
// unconfirmed, starts the health check that confirms it or restarts.
void ota_boot_check(void);
// The web server is answering (one of the health conditions).
void ota_note_web_up(void);

// Streaming an image: begin (with the first bytes, at least the app header),
// write, then finish. Each returns NULL on success or a short reason.
const char *ota_begin(const uint8_t *first, size_t n, size_t total);
const char *ota_write(const uint8_t *p, size_t n);
const char *ota_finish(char *version, size_t vn);  // sets the boot slot; caller restarts
void ota_abort(void);

const char *ota_running_slot(void);
// Before deep sleep: a wake is a reboot, and rebooting an image still on trial
// would roll it back. True if it is confirmed (or could be confirmed now);
// false if it is on trial and not yet healthy.
bool ota_ready_for_sleep(void);
