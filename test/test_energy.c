// Host tests for energy_parse.c: HA state parsing, Predbat plan resampling
// and rate steps.
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "energy_parse.h"

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
#define NEAR(a, b) (fabsf((a) - (b)) < 0.001f)

// 2026-11-20T00:00:00Z
#define DAY0 1795132800u

static void test_time(void)
{
    const char *a = "2026-11-20T13:05:00+0000";
    CHECK(ha_parse_time(a, strlen(a)) == DAY0 + 13 * 3600 + 5 * 60, "basic %u", ha_parse_time(a, strlen(a)));
    const char *b = "2026-11-20T13:05:00+00:00";
    CHECK(ha_parse_time(b, strlen(b)) == DAY0 + 13 * 3600 + 300, "colon offset");
    const char *c = "2026-11-20T14:05:00.123456+01:00";
    CHECK(ha_parse_time(c, strlen(c)) == DAY0 + 13 * 3600 + 300, "fraction and +01:00");
    const char *d = "2026-11-20T13:05:00Z";
    CHECK(ha_parse_time(d, strlen(d)) == DAY0 + 13 * 3600 + 300, "Z");
    const char *e = "2024-02-29T00:00:00-0500";
    CHECK(ha_parse_time(e, strlen(e)) == 1709182800u, "leap day, west %u", ha_parse_time(e, strlen(e)));
    CHECK(ha_parse_time("garbage", 7) == 0, "garbage");
}

static void test_states(void)
{
    const char *j = "{\"entity_id\":\"sensor.x\",\"state\":\"1187.0\",\"attributes\":{\"state_class\":\"measurement\","
                    "\"state\":\"nested\",\"unit_of_measurement\":\"W\"},\"last_changed\":\"2026-11-20T12:00:00+00:00\"}";
    CHECK(NEAR(ha_state_num(j), 1187.0f), "num");
    char b[24];
    CHECK(ha_state_str(j, b, sizeof b) && !strcmp(b, "1187.0"), "str");
    CHECK(isnan(ha_state_num("{\"entity_id\":\"x\",\"state\":\"unavailable\",\"attributes\":{}}")), "unavailable");
    CHECK(isnan(ha_state_num("{\"entity_id\":\"x\",\"state\":\"unknown\",\"attributes\":{}}")), "unknown");
    CHECK(isnan(ha_state_num("{\"message\":\"Entity not found.\"}")), "missing");
    // Attributes come first in some payloads; nested "state" must not win.
    const char *k = "{\"attributes\":{\"state\":\"5\",\"soc_now\":1.74,\"soc_max\":17.4,\"results\":{}},"
                    "\"entity_id\":\"sensor.example_battery_plan\",\"state\":\"10.185\"}";
    CHECK(NEAR(ha_state_num(k), 10.185f), "top-level state after attributes %f", ha_state_num(k));
    CHECK(NEAR(ha_attr_num(k, "soc_now"), 1.74f), "soc_now");
    CHECK(NEAR(ha_attr_num(k, "soc_max"), 17.4f), "soc_max");
    CHECK(isnan(ha_attr_num(k, "nope")), "absent attr");
    const char *st = "{\"entity_id\":\"sensor.example_plan_status\",\"state\":\"Charging \\\"low\\\"\",\"attributes\":{}}";
    CHECK(ha_state_str(st, b, sizeof b) && !strcmp(b, "Charging \"low\""), "escaped status '%s'", b);
}

