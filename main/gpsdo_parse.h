// The GPSDO status document (JSON, as the gpsdo-cyd monitor publishes it on
// MQTT) into the model's gps_* fields. No ESP-IDF includes (cJSON only), so
// the host test in test/ builds it too.
#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "model.h"

// Fills the gps_* readings of m from one message (missing or odd fields: NAN
// for measurements, 0 / false / "" otherwise). Leaves gps_up, gps_seq,
// gps_msg_ms and gps_name alone. False if it is not a JSON object.
bool gpsdo_parse(const char *json, size_t len, model_t *m);
