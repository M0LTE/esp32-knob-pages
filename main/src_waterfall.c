// Live waterfall from a pdn-soundmodem station page WebSocket (ws://host:port/).
// Listen only: after the HTTP upgrade request the knob sends nothing but what
// the WebSocket protocol itself requires (a pong to a ping, a close in reply
// to a close) and exactly three fixed text messages of the station page's
// own protocol: {"type":"pong"}, the keep-alive answer (the page sends
// {"type":"ping"} every 20 s and drops a viewer that stays silent for 60 s),
// and {"type":"audio","on":true} / {"type":"audio","on":false} to start and
// stop its receive audio while the page is being listened to. Nothing else
// is ever sent: the page protocol also carries commands (txtest, a
// transmitter test) that key the radio. Binary messages are spectrum lines
// (0x01 heard, 0x03 our own transmission) and audio blocks (0x02); text
// messages are JSON, and only "config" and "frame" are read.
#include <errno.h>
#include <stddef.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/task.h"
#include "lwip/netdb.h"
#include "lwip/sockets.h"
#include "model.h"
#include "net.h"
#include "platform.h"
#include "settings.h"
#include "sources.h"
#include "audio.h"
#include "rx_audio.h"
#include "wf_fold.h"
#include "ws_parse.h"

static const char *TAG = "waterfall";

#define MSG_MAX (128 * 1024)   // biggest message kept (history and host lists can be large)

volatile int g_wf_state;       // 0 off, 1 down, 2 up
volatile uint32_t g_wf_rows, g_wf_tags, g_wf_busy_us;
volatile uint32_t g_wf_audio_gap_ms, g_wf_audio_block;  // longest wait between audio blocks, last block size

typedef struct {
    int sock;
    bool closing;
    // from "config"
    bool have_config;
    int line_len;
    float bin_hz, lines_per_s;
    uint32_t sample_rate;   // the audio channel rate
    bool audio_sent_on;     // we have asked for audio on this connection
    uint32_t audio_last_ms; // when the last audio block arrived
    wf_map_t *map;
    wf_rows_t rows;
    uint8_t cols[WF_W];
} wf_ctx_t;

static void send_control(wf_ctx_t *c, uint8_t op, const uint8_t *p, size_t len)
{
    uint8_t frame[6 + 125], mask[4];
    esp_fill_random(mask, sizeof mask);
    size_t n = ws_control_frame(op, p, len, mask, frame);
    send(c->sock, frame, n, 0);  // the only writes on this socket: pong, close, the keep-alive answer
}

// The page's listen switch and volume to the stream: ask for audio, or stop it.
static void follow_listen(wf_ctx_t *c, bool up)
{
    static const char audio_on[] = "{\"type\":\"audio\",\"on\":true}";
    static const char audio_off[] = "{\"type\":\"audio\",\"on\":false}";
    model_lock();
    bool listen = g_model.wf_listen;
    int vol = g_model.wf_volume;
    model_unlock();
#ifdef WF_TEST_LISTEN
    listen = true;  // bench check only: listen from the start, muted
    vol = 0;
#endif
    audio_stream_volume(vol);
    bool want = listen && up && c->sample_rate;
    if (want && !c->audio_sent_on) {
        audio_stream_start(c->sample_rate);
        send_control(c, WS_OP_TEXT, (const uint8_t *)audio_on, sizeof audio_on - 1);
        c->audio_sent_on = true;
        c->audio_last_ms = model_now_ms();
        ESP_LOGI(TAG, "listening to the receive audio at %lu Hz", (unsigned long)c->sample_rate);
    } else if (!want && c->audio_sent_on) {
        if (up) send_control(c, WS_OP_TEXT, (const uint8_t *)audio_off, sizeof audio_off - 1);
        c->audio_sent_on = false;
        audio_stream_stop();
        ESP_LOGI(TAG, "receive audio off");
    }
    bool flowing = c->audio_sent_on && model_now_ms() - c->audio_last_ms < 1000 && audio_stream_playing();
    float db = c->audio_sent_on ? audio_stream_dbfs() : NAN;
    model_lock();
    g_model.wf_audio_on = flowing;
    g_model.wf_audio_dbfs = db;
    model_unlock();
}

static void on_row(void *ctx, uint32_t row, const uint8_t cols[WF_W], bool tx)
{
    model_lock();
    memcpy(g_model.wf_rows[row % WF_ROWS], cols, WF_W);
    g_model.wf_row_tx[row % WF_ROWS] = tx;
    g_model.wf_row_seq = row + 1;
    model_unlock();
    g_wf_rows++;
}

