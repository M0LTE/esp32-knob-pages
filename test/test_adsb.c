// Host tests for adsb_parse.c. The home point is made up (the middle of the
// sample's box, near Birmingham airport), not anyone's address.
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "adsb_parse.h"

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
#define NEAR(a, b, e) (fabsf((a) - (b)) < (e))

#define HOME_LAT 52.45f
#define HOME_LON -1.75f

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

static void test_box(void)
{
    float b[4];
    adsb_box(HOME_LAT, HOME_LON, 25, b);
    CHECK(NEAR(b[1] - HOME_LAT, 25 / 111.32f, 1e-4f) && NEAR(HOME_LAT - b[0], 25 / 111.32f, 1e-4f), "lat span");
    float dlon = 25 / (111.32f * cosf(HOME_LAT * 3.14159265f / 180));
    CHECK(NEAR(b[3] - HOME_LON, dlon, 1e-4f) && NEAR(HOME_LON - b[2], dlon, 1e-4f), "lon span %f", b[3] - HOME_LON);
    // The box's edge midpoints are 25 km out.
    CHECK(NEAR(adsb_dist_km(HOME_LAT, HOME_LON, b[1], HOME_LON), 25, 0.01f), "north edge");
    CHECK(NEAR(adsb_dist_km(HOME_LAT, HOME_LON, HOME_LAT, b[3]), 25, 0.05f), "east edge %f",
          adsb_dist_km(HOME_LAT, HOME_LON, HOME_LAT, b[3]));
}

static void test_parse(void)
{
    char *j = slurp("adsb_box_sample.json");
    CHECK(j != NULL, "sample");
    if (!j) return;
    static aircraft_t a[64];
    int n = adsb_parse(j, a, 64, 1000);
    printf("adsb sample: %d aircraft with positions\n", n);
    CHECK(n > 5, "n %d", n);
    const aircraft_t *ry = NULL;
    int ground = 0;
    for (int i = 0; i < n; i++) {
        if (!strcmp(a[i].hex, "4ca242")) ry = &a[i];
        if (a[i].alt_ft == 0) ground++;
    }
    CHECK(ry && !strcmp(ry->flight, "RYR6GJ") && !strcmp(ry->reg, "EI-DCL") && !strcmp(ry->type, "B738"),
          "fields, trimmed callsign '%s'", ry ? ry->flight : "");
    CHECK(ry && NEAR(ry->alt_ft, 37000, 0.5f) && NEAR(ry->gs_kt, 476, 0.1f) && NEAR(ry->track, 133.55f, 0.01f) &&
              NEAR(ry->rate_fpm, 0, 0.1f) && !strcmp(ry->cat, "A3") && ry->seen_ms == 1000 && ry->trail_n == 0,
          "numbers");
    CHECK(ry && !strcmp(ry->op, "RYANAIR DESIGNATED ACTIVITY COM"), "op truncated '%s'", ry ? ry->op : "");
    CHECK(ground >= 1, "ground altitude read as 0 (%d)", ground);
    free(j);

    const char *k = "{\"ac\":[{\"hex\":\"ABC123\",\"lat\":52.5,\"lon\":-1.7,\"alt_baro\":\"ground\"},"
                    "{\"hex\":\"def456\",\"flight\":\"  \",\"lat\":52.4,\"lon\":-1.8,\"geom_rate\":-640},"
                    "{\"hex\":\"no_pos\",\"flight\":\"X\"},{\"lat\":1,\"lon\":2},"
                    "{\"hex\":\"a1\",\"lat\":52.4,\"lon\":-1.8,\"alt_baro\":\"weird\",\"gs\":null}]}";
    n = adsb_parse(k, a, 64, 5);
    CHECK(n == 3, "n %d", n);
    CHECK(!strcmp(a[0].hex, "abc123") && a[0].alt_ft == 0 && isnan(a[0].gs_kt) && a[0].flight[0] == 0, "minimal");
    CHECK(a[1].flight[0] == 0 && NEAR(a[1].rate_fpm, -640, 0.1f) && isnan(a[1].alt_ft), "blank callsign, geom_rate");
    CHECK(isnan(a[2].alt_ft) && isnan(a[2].gs_kt), "odd altitude, null speed");
    CHECK(adsb_parse("{\"now\":1}", a, 64, 0) == 0, "no ac array");
    CHECK(adsb_parse("garbage", a, 64, 0) == -1, "bad json");
}

static aircraft_t mk(const char *hex, float lat, float lon, uint32_t t)
{
    aircraft_t x;
    memset(&x, 0, sizeof x);
    snprintf(x.hex, sizeof x.hex, "%s", hex);
    snprintf(x.flight, sizeof x.flight, "T%s", hex);
    x.lat = lat, x.lon = lon, x.seen_ms = t;
    return x;
}

