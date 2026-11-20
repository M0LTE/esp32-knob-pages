// Listen-only KISS tap. The far end is a live amateur station: anything
// written to this socket would be transmitted, so this file never writes to
// it. There is deliberately no send() or write() call here.
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/idf_additions.h"
#include "esp_heap_caps.h"
#include "kiss_parse.h"
#include "lwip/netdb.h"
#include "lwip/sockets.h"
#include "model.h"
#include "net.h"
#include "platform.h"
#include "settings.h"
#include "sources.h"

static const char *TAG = "kiss";

volatile uint32_t g_kiss_frames, g_kiss_bytes;
volatile int g_kiss_connects;

typedef struct {
    char own[20];
} kctx_t;

static void on_frame(void *ctx, const uint8_t *f, size_t n)
{
    kctx_t *k = ctx;
    kiss_frame_t fr = {0};
    if (!kiss_frame_decode(f, n, k->own, &fr)) return;
    fr.t_ms = model_now_ms();
    fr.wall = model_wall();
    g_kiss_frames++;
    model_lock();
    model_kiss_push(&fr);
    model_unlock();
}

static void set_up(bool up, const char *name)
{
    model_lock();
    if (name) snprintf(g_model.kiss_name, sizeof g_model.kiss_name, "%s", name);
    if (up && !g_model.kiss_up) g_model.kiss_since_ms = model_now_ms();
    if (!up && g_model.kiss_up) g_model.kiss_since_ms = model_now_ms();
    g_model.kiss_up = up;
    model_unlock();
}

static int connect_to(const char *host, int port)
{
    char ps[8];
    snprintf(ps, sizeof ps, "%d", port);
    struct addrinfo hints = {.ai_family = AF_INET, .ai_socktype = SOCK_STREAM}, *res = NULL;
    if (getaddrinfo(host, ps, &hints, &res) != 0 || !res) return -1;
    int s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s < 0) {
        freeaddrinfo(res);
        return -1;
    }
    // Bounded connect, then keepalive so a dead peer is noticed. Keepalive
    // probes carry no data.
    struct timeval tv = {.tv_sec = 5};
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
    int on = 1, idle = 30, intvl = 10, cnt = 3;
    setsockopt(s, SOL_SOCKET, SO_KEEPALIVE, &on, sizeof on);
    setsockopt(s, IPPROTO_TCP, TCP_KEEPIDLE, &idle, sizeof idle);
    setsockopt(s, IPPROTO_TCP, TCP_KEEPINTVL, &intvl, sizeof intvl);
    setsockopt(s, IPPROTO_TCP, TCP_KEEPCNT, &cnt, sizeof cnt);
    int r = connect(s, res->ai_addr, res->ai_addrlen);
    freeaddrinfo(res);
    if (r != 0) {
        close(s);
        return -1;
    }
    tv.tv_sec = 1;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    return s;
}

static void kiss_task(void *arg)
{
    kiss_dec_t *dec = malloc(sizeof *dec);
    // Settings are big: keep both copies in PSRAM rather than on the stack.
    settings_t *cur_p = heap_caps_malloc(sizeof *cur_p, MALLOC_CAP_SPIRAM);
    settings_t *new_p = heap_caps_malloc(sizeof *new_p, MALLOC_CAP_SPIRAM);
#define s (*cur_p)
#define n (*new_p)
    kctx_t ctx;
    int backoff = 1;
    uint8_t buf[512];
    for (;;) {
        settings_get(&s);
        uint32_t gen = settings_gen();
        snprintf(ctx.own, sizeof ctx.own, "%s", s.kiss_name);
        set_up(false, s.kiss_name);
        if (!s.kiss_host[0] || s.kiss_port <= 0 || !net_up()) {
            vTaskDelay(pdMS_TO_TICKS(2000));
            continue;
        }
        int sock = connect_to(s.kiss_host, s.kiss_port);
        if (sock < 0) {
            ESP_LOGW(TAG, "cannot connect to %s:%d, retry in %d s", s.kiss_host, s.kiss_port, backoff);
            for (int i = 0; i < backoff * 10 && settings_gen() == gen; i++) vTaskDelay(pdMS_TO_TICKS(100));
            backoff = backoff < 30 ? backoff * 2 : 60;
            continue;
        }
        ESP_LOGI(TAG, "listening to %s:%d", s.kiss_host, s.kiss_port);
        g_kiss_connects++;
        backoff = 1;
        kiss_dec_init(dec);
        set_up(true, NULL);
        for (;;) {
            int r = recv(sock, buf, sizeof buf, 0);
            if (r > 0) {
                g_kiss_bytes += r;
                kiss_dec_feed(dec, buf, r, on_frame, &ctx);
            } else if (r == 0) {
                ESP_LOGW(TAG, "closed by the far end");
                break;
            } else if (errno != EAGAIN && errno != EWOULDBLOCK) {
                ESP_LOGW(TAG, "receive error %d", errno);
                break;
            }
            if (settings_gen() != gen) {
                settings_get(&n);
                gen = settings_gen();
                if (strcmp(n.kiss_host, s.kiss_host) || n.kiss_port != s.kiss_port || strcmp(n.kiss_name, s.kiss_name)) {
                    ESP_LOGI(TAG, "settings changed, reconnecting");
                    break;
                }
            }
        }
        close(sock);
        set_up(false, NULL);
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

#undef s
#undef n

void src_kiss_start(void)
{
    xTaskCreatePinnedToCoreWithCaps(kiss_task, "kiss", 8192, NULL, 5, NULL, 0, MALLOC_CAP_SPIRAM);
}
