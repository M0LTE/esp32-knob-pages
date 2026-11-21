// Host tests for gpsdo_parse.c.
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "gpsdo_parse.h"

static model_t m;
static int fails, checks;
#define CHECK(cond, ...)                                    \
    do {                                                    \
        checks++;                                           \
        if (!(cond)) {                                      \
            fails++;                                        \
            printf("FAIL %s:%d: ", __FILE__, __LINE__);     \
            printf(__VA_ARGS__);                            \
            printf("\n");                                   \
        }                                                   \
    } while (0)

// A status message in the shape the monitor publishes (values made up).
static const char *SAMPLE =
    "{\"state\":\"LOCKED\",\"reference_ok\":true,\"locked\":true,\"locked_s\":452870,\"lock_losses\":2,"
    "\"gpsdo_restarts\":0,\"gps_restarts\":0,\"tic_ns\":-8,\"dac\":30280,\"dac_near_limit\":false,\"ocxo_c\":44.2,"
    "\"diff_ns\":-8,\"tic_error_ns\":-9.0,\"tc_s\":300,\"ambient_c\":34.2,\"gpsdo_age_s\":0,\"firmware\":\"x\","
    "\"lock_counter\":454370,\"lock_target\":1500,\"missed_pps_total\":3,\"since_missed_pps_s\":456112,"
    "\"freq_error_ppb_30min\":-0.0145,\"freq_error_ppb_5min\":-0.05,\"pps_input\":true,\"fix\":\"3D\",\"sats_used\":10,"
    "\"sats_in_view\":21,\"hdop\":0.84,\"gps_age_s\":0,\"alarms\":[]}";

int main(void)
{
    CHECK(gpsdo_parse(SAMPLE, strlen(SAMPLE), &m), "sample");
    CHECK(!strcmp(m.gps_state, "LOCKED") && m.gps_locked && m.gps_ref_ok && m.gps_pps && !m.gps_dac_near_limit, "flags");
    CHECK(m.gps_locked_s == 452870 && m.gps_lock_losses == 2 && m.gps_missed_pps == 3 && m.gps_since_missed_s == 456112,
          "counters");
    CHECK(m.gps_lock_counter == 454370 && m.gps_lock_target == 1500 && m.gps_dac == 30280, "lock, dac");
    CHECK(m.gps_tic_ns == -8 && fabsf(m.gps_ocxo_c - 44.2f) < 1e-4f && fabsf(m.gps_ambient_c - 34.2f) < 1e-4f, "temps");
    CHECK(fabsf(m.gps_ppb_5m + 0.05f) < 1e-6f && fabsf(m.gps_ppb_30m + 0.0145f) < 1e-6f, "ppb");
    CHECK(!strcmp(m.gps_fix, "3D") && m.gps_sats_used == 10 && m.gps_sats_view == 21 && fabsf(m.gps_hdop - 0.84f) < 1e-4f,
          "gps");
    CHECK(m.gps_alarms[0] == 0, "no alarms");

    // Missing and odd fields.
    const char *odd = "{\"state\":5,\"locked\":\"yes\",\"tic_ns\":null,\"dac\":-12.7,\"locked_s\":-4,\"sats_used\":\"10\","
                      "\"fix\":\"NONE\",\"alarms\":[\"NO_PPS\",3,\"\",\"HOLDOVER\",null,\"DAC_LIMIT\"]}";
    CHECK(gpsdo_parse(odd, strlen(odd), &m), "odd");
    CHECK(m.gps_state[0] == 0 && !m.gps_locked && isnan(m.gps_tic_ns) && isnan(m.gps_ocxo_c) && isnan(m.gps_hdop),
          "missing are NAN / empty");
    CHECK(m.gps_dac == -12 && m.gps_locked_s == 0 && m.gps_sats_used == 0, "odd numbers %d %u %d", (int)m.gps_dac,
          (unsigned)m.gps_locked_s, m.gps_sats_used);
    CHECK(!strcmp(m.gps_alarms, "NO_PPS,HOLDOVER,DAC_LIMIT"), "alarms '%s'", m.gps_alarms);
    // Alarms that do not all fit.
    const char *many = "{\"alarms\":[\"AAAAAAAAAAAAAAAAAAAA\",\"BBBBBBBBBBBBBBBBBBBB\",\"CCCCCCCCCCCCCCCCCCCC\","
                       "\"DDDDDDDDDDDDDDDDDDDD\"]}";
    gpsdo_parse(many, strlen(many), &m);
    CHECK(strlen(m.gps_alarms) < sizeof m.gps_alarms && !strcmp(m.gps_alarms + strlen(m.gps_alarms) - 20, "CCCCCCCCCCCCCCCCCCCC"),
          "truncated at a whole name '%s'", m.gps_alarms);
    CHECK(!gpsdo_parse("[1,2]", 5, &m) && !gpsdo_parse("nope", 4, &m), "not an object");
    // Length-limited: only the first len bytes count.
    CHECK(gpsdo_parse("{\"fix\":\"2D\"}garbage", 12, &m) && !strcmp(m.gps_fix, "2D"), "length");
    printf("gpsdo: %d checks, %d failed\n", checks, fails);
    return fails ? 1 : 0;
}