static void test_merge(void)
{
    static aircraft_t cur[MODEL_MAX_AC], fresh[80], scratch[MODEL_MAX_AC + 80];
    int n = 0;
    fresh[0] = mk("far", 52.6f, -1.75f, 0);
    fresh[1] = mk("near", 52.46f, -1.75f, 0);
    adsb_merge(cur, &n, fresh, 2, HOME_LAT, HOME_LON, 0, scratch);
    CHECK(n == 2 && !strcmp(cur[0].hex, "near") && cur[0].trail_n == 1, "sorted nearest first, trail starts");
    snprintf(cur[0].orig, sizeof cur[0].orig, "EGBB");  // a route found meanwhile
    // Next poll: near moved, far did not, a new one arrived.
    fresh[0] = mk("near", 52.47f, -1.75f, 3000);
    fresh[1] = mk("far", 52.6f, -1.75f, 3000);
    fresh[2] = mk("mid", 52.5f, -1.75f, 3000);
    adsb_merge(cur, &n, fresh, 3, HOME_LAT, HOME_LON, 3000, scratch);
    CHECK(n == 3 && !strcmp(cur[0].hex, "near") && !strcmp(cur[1].hex, "mid") && !strcmp(cur[2].hex, "far"), "order");
    CHECK(cur[0].trail_n == 2 && NEAR(cur[0].trail_lat[0], 52.46f, 1e-5f) && NEAR(cur[0].trail_lat[1], 52.47f, 1e-5f),
          "trail oldest first");
    CHECK(cur[2].trail_n == 1, "no new point when it did not move");
    CHECK(!strcmp(cur[0].orig, "EGBB"), "route kept by hex");
    // Trail capped at AC_TRAIL.
    for (int k = 0; k < 15; k++) {
        fresh[0] = mk("near", 52.47f + 0.001f * (k + 1), -1.75f, 6000 + 3000 * k);
        adsb_merge(cur, &n, fresh, 1, HOME_LAT, HOME_LON, 6000 + 3000 * k, scratch);
    }
    const aircraft_t *nr = NULL;
    for (int i = 0; i < n; i++)
        if (!strcmp(cur[i].hex, "near")) nr = &cur[i];
    CHECK(nr && nr->trail_n == AC_TRAIL && NEAR(nr->trail_lat[AC_TRAIL - 1], 52.485f, 1e-4f), "trail capped");
    // far and mid were last seen at 3000; at 6000 + 3000*14 = 48000 they are kept, after 63000 gone.
    CHECK(n == 3, "kept while recent: %d", n);
    adsb_merge(cur, &n, fresh, 1, HOME_LAT, HOME_LON, 63001, scratch);
    CHECK(n == 1 && !strcmp(cur[0].hex, "near"), "forgotten after a minute: %d", n);
    // Cap at MODEL_MAX_AC.
    for (int i = 0; i < 80; i++) {
        char h[16];
        snprintf(h, sizeof h, "x%02d", i);
        fresh[i] = mk(h, HOME_LAT + 0.001f * i, HOME_LON, 70000);
    }
    adsb_merge(cur, &n, fresh, 80, HOME_LAT, HOME_LON, 70000, scratch);
    CHECK(n == MODEL_MAX_AC && !strcmp(cur[0].hex, "x00"), "capped to the nearest %d", n);
}

static void test_routes(void)
{
    char *j = slurp("adsbdb_route_sample.json");
    adsb_route_t r;
    CHECK(j && adsbdb_parse(j, 200, &r) == 1, "route");
    CHECK(!strcmp(r.orig, "EGLL") && !strcmp(r.dest, "OTHH") && !strcmp(r.orig_city, "London") &&
              !strcmp(r.dest_city, "Doha"), "fields %s %s %s %s", r.orig, r.dest, r.orig_city, r.dest_city);
    free(j);
    CHECK(adsbdb_parse("{\"response\":\"unknown callsign\"}", 404, &r) == 0, "404");
    CHECK(adsbdb_parse("{\"response\":\"unknown callsign\"}", 200, &r) == 0, "unknown in a 200");
    CHECK(adsbdb_parse("", 500, &r) == -1, "server error");
    CHECK(adsbdb_parse("{nope", 200, &r) == -1, "bad json");
    char k[12];
    CHECK(adsb_callsign_key("baw123  ", k, sizeof k) && !strcmp(k, "BAW123"), "key '%s'", k);
    CHECK(!adsb_callsign_key("  ", k, sizeof k), "blank callsign");
}

int main(void)
{
    test_box();
    test_parse();
    test_merge();
    test_routes();
    printf("adsb: %d checks, %d failed\n", checks, fails);
    return fails ? 1 : 0;
}