static void on_config(wf_ctx_t *c, cJSON *j)
{
    cJSON *v;
    float sr = cJSON_IsNumber(v = cJSON_GetObjectItem(j, "sampleRate")) ? (float)v->valuedouble : 48000;
    c->sample_rate = (uint32_t)sr;
    c->bin_hz = cJSON_IsNumber(v = cJSON_GetObjectItem(j, "binWidthHz")) ? (float)v->valuedouble : 0;
    c->line_len = cJSON_IsNumber(v = cJSON_GetObjectItem(j, "lineLength")) ? v->valueint : 0;
    c->lines_per_s = cJSON_IsNumber(v = cJSON_GetObjectItem(j, "linesPerSecond")) ? (float)v->valuedouble : 0;
    double dial = cJSON_IsNumber(v = cJSON_GetObjectItem(j, "dialHz")) ? v->valuedouble : 0;
    v = cJSON_GetObjectItem(j, "sideband");
    bool lsb = cJSON_IsString(v) && !strcmp(v->valuestring, "lsb");

    wf_band_t bands[WF_MAX_BANDS];
    float lo[WF_MAX_BANDS], hi[WF_MAX_BANDS];
    int nb = 0;
    cJSON *m, *mods = cJSON_GetObjectItem(j, "modems");
    cJSON_ArrayForEach(m, mods) {
        if (nb >= WF_MAX_BANDS) break;
        cJSON *l = cJSON_GetObjectItem(m, "lowHz"), *h = cJSON_GetObjectItem(m, "highHz");
        if (!cJSON_IsNumber(l) || !cJSON_IsNumber(h)) continue;
        wf_band_t *b = &bands[nb];
        memset(b, 0, sizeof *b);
        b->lo_hz = lo[nb] = (float)l->valuedouble;
        b->hi_hz = hi[nb] = (float)h->valuedouble;
        cJSON *s = cJSON_GetObjectItem(m, "sub");
        b->sub = cJSON_IsNumber(s) ? (uint8_t)s->valueint : 0;
        cJSON *n = cJSON_GetObjectItem(m, "modeName");
        if (cJSON_IsString(n)) snprintf(b->name, sizeof b->name, "%s", n->valuestring);
        nb++;
    }
    float slo, shi;
    wf_span(lo, hi, nb, sr, &slo, &shi);
    c->have_config = c->bin_hz > 0 && c->line_len > 0;
    if (c->have_config) wf_map_build(c->map, slo, shi, c->bin_hz, c->line_len);

    model_lock();
    g_model.wf_dial_hz = dial;
    g_model.wf_lsb = lsb;
    g_model.wf_lo_hz = slo;
    g_model.wf_hi_hz = shi;
    g_model.wf_rows_per_s = c->lines_per_s / WF_LINES_PER_ROW;
    memcpy(g_model.wf_bands, bands, sizeof(wf_band_t) * nb);
    g_model.wf_n_bands = nb;
    model_unlock();
    ESP_LOGI(TAG, "config: %d bands, %.0f to %.0f Hz, %.0f lines/s, %d bins of %.3f Hz", nb, slo, shi,
             c->lines_per_s, c->line_len, c->bin_hz);
}

static void on_frame(wf_ctx_t *c, cJSON *j)
{
    cJSON *v = cJSON_GetObjectItem(j, "line");
    if (!cJSON_IsNumber(v)) return;
    uint32_t line = (uint32_t)v->valuedouble;
    v = cJSON_GetObjectItem(j, "burstLines");
    uint32_t burst = cJSON_IsNumber(v) && v->valuedouble > 0 ? (uint32_t)v->valuedouble : 0;
    wf_tag_t t;
    memset(&t, 0, sizeof t);
    if (!wf_tag_rows(&c->rows, line, burst, &t.row, &t.rows)) return;
    v = cJSON_GetObjectItem(j, "sub");
    t.sub = cJSON_IsNumber(v) ? (uint8_t)v->valueint : 0;
    t.tx = cJSON_IsTrue(cJSON_GetObjectItem(j, "tx"));
    v = cJSON_GetObjectItem(j, "snrDb");
    t.snr_db = cJSON_IsNumber(v) ? (float)v->valuedouble : NAN;
    v = cJSON_GetObjectItem(j, "from");
    if (cJSON_IsString(v)) snprintf(t.from, sizeof t.from, "%s", v->valuestring);
    v = cJSON_GetObjectItem(j, "to");
    if (cJSON_IsString(v)) snprintf(t.to, sizeof t.to, "%s", v->valuestring);
    model_lock();
    g_model.wf_tags[g_model.wf_tag_seq % WF_TAGS] = t;
    g_model.wf_tag_seq++;
    model_unlock();
    g_wf_tags++;
}

