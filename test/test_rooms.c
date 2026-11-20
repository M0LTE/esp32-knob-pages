// Host tests for rooms_parse.c: settings lines, the night helper window and
// a climate controller's /api/state (made-up sample).
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "rooms_parse.h"

model_t g_model;
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

// Made-up rooms and helper entities.
static const char *CFG =
    "living Living input_number.living_target\n"
    "office Office input_number.office_target\r\n"
    "guest Guest input_number.guest_target\n"
    "landing Landing_room input_number.landing_target\n"
    "bedroom Bedroom input_number.bedroom_day_target input_number.bedroom_night_target 22:00-07:30\n"
    "\n"
    "broken line\n"
    "garage Garage input_number.x input_number.y 25:00-07:00\n";

static void test_cfg(room_cfg_t *c, int *n)
{
    *n = rooms_cfg_parse(CFG, c, 8);
    CHECK(*n == 5, "rooms %d", *n);
    CHECK(!strcmp(c[3].name, "Landing room"), "underscores to spaces '%s'", c[3].name);
    CHECK(!strcmp(c[1].helper, "input_number.office_target"), "CRLF '%s'", c[1].helper);
    CHECK(c[4].night_from == 22 * 60 && c[4].night_to == 7 * 60 + 30, "window %d-%d", c[4].night_from, c[4].night_to);
    char line[200];
    rooms_cfg_line(&c[4], line, sizeof line);
    CHECK(!strcmp(line, "bedroom Bedroom input_number.bedroom_day_target input_number.bedroom_night_target 22:00-07:30"),
          "line '%s'", line);
    rooms_cfg_line(&c[3], line, sizeof line);
    CHECK(!strcmp(line, "landing Landing_room input_number.landing_target"), "line '%s'", line);

    // Night window across midnight: in from 22:00 up to (not including) 07:30.
    const room_cfg_t *b = &c[4];
    CHECK(!strcmp(rooms_helper_at(b, 21 * 60 + 59), b->helper), "21:59 day");
    CHECK(!strcmp(rooms_helper_at(b, 22 * 60), b->night), "22:00 night");
    CHECK(!strcmp(rooms_helper_at(b, 0), b->night), "00:00 night");
    CHECK(!strcmp(rooms_helper_at(b, 7 * 60 + 29), b->night), "07:29 night");
    CHECK(!strcmp(rooms_helper_at(b, 7 * 60 + 30), b->helper), "07:30 day");
    CHECK(!strcmp(rooms_helper_at(&c[0], 23 * 60), c[0].helper), "no night helper");
    // A window within one day.
    room_cfg_t d;
    rooms_cfg_parse("x X h.day h.mid 01:00-05:00", &d, 1);
    CHECK(!strcmp(rooms_helper_at(&d, 0), "h.day") && !strcmp(rooms_helper_at(&d, 60), "h.mid") &&
              !strcmp(rooms_helper_at(&d, 300), "h.day"), "same-day window");
}

static char *slurp(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *b = malloc(n + 1);
    if (fread(b, 1, n, f) != (size_t)n) n = 0;
    b[n] = 0;
    fclose(f);
    return b;
}

