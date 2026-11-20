// Pings every configured host every 2 s, staggered across the interval, with
// one raw ICMP socket on one task (what esp_ping does inside, without a task
// per host). Names are resolved by a second task so a slow DNS answer never
// holds up the pings.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "freertos/idf_additions.h"
#include "esp_heap_caps.h"
#include "lwip/icmp.h"
#include "lwip/inet_chksum.h"
#include "lwip/ip.h"
#include "lwip/netdb.h"
#include "lwip/sockets.h"
#include "model.h"
#include "net.h"
#include "platform.h"
#include "settings.h"
#include "sources.h"

static const char *TAG = "ping";

#define ROUND_MS 2000
#define TIMEOUT_MS 1500
#define ICMP_ID 0x4B4E

typedef struct {
    char addr[48];
    uint32_t ip;          // network order, 0 = not resolved
    uint32_t resolved_ms;
    uint16_t seq;
    bool waiting;
    int64_t sent_us;
} target_t;

static target_t s_t[MODEL_MAX_HOSTS];
static int s_n;
static SemaphoreHandle_t s_mux;
static uint32_t s_gen = 0xFFFFFFFF;
static set_host_t s_cfg[SET_MAX_HOSTS];
static int s_cfg_n = -1;

static bool is_ip(const char *a, uint32_t *ip)
{
    struct in_addr in;
    if (inet_aton(a, &in)) {
        *ip = in.s_addr;
        return true;
    }
    return false;
}

// Picks up settings changes: rebuilds the host table in the model if the
// host list changed (otherwise the history is kept).
static void reload(void)
{
    settings_t *s = malloc(sizeof *s);
    if (!s) return;
    settings_get(s);
    if (s->n_hosts == s_cfg_n && !memcmp(s->hosts, s_cfg, sizeof s_cfg)) {
        free(s);
        return;
    }
    s_cfg_n = s->n_hosts;
    memcpy(s_cfg, s->hosts, sizeof s_cfg);
    xSemaphoreTake(s_mux, portMAX_DELAY);
    model_lock();
    s_n = s->n_hosts < MODEL_MAX_HOSTS ? s->n_hosts : MODEL_MAX_HOSTS;
    memset(g_model.hosts, 0, sizeof g_model.hosts);
    memset(s_t, 0, sizeof s_t);
    for (int i = 0; i < s_n; i++) {
        strlcpy(g_model.hosts[i].name, s->hosts[i].name, sizeof g_model.hosts[i].name);
        strlcpy(g_model.hosts[i].addr, s->hosts[i].addr, sizeof g_model.hosts[i].addr);
        strlcpy(s_t[i].addr, s->hosts[i].addr, sizeof s_t[i].addr);
        is_ip(s_t[i].addr, &s_t[i].ip);
    }
    g_model.n_hosts = s_n;
    model_unlock();
    xSemaphoreGive(s_mux);
    free(s);
}

static void resolve_task(void *arg)
{
    for (;;) {
        if (net_up()) {
            for (int i = 0; i < MODEL_MAX_HOSTS; i++) {
                char addr[48];
                uint32_t ip = 0, when = 0;
                xSemaphoreTake(s_mux, portMAX_DELAY);
                bool have = i < s_n;
                if (have) {
                    memcpy(addr, s_t[i].addr, sizeof addr);
                    ip = s_t[i].ip;
                    when = s_t[i].resolved_ms;
                }
                xSemaphoreGive(s_mux);
                if (!have || is_ip(addr, &ip)) continue;
                uint32_t now = model_now_ms();
                // Re-resolve every 10 minutes, or every 30 s while unresolved.
                if (ip && now - when < 600000) continue;
                if (!ip && when && now - when < 30000) continue;
                struct addrinfo hints = {.ai_family = AF_INET, .ai_socktype = SOCK_RAW}, *res = NULL;
                uint32_t got = 0;
                if (getaddrinfo(addr, NULL, &hints, &res) == 0 && res) {
                    got = ((struct sockaddr_in *)res->ai_addr)->sin_addr.s_addr;
                    freeaddrinfo(res);
                } else {
                    ESP_LOGW(TAG, "cannot resolve %s", addr);
                }
                xSemaphoreTake(s_mux, portMAX_DELAY);
                if (i < s_n && !strcmp(s_t[i].addr, addr)) {
                    if (got || !s_t[i].ip) s_t[i].ip = got;  // keep the old address if a re-resolve fails
                    s_t[i].resolved_ms = model_now_ms();
                }
                xSemaphoreGive(s_mux);
            }
        }
        vTaskDelay(pdMS_TO_TICKS(2000));
    }
}

static void record(int i, float rtt)
{
    model_lock();
    if (i < g_model.n_hosts) model_host_sample(&g_model.hosts[i], rtt, model_now_ms());
    model_unlock();
}