static void on_msg(void *ctx, uint8_t op, const uint8_t *p, size_t len)
{
    wf_ctx_t *c = ctx;
    int64_t t0 = esp_timer_get_time();
    if (op == WS_OP_BINARY && len && p[0] == 0x02) {
        const uint8_t *pcm;
        int n = rxa_block(p, len, &pcm);
        if (n > 0 && c->audio_sent_on) {
            audio_stream_push(pcm, n);
            uint32_t now = model_now_ms(), gap = now - c->audio_last_ms;
            if (gap > g_wf_audio_gap_ms) g_wf_audio_gap_ms = gap;
            g_wf_audio_block = n;
            c->audio_last_ms = now;
        }
    } else if (op == WS_OP_BINARY) {
        if (c->have_config && len == (size_t)c->line_len + 5 && (p[0] == 0x01 || p[0] == 0x03)) {
            uint32_t line = p[1] | p[2] << 8 | p[3] << 16 | (uint32_t)p[4] << 24;
            wf_fold_line(c->map, p + 5, c->cols);
            model_lock();
            memcpy(g_model.wf_spec, c->cols, WF_W);
            model_unlock();
            wf_rows_feed(&c->rows, line, c->cols, p[0] == 0x03, on_row, c);
        }
    } else if (op == WS_OP_TEXT) {
        // The type comes first; only parse the two we use.
        size_t head = len < 48 ? len : 48;
        bool cfg = memmem(p, head, "\"type\":\"config\"", 15) != NULL;
        bool frm = memmem(p, head, "\"type\":\"frame\"", 14) != NULL;
        if (memmem(p, head, "\"type\":\"ping\"", 13)) {
            static const char pong[] = "{\"type\":\"pong\"}";
            send_control(c, WS_OP_TEXT, (const uint8_t *)pong, sizeof pong - 1);
        }
        if (cfg || frm) {
            ((uint8_t *)p)[len] = 0;  // the buffer has a spare byte for this
            cJSON *j = cJSON_ParseWithLength((const char *)p, len);
            if (j) {
                if (cfg) on_config(c, j);
                else on_frame(c, j);
                cJSON_Delete(j);
            }
        }
    } else if (op == WS_OP_PING) {
        send_control(c, WS_OP_PONG, p, len);
    } else if (op == WS_OP_CLOSE) {
        if (!c->closing) send_control(c, WS_OP_CLOSE, p, len >= 2 ? 2 : 0);
        c->closing = true;
    }
    g_wf_busy_us += (uint32_t)(esp_timer_get_time() - t0);
}

// ws://host[:port][/path]
static bool parse_url(const char *url, char *host, size_t hn, int *port, char *path, size_t pn)
{
    if (strncmp(url, "ws://", 5)) return false;
    const char *h = url + 5, *slash = strchr(h, '/'), *colon = strchr(h, ':');
    const char *hend = slash ? slash : h + strlen(h);
    if (colon && colon < hend) {
        *port = atoi(colon + 1);
        hend = colon;
    } else {
        *port = 80;
    }
    if (hend == h || (size_t)(hend - h) >= hn) return false;
    memcpy(host, h, hend - h);
    host[hend - h] = 0;
    snprintf(path, pn, "%s", slash ? slash : "/");
    return *port > 0;
}

static int connect_ws(const char *host, int port, const char *path, uint8_t *rest, size_t cap, size_t *rest_n)
{
    char ps[12];
    snprintf(ps, sizeof ps, "%d", port);
    struct addrinfo hints = {.ai_family = AF_INET, .ai_socktype = SOCK_STREAM}, *res = NULL;
    if (getaddrinfo(host, ps, &hints, &res) != 0 || !res) return -1;
    int s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s < 0) {
        freeaddrinfo(res);
        return -1;
    }
    struct timeval tv = {.tv_sec = 5};
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    int on = 1, idle = 20, intvl = 5, cnt = 3;
    setsockopt(s, SOL_SOCKET, SO_KEEPALIVE, &on, sizeof on);
    setsockopt(s, IPPROTO_TCP, TCP_KEEPIDLE, &idle, sizeof idle);
    setsockopt(s, IPPROTO_TCP, TCP_KEEPINTVL, &intvl, sizeof intvl);
    setsockopt(s, IPPROTO_TCP, TCP_KEEPCNT, &cnt, sizeof cnt);
    int r = connect(s, res->ai_addr, res->ai_addrlen);
    freeaddrinfo(res);
    if (r) {
        close(s);
        return -1;
    }
    uint8_t rnd[16];
    char key[25], req[320];
    esp_fill_random(rnd, sizeof rnd);
    ws_key(rnd, key);
    int n = snprintf(req, sizeof req,
                     "GET %s HTTP/1.1\r\nHost: %s:%d\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
                     "Sec-WebSocket-Key: %s\r\nSec-WebSocket-Version: 13\r\n\r\n",
                     path, host, port, key);
    if (send(s, req, n, 0) != n) {
        close(s);
        return -1;
    }
    // Read the response head; anything after it is the first WebSocket bytes.
    size_t got = 0;
    char *end = NULL;
    while (!end && got < cap - 1) {
        int k = recv(s, rest + got, cap - 1 - got, 0);
        if (k <= 0) break;
        got += k;
        rest[got] = 0;
        end = strstr((char *)rest, "\r\n\r\n");
    }
    if (!end || strncmp((char *)rest, "HTTP/1.1 101", 12)) {
        ESP_LOGW(TAG, "upgrade refused: %.40s", (char *)rest);
        close(s);
        return -1;
    }
    size_t hl = (end + 4) - (char *)rest;
    memmove(rest, rest + hl, got - hl);
    *rest_n = got - hl;
    tv.tv_sec = 1;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    return s;
}