static void test_state(const room_cfg_t *c, int n)
{
    char *j = slurp("brain_state_sample.json");
    CHECK(j != NULL, "sample");
    if (!j) return;
    room_t r[8];
    memset(r, 0, sizeof r);
    for (int i = 0; i < n; i++) r[i].t_min = 16, r[i].t_max = 28, r[i].t_step = 0.5f;
    int found = rooms_parse_state(j, c, n, r);
    CHECK(found == 5, "found %d", found);
    for (int i = 0; i < n; i++)
        printf("  %-9s %-10s %-8s target %5.1f temp %5.2f rh %5.1f ac %-5s %5.1f valves %d demand %d why '%s'\n",
               r[i].id, r[i].name, r[i].status, r[i].target, r[i].temp, r[i].humidity, r[i].ac_mode, r[i].ac_setpoint,
               r[i].valves, r[i].heat_demand, r[i].why);
    // Configured order kept (the sample lists bedroom before guest).
    CHECK(!strcmp(r[2].id, "guest") && !strcmp(r[4].id, "bedroom"), "order");
    CHECK(!strcmp(r[1].status, "cooling") && NEAR(r[1].target, 22.0f) && NEAR(r[1].temp, 23.41f), "office");
    CHECK(!strcmp(r[1].ac_mode, "cool") && NEAR(r[1].ac_setpoint, 19.0f), "office A/C");
    CHECK(!strcmp(r[0].ac_mode, "off") && isnan(r[0].ac_setpoint), "setpoint NAN when off");
    CHECK(r[0].valves == 0 && !r[0].heat_demand, "living valves shut");
    CHECK(r[4].valves == -1, "bedroom has no heating");
    CHECK(isnan(r[3].humidity), "landing humidity null");
    CHECK(r[2].valves == 1 && r[2].heat_demand, "guest valves open and demanding");
    CHECK(strlen(r[1].why) == sizeof r[1].why - 1, "why truncated to %zu", strlen(r[1].why));
    CHECK(NEAR(r[0].t_max, 28) && NEAR(r[0].t_step, 0.5f), "limits kept");
    free(j);

    // heat_plant and unknown zones ignored, a configured zone missing, nulls, valves open, UTF-8 cut.
    const char *k = "{\"decisions\":[{\"zone_id\":\"heat_plant\",\"status\":\"heating\",\"target\":60},"
                    "{\"zone_id\":\"attic\",\"status\":\"cooling\"},"
                    "{\"zone_id\":\"living\",\"status\":\"heating\",\"target\":null,\"pv\":19.5,\"humidity\":null,"
                    "\"current\":{\"mode\":\"heat\",\"setpoint\":null},\"heating\":{\"valves_open\":true,"
                    "\"demanding\":true},\"why\":[\"r\\u00e9chauffement \\u00e9\\u00e9\\u00e9\\u00e9\\u00e9\\u00e9\\u00e9"
                    "\\u00e9\\u00e9\\u00e9\\u00e9\\u00e9\\u00e9\\u00e9\\u00e9\\u00e9\\u00e9\\u00e9\\u00e9\\u00e9\\u00e9"
                    "\\u00e9\\u00e9\\u00e9\\u00e9\\u00e9\\u00e9\\u00e9\\u00e9\\u00e9\\u00e9\\u00e9\\u00e9\\u00e9\\u00e9\"]}]}";
    found = rooms_parse_state(k, c, n, r);
    CHECK(found == 1, "found %d", found);
    CHECK(!strcmp(r[0].status, "heating") && isnan(r[0].target) && NEAR(r[0].temp, 19.5f), "nulls");
    CHECK(!strcmp(r[0].ac_mode, "heat") && isnan(r[0].ac_setpoint), "null setpoint");
    CHECK(r[0].valves == 1 && r[0].heat_demand, "valves open");
    size_t wl = strlen(r[0].why);
    CHECK(wl < sizeof r[0].why && ((unsigned char)r[0].why[wl - 1] & 0xC0) != 0xC0, "utf-8 cut cleanly (%zu)", wl);
    CHECK(r[1].status[0] == 0 && isnan(r[1].temp) && !strcmp(r[1].name, "Office"), "missing zone keeps name");
    CHECK(rooms_parse_state("{nope", c, n, r) == -1, "bad json");
    CHECK(rooms_parse_state("{\"x\":1}", c, n, r) == -1, "no decisions");
}

int main(void)
{
    room_cfg_t c[8];
    int n;
    test_cfg(c, &n);
    test_state(c, n);
    printf("rooms: %d checks, %d failed\n", checks, fails);
    return fails ? 1 : 0;
}
