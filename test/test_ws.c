// Host tests for ws_parse.c (WebSocket frames) and wf_fold.c (waterfall maths).
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "wf_fold.h"
#include "ws_parse.h"

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

typedef struct {
    uint8_t op;
    size_t len;
    uint8_t first, last;
    char text[64];
} got_t;
static got_t got[32];
static int ngot;

static void on_msg(void *ctx, uint8_t op, const uint8_t *p, size_t len)
{
    if (ngot >= 32) return;
    got_t *g = &got[ngot++];
    g->op = op;
    g->len = len;
    g->first = len ? p[0] : 0;
    g->last = len ? p[len - 1] : 0;
    size_t n = len < 63 ? len : 63;
    memcpy(g->text, p, n);
    g->text[n] = 0;
}

// Server frame (unmasked unless mask given).
static size_t frame(uint8_t *out, int fin, uint8_t op, const uint8_t *pl, size_t len, const uint8_t *mask)
{
    size_t o = 0;
    out[o++] = (fin ? 0x80 : 0) | op;
    uint8_t m = mask ? 0x80 : 0;
    if (len < 126) {
        out[o++] = m | (uint8_t)len;
    } else if (len < 65536) {
        out[o++] = m | 126;
        out[o++] = len >> 8;
        out[o++] = len & 255;
    } else {
        out[o++] = m | 127;
        for (int k = 7; k >= 0; k--) out[o++] = (uint8_t)((uint64_t)len >> (8 * k));
    }
    if (mask) memcpy(out + o, mask, 4), o += 4;
    for (size_t k = 0; k < len; k++) out[o + k] = mask ? pl[k] ^ mask[k & 3] : pl[k];
    return o + len;
}