static void test_plan(void)
{
    // 5-minute plan from 12:35 with a gap from 13:00 to 13:30, values in kWh.
    char json[4096];
    int o = snprintf(json, sizeof json,
                     "{\"entity_id\":\"sensor.example_battery_plan\",\"state\":\"1.74\",\"attributes\":{\"soc_now\":1.74,"
                     "\"soc_max\":17.4,\"results\":{");
    float v = 1.0f;
    int first = 1;
    for (int m = 12 * 60 + 35; m <= 14 * 60; m += 5) {
        if (m > 13 * 60 && m < 13 * 60 + 30) continue;  // gap
        o += snprintf(json + o, sizeof json - o, "%s\"2026-11-20T%02d:%02d:00+0000\":%.2f", first ? "" : ",", m / 60,
                      m % 60, v);
        first = 0;
        v += 0.5f;
    }
    o += snprintf(json + o, sizeof json - o, "},\"friendly_name\":\"Battery SoC kWh best\"}}");
    ha_point_t pts[64];
    int n = ha_results(json, pts, 64);
    CHECK(n == 13, "points %d", n);
    // Slots from 12:30: 12:30 before the first point -> NAN; 12:45 -> value at 12:45 (third point = 2.0).
    float out[96];
    ha_resample(pts, n, DAY0 + 12 * 3600 + 30 * 60, 900, 96, out);
    CHECK(isnan(out[0]), "before first %f", out[0]);
    CHECK(NEAR(out[1], 2.0f), "12:45 %f", out[1]);
    CHECK(NEAR(out[2], 3.5f), "13:00 %f", out[2]);
    // 13:15 is in the gap: carry 13:00 forward.
    CHECK(NEAR(out[3], 3.5f), "13:15 carried %f", out[3]);
    CHECK(NEAR(out[4], 4.0f), "13:30 %f", out[4]);
    CHECK(NEAR(out[95], pts[n - 1].v), "after the end carries the last value");
    // Unsorted input is sorted.
    const char *u = "{\"entity_id\":\"x\",\"state\":\"1\",\"attributes\":{\"results\":{"
                    "\"2026-11-20T13:00:00+0000\":2,\"2026-11-20T12:00:00+0000\":1,\"bad\":3,\"2026-11-20T14:00:00+0000\":\"x\"}}}";
    n = ha_results(u, pts, 64);
    CHECK(n == 2 && pts[0].t < pts[1].t && NEAR(pts[0].v, 1), "unsorted/bad entries n=%d", n);
}

static void test_rates(void)
{
    const char *j = "{\"entity_id\":\"sensor.example_import_rates\",\"state\":\"28.66\",\"attributes\":{\"min\":14.05,\"results\":{"
                    "\"2026-11-20T12:30:00+0000\":28.66,\"2026-11-20T13:00:00+0000\":28.66,"
                    "\"2026-11-20T13:30:00+0000\":14.05,\"2026-11-20T16:00:00+0000\":35.1,"
                    "\"2026-11-20T19:00:00+0000\":28.66},\"friendly_name\":\"Import rates\"}}";
    ha_point_t pts[16];
    int n = ha_results(j, pts, 16);
    CHECK(n == 5, "n %d", n);
    float rate = ha_state_num(j);
    uint32_t now = DAY0 + 12 * 3600 + 41 * 60;
    float nv;
    uint32_t at;
    CHECK(ha_next_change(pts, n, now, rate, &nv, &at) && NEAR(nv, 14.05f) && at == DAY0 + 13 * 3600 + 1800,
          "next change %f at %u", nv, at);
    float out[96];
    uint32_t start = now - now % 900;
    ha_resample(pts, n, start, 900, 96, out);
    CHECK(NEAR(out[0], 28.66f), "slot 0 %f", out[0]);    // 12:30
    CHECK(NEAR(out[4], 14.05f), "13:30 %f", out[4]);
    CHECK(NEAR(out[13], 14.05f), "15:45 %f", out[13]);
    CHECK(NEAR(out[14], 35.1f), "16:00 %f", out[14]);
    CHECK(NEAR(out[95], 28.66f), "end %f", out[95]);
    // Nothing changes after now.
    CHECK(!ha_next_change(pts, n, DAY0 + 20 * 3600, 28.66f, &nv, &at), "no later change");
    // Before the schedule starts the plan is NAN.
    ha_resample(pts, n, DAY0 + 11 * 3600, 900, 96, out);
    CHECK(isnan(out[0]) && NEAR(out[6], 28.66f), "before the first step");
}

int main(void)
{
    test_time();
    test_states();
    test_plan();
    test_rates();
    printf("energy: %d checks, %d failed\n", checks, fails);
    return fails ? 1 : 0;
}