static void ping_task(void *arg)
{
    int sock = -1;
    uint16_t seq = 1;
    int next = 0;
    int64_t next_us = esp_timer_get_time();
    for (;;) {
        if (settings_gen() != s_gen) {
            s_gen = settings_gen();
            reload();
        }
        if (sock < 0) {
            sock = socket(AF_INET, SOCK_RAW, IP_PROTO_ICMP);
            if (sock < 0) {
                ESP_LOGE(TAG, "raw socket failed");
                vTaskDelay(pdMS_TO_TICKS(5000));
                continue;
            }
        }
        int64_t now = esp_timer_get_time();

        // Time out anything that has waited too long.
        for (int i = 0; i < s_n; i++) {
            if (s_t[i].waiting && now - s_t[i].sent_us > TIMEOUT_MS * 1000LL) {
                s_t[i].waiting = false;
                record(i, -1);
            }
        }

        // Send the next one when its slot comes round.
        if (now >= next_us && s_n > 0) {
            int i = next % s_n;
            next = (next + 1) % s_n;
            next_us += ROUND_MS * 1000LL / s_n;
            if (next_us < now - ROUND_MS * 1000LL) next_us = now;  // fell far behind: resync
            xSemaphoreTake(s_mux, portMAX_DELAY);
            uint32_t ip = s_t[i].ip;
            xSemaphoreGive(s_mux);
            if (s_t[i].waiting) {
                // Still waiting for the last one: count it lost now.
                s_t[i].waiting = false;
                record(i, -1);
            }
            if (!net_up()) {
                // No network: leave the history alone rather than filling it with losses.
            } else if (!ip) {
                record(i, -1);
            } else {
                struct {
                    struct icmp_echo_hdr h;
                    uint8_t data[24];
                } pkt;
                memset(&pkt, 0, sizeof pkt);
                ICMPH_TYPE_SET(&pkt.h, ICMP_ECHO);
                pkt.h.id = htons(ICMP_ID);
                pkt.h.seqno = htons(seq);
                memcpy(pkt.data, "desk-knob ping", 14);
                pkt.h.chksum = inet_chksum(&pkt, sizeof pkt);
                struct sockaddr_in to = {.sin_family = AF_INET, .sin_addr.s_addr = ip};
                s_t[i].seq = seq++;
                if (seq == 0) seq = 1;
                s_t[i].sent_us = esp_timer_get_time();
                s_t[i].waiting = sendto(sock, &pkt, sizeof pkt, 0, (struct sockaddr *)&to, sizeof to) > 0;
                if (!s_t[i].waiting) record(i, -1);
            }
        }

        // Wait for replies until the next send is due.
        int64_t wait_us = next_us - esp_timer_get_time();
        if (wait_us < 1000) wait_us = 1000;
        if (wait_us > 100000) wait_us = 100000;
        fd_set rf;
        FD_ZERO(&rf);
        FD_SET(sock, &rf);
        struct timeval tv = {.tv_sec = 0, .tv_usec = (long)wait_us};
        if (select(sock + 1, &rf, NULL, NULL, &tv) > 0) {
            uint8_t buf[128];
            struct sockaddr_in from;
            socklen_t fl = sizeof from;
            int r = recvfrom(sock, buf, sizeof buf, 0, (struct sockaddr *)&from, &fl);
            int64_t t = esp_timer_get_time();
            if (r >= (int)(sizeof(struct ip_hdr) + sizeof(struct icmp_echo_hdr))) {
                struct ip_hdr *iph = (struct ip_hdr *)buf;
                int hl = IPH_HL(iph) * 4;
                if (r >= hl + (int)sizeof(struct icmp_echo_hdr)) {
                    struct icmp_echo_hdr *e = (struct icmp_echo_hdr *)(buf + hl);
                    if (ICMPH_TYPE(e) == ICMP_ER && ntohs(e->id) == ICMP_ID) {
                        uint16_t sq = ntohs(e->seqno);
                        for (int i = 0; i < s_n; i++) {
                            if (s_t[i].waiting && s_t[i].seq == sq) {
                                s_t[i].waiting = false;
                                record(i, (t - s_t[i].sent_us) / 1000.0f);
                                break;
                            }
                        }
                    }
                }
            }
        }
    }
}

void src_ping_start(void)
{
    s_mux = xSemaphoreCreateMutex();
    reload();
    s_gen = settings_gen();
    xTaskCreatePinnedToCoreWithCaps(ping_task, "ping", 4096, NULL, 6, NULL, 0, MALLOC_CAP_SPIRAM);
    xTaskCreatePinnedToCoreWithCaps(resolve_task, "resolve", 4096, NULL, 3, NULL, 0, MALLOC_CAP_SPIRAM);
}