static void test_ws(void)
{
    static uint8_t buf[8192], stream[200000];
    ws_dec_t d;
    ws_dec_init(&d, buf, sizeof buf);
    size_t s = 0;
    const char *t = "{\"type\":\"config\"}";
    s += frame(stream + s, 1, WS_OP_TEXT, (const uint8_t *)t, strlen(t), NULL);
    // A spectrum line: 4101 bytes, 126-length form.
    static uint8_t line[4101];
    for (int i = 0; i < 4101; i++) line[i] = (uint8_t)i;
    line[0] = 0x01;
    s += frame(stream + s, 1, WS_OP_BINARY, line, sizeof line, NULL);
    // Ping in the middle of a fragmented text message.
    s += frame(stream + s, 0, WS_OP_TEXT, (const uint8_t *)"hel", 3, NULL);
    s += frame(stream + s, 1, WS_OP_PING, (const uint8_t *)"pp", 2, NULL);
    s += frame(stream + s, 0, WS_OP_CONT, (const uint8_t *)"lo ", 3, NULL);
    s += frame(stream + s, 1, WS_OP_CONT, (const uint8_t *)"world", 5, NULL);
    // Masked (servers should not, but accept it) and an empty message.
    const uint8_t mk[4] = {1, 2, 3, 4};
    s += frame(stream + s, 1, WS_OP_TEXT, (const uint8_t *)"masked", 6, mk);
    s += frame(stream + s, 1, WS_OP_BINARY, NULL, 0, NULL);
    // Too big for the buffer: dropped, the stream carries on.
    static uint8_t big[70000];
    memset(big, 7, sizeof big);
    s += frame(stream + s, 1, WS_OP_BINARY, big, sizeof big, NULL);
    s += frame(stream + s, 1, WS_OP_TEXT, (const uint8_t *)"after", 5, NULL);
    s += frame(stream + s, 1, WS_OP_CLOSE, (const uint8_t *)"\x03\xe8", 2, NULL);

    // Byte by byte, then in odd chunks: the same result.
    for (int pass = 0; pass < 2; pass++) {
        ngot = 0;
        ws_dec_init(&d, buf, sizeof buf);
        int bad = 0;
        if (pass == 0) {
            for (size_t i = 0; i < s; i++) bad |= ws_dec_feed(&d, stream + i, 1, on_msg, NULL);
        } else {
            for (size_t i = 0; i < s; i += 777) bad |= ws_dec_feed(&d, stream + i, s - i < 777 ? s - i : 777, on_msg, NULL);
        }
        CHECK(!bad, "pass %d error", pass);
        CHECK(ngot == 8, "pass %d messages %d", pass, ngot);
        CHECK(got[0].op == WS_OP_TEXT && !strcmp(got[0].text, t), "text");
        CHECK(got[1].op == WS_OP_BINARY && got[1].len == 4101 && got[1].first == 1 && got[1].last == (uint8_t)4100,
              "binary 126-length %zu", got[1].len);
        CHECK(got[2].op == WS_OP_PING && got[2].len == 2, "ping between fragments");
        CHECK(got[3].op == WS_OP_TEXT && !strcmp(got[3].text, "hello world"), "reassembled '%s'", got[3].text);
        CHECK(got[4].op == WS_OP_TEXT && !strcmp(got[4].text, "masked"), "masked '%s'", got[4].text);
        CHECK(got[5].op == WS_OP_BINARY && got[5].len == 0, "empty");
        CHECK(got[6].op == WS_OP_TEXT && !strcmp(got[6].text, "after"), "after an oversized message (127-length)");
        CHECK(got[7].op == WS_OP_CLOSE && got[7].len == 2, "close");
    }
    // Protocol errors.
    ws_dec_init(&d, buf, sizeof buf);
    uint8_t e1[] = {0x80, 0x01, 'x'};  // continuation of nothing
    CHECK(ws_dec_feed(&d, e1, 3, on_msg, NULL) == -1, "bad continuation");
    ws_dec_init(&d, buf, sizeof buf);
    uint8_t e2[] = {0x09, 0x00};       // fragmented control frame
    CHECK(ws_dec_feed(&d, e2, 2, on_msg, NULL) == -1, "fragmented ping");
    // Pong frame: masked, and unmasks back to the payload.
    uint8_t out[16];
    size_t n = ws_control_frame(WS_OP_PONG, (const uint8_t *)"pp", 2, mk, out);
    CHECK(n == 8 && out[0] == 0x8A && out[1] == 0x82 && (out[6] ^ mk[0]) == 'p' && (out[7] ^ mk[1]) == 'p', "pong");
    char key[25];
    const uint8_t rnd[16] = {0};
    ws_key(rnd, key);
    CHECK(!strcmp(key, "AAAAAAAAAAAAAAAAAAAAAA=="), "key '%s'", key);
    uint8_t r2[16];
    memcpy(r2, "the sample nonce", 16);
    ws_key(r2, key);
    CHECK(!strcmp(key, "dGhlIHNhbXBsZSBub25jZQ=="), "RFC 6455 example key '%s'", key);
}

typedef struct {
    uint32_t row[64];
    uint8_t col0[64], col359[64];
    bool tx[64];
    int n;
} rows_t;

static void on_row(void *ctx, uint32_t row, const uint8_t cols[WF_W], bool tx)
{
    rows_t *r = ctx;
    if (r->n >= 64) return;
    r->row[r->n] = row;
    r->col0[r->n] = cols[0];
    r->col359[r->n] = cols[WF_W - 1];
    r->tx[r->n] = tx;
    r->n++;
}