static void set_up(bool up, const char *name)
{
    model_lock();
    g_model.wf_up = up;
    if (name) snprintf(g_model.wf_name, sizeof g_model.wf_name, "%s", name);
    model_unlock();
}

static void wf_task(void *arg)
{
    uint8_t *msg = heap_caps_malloc(MSG_MAX + 1, MALLOC_CAP_SPIRAM);  // +1 for a NUL after text
    uint8_t *rx = heap_caps_malloc(4096, MALLOC_CAP_SPIRAM);
    ws_dec_t *dec = heap_caps_calloc(1, sizeof *dec, MALLOC_CAP_SPIRAM);
    wf_ctx_t *c = heap_caps_calloc(1, sizeof *c, MALLOC_CAP_SPIRAM);
    c->map = heap_caps_calloc(1, sizeof *c->map, MALLOC_CAP_SPIRAM);
    settings_t *s = heap_caps_malloc(sizeof *s, MALLOC_CAP_SPIRAM);
    int backoff = 1, last_state = -1;
    for (;;) {
        settings_get(s);
        uint32_t gen = settings_gen();
        char host[64], path[64];
        int port;
        int state = 0;
        if (!s->wf_url[0]) {
            set_up(false, "");  // unconfigured: no name, no span
            model_lock();
            g_model.wf_lo_hz = g_model.wf_hi_hz = 0;
            model_unlock();
        } else if (!parse_url(s->wf_url, host, sizeof host, &port, path, sizeof path)) {
            if (last_state != 1) ESP_LOGW(TAG, "URL should look like ws://host:port/");
            state = 1;
            set_up(false, s->wf_name);
        } else if (net_up()) {
            set_up(false, s->wf_name);
            size_t rest_n = 0;
            int sock = connect_ws(host, port, path, rx, 4096, &rest_n);
            if (sock >= 0) {
                ESP_LOGI(TAG, "listening to %s:%d%s", host, port, path);
                backoff = 1;
                memset(c, 0, offsetof(wf_ctx_t, map));
                c->sock = sock;
                model_lock();
                uint32_t next = g_model.wf_row_seq;
                model_unlock();
                wf_rows_reset(&c->rows, next);
                ws_dec_init(dec, msg, MSG_MAX);
                set_up(true, NULL);
                g_wf_state = 2;
                if (last_state != 2) last_state = 2;
                int bad = ws_dec_feed(dec, rx, rest_n, on_msg, c);
                while (!bad && !c->closing && settings_gen() == gen) {
                    follow_listen(c, true);
                    int k = recv(sock, rx, 4096, 0);
                    if (k > 0) {
                        bad = ws_dec_feed(dec, rx, k, on_msg, c);
                    } else if (k == 0) {
                        break;
                    } else if (errno != EAGAIN && errno != EWOULDBLOCK) {
                        break;
                    }
                }
                close(sock);
                follow_listen(c, false);  // stop the audio; asked for again after reconnecting
                set_up(false, NULL);
                ESP_LOGW(TAG, "disconnected%s", bad ? " (protocol error)" : "");
            } else {
                ESP_LOGW(TAG, "cannot connect to %s:%d, retry in %d s", host, port, backoff);
            }
            state = 1;
            for (int i = 0; i < backoff * 10 && settings_gen() == gen; i++) vTaskDelay(pdMS_TO_TICKS(100));
            backoff = backoff < 30 ? backoff * 2 : 30;
            g_wf_state = state;
            last_state = state;
            continue;
        } else {
            state = 1;
            set_up(false, s->wf_name);
        }
        if (state != last_state) {
            if (state == 0) ESP_LOGI(TAG, "off (no URL set)");
            last_state = state;
        }
        g_wf_state = state;
        for (int i = 0; i < 20 && settings_gen() == gen; i++) vTaskDelay(pdMS_TO_TICKS(100));
    }
}

void src_waterfall_start(void)
{
    xTaskCreatePinnedToCoreWithCaps(wf_task, "waterfall", 8192, NULL, 5, NULL, 0, MALLOC_CAP_SPIRAM);
}