static void test_fold(void)
{
    // Span from GB7XYZ-like modems.
    float lo[] = {386.7f, 950, 1675.8f, 2660.2f}, hi[] = {714.8f, 1450, 2027.3f, 5460.9f};
    float a, b;
    wf_span(lo, hi, 4, 48000, &a, &b);
    CHECK(fabsf(a - 236.7f) < 0.01f && fabsf(b - 5610.9f) < 0.01f, "span %f %f", a, b);
    wf_span(NULL, NULL, 0, 48000, &a, &b);
    CHECK(a == 0 && b == 3000, "no modems");
    float l1[] = {50}, h1[] = {23990};
    wf_span(l1, h1, 1, 48000, &a, &b);
    CHECK(a == 0 && b == 24000, "clamped %f %f", a, b);

    // Columns over 0..3600 Hz with 10 Hz bins: 10 Hz per column, one bin each.
    static wf_map_t m;
    wf_map_build(&m, 0, 3600, 10, 4096);
    CHECK(m.b0[0] == 0 && m.b1[0] == 0 && m.b0[359] == 359 && m.b1[359] == 359, "one bin per column");
    // 5.859375 Hz bins, 236.7..5610.9 Hz: about 2.5 bins per column, all covered once.
    wf_map_build(&m, 236.7f, 5610.9f, 5.859375f, 4096);
    int gaps = 0, overlaps = 0;
    for (int c = 1; c < WF_W; c++) {
        if (m.b0[c] > m.b1[c - 1] + 1) gaps++;
        if (m.b0[c] <= m.b1[c - 1]) overlaps++;
    }
    CHECK(!gaps && !overlaps && m.b0[0] == 41 && m.b1[WF_W - 1] == 957, "contiguous %d %d %u %u", gaps, overlaps, m.b0[0],
          m.b1[WF_W - 1]);
    // Narrow span: fewer bins than columns, each column the nearest bin.
    wf_map_build(&m, 1000, 1100, 5.859375f, 4096);
    CHECK(m.b0[0] == 171 && m.b1[0] == 171 && m.b0[359] == 188, "nearest %u %u", m.b0[0], m.b0[359]);
    // Max over bins.
    static uint8_t bins[4096], cols[WF_W];
    wf_map_build(&m, 0, 3600, 5, 4096);  // two bins per column
    for (int i = 0; i < 4096; i++) bins[i] = (uint8_t)(i & 1 ? 200 : 10);
    wf_fold_line(&m, bins, cols);
    CHECK(cols[0] == 200 && cols[359] == 200, "max %u", cols[0]);

    // Rows: 4 lines each, from the first line seen.
    static wf_rows_t r;
    rows_t got = {0};
    wf_rows_reset(&r, 100);
    for (uint32_t line = 1000; line < 1012; line++) {
        memset(cols, (uint8_t)(line - 1000), WF_W);
        wf_rows_feed(&r, line, cols, line == 1005, on_row, &got);
    }
    CHECK(got.n == 3 && got.row[0] == 100 && got.row[2] == 102, "three rows %d", got.n);
    CHECK(got.col0[0] == 3 && got.col0[1] == 7 && got.col0[2] == 11, "row is the max of its lines");
    CHECK(!got.tx[0] && got.tx[1] && !got.tx[2], "tx flag from any line");
    // A gap of 6 lines: the partial row is closed and an empty one fills the gap.
    got.n = 0;
    memset(cols, 50, WF_W);
    wf_rows_feed(&r, 1012, cols, false, on_row, &got);
    wf_rows_feed(&r, 1020, cols, false, on_row, &got);
    CHECK(got.n == 2 && got.row[0] == 103 && got.row[1] == 104 && got.col0[1] == 0, "gap rows %d", got.n);
    // Tags: a frame ending in line 1019 (row 104) that lasted 10 lines: 3 rows from 101.
    uint32_t st;
    uint16_t nr;
    CHECK(wf_tag_rows(&r, 1019, 10, &st, &nr) && st == 101 && nr == 3, "tag %u %u", st, nr);
    CHECK(wf_tag_rows(&r, 1019, 0, &st, &nr) && st == 104 && nr == 1, "no burst length %u %u", st, nr);
    CHECK(!wf_tag_rows(&r, 999, 4, &st, &nr), "before the origin");
    CHECK(wf_tag_rows(&r, 1001, 400, &st, &nr) && st == 100 && nr == 1, "clamped to the origin %u %u", st, nr);
    // The stream restarts (line index back to 5): numbering carries on.
    got.n = 0;
    for (uint32_t line = 5; line < 9; line++) wf_rows_feed(&r, line, cols, false, on_row, &got);
    CHECK(got.n == 2 && got.row[0] == 105 && got.row[1] == 106, "restart %d %u %u", got.n, got.row[0], got.row[1]);
    // Line 8 ends row 106 (the first after the restart); a 4-line burst would start in row 105, from
    // before the restart, so it is held to 106.
    CHECK(wf_tag_rows(&r, 8, 4, &st, &nr) && st == 106 && nr == 1, "tag after restart %u %u", st, nr);
}

int main(void)
{
    test_ws();
    test_fold();
    printf("ws: %d checks, %d failed\n", checks, fails);
    return fails ? 1 : 0;
}
