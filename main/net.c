#include "net.h"

#include <string.h>
#include <time.h>

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_netif_sntp.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "freertos/idf_additions.h"
#include "esp_heap_caps.h"
#include "lwip/sockets.h"

static const char *TAG = "net";

#define BIT_GOT_IP BIT0

static EventGroupHandle_t s_ev;
static esp_netif_t *s_sta, *s_ap;
static char s_ip[16];
static net_mode_t s_mode = NET_STA;
static volatile bool s_up;
static int s_retry;

bool net_up(void) { return s_up; }
const char *net_ip(void) { return s_ip; }
net_mode_t net_mode(void) { return s_mode; }

int net_rssi(void)
{
    wifi_ap_record_t ap;
    if (!s_up || esp_wifi_sta_get_ap_info(&ap) != ESP_OK) return 0;
    return ap.rssi;
}

static void retry_cb(void *arg) { esp_wifi_connect(); }

static void on_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    static esp_timer_handle_t retry_timer;
    if (!retry_timer) {
        const esp_timer_create_args_t t = {.callback = retry_cb, .name = "wifi_retry"};
        esp_timer_create(&t, &retry_timer);
    }
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        wifi_event_sta_disconnected_t *d = data;
        if (s_up) ESP_LOGW(TAG, "wifi lost, reason %d", d->reason);
        s_up = false;
        s_ip[0] = 0;
        xEventGroupClearBits(s_ev, BIT_GOT_IP);
        // Back off: quick at first, then every 30 s (scanning disturbs the set-up AP).
        int ms = s_mode == NET_AP ? 60000 : (s_retry < 5 ? 1000 : s_retry < 20 ? 5000 : 30000);
        s_retry++;
        esp_timer_stop(retry_timer);
        esp_timer_start_once(retry_timer, (uint64_t)ms * 1000);
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *e = data;
        snprintf(s_ip, sizeof s_ip, IPSTR, IP2STR(&e->ip_info.ip));
        ESP_LOGI(TAG, "wifi up, address %s", s_ip);
        s_up = true;
        s_retry = 0;
        xEventGroupSetBits(s_ev, BIT_GOT_IP);
        if (s_mode == NET_AP) {
            // The set-up AP came up because the network was missing at boot and it is
            // back now. Restart into normal running unless someone is using the AP.
            wifi_sta_list_t l;
            if (esp_wifi_ap_get_sta_list(&l) == ESP_OK && l.num == 0) {
                ESP_LOGI(TAG, "network is back and nobody is on the set-up AP, restarting");
                esp_restart();
            }
        }
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_AP_STACONNECTED) {
        ESP_LOGI(TAG, "a device joined the set-up AP");
    }
}

// Answers every DNS query with the AP's address so phones show the set-up page.
static void dns_task(void *arg)
{
    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    struct sockaddr_in a = {.sin_family = AF_INET, .sin_port = htons(53), .sin_addr.s_addr = htonl(INADDR_ANY)};
    if (sock < 0 || bind(sock, (struct sockaddr *)&a, sizeof a) < 0) {
        ESP_LOGE(TAG, "dns bind failed");
        vTaskDelete(NULL);
    }
    uint8_t buf[512];
    for (;;) {
        struct sockaddr_in from;
        socklen_t fl = sizeof from;
        int n = recvfrom(sock, buf, sizeof buf - 16, 0, (struct sockaddr *)&from, &fl);
        if (n < 12 || (buf[2] & 0x80)) continue;            // too short, or a response
        if (buf[4] != 0 || buf[5] != 1) continue;           // exactly one question
        int p = 12;
        while (p < n && buf[p]) p += buf[p] + 1;            // skip the name
        if (p + 5 > n) continue;
        uint16_t qtype = (buf[p + 1] << 8) | buf[p + 2];
        p += 5;                                              // end of question
        buf[2] = 0x84 | (buf[2] & 0x01);                     // response, authoritative, keep RD
        buf[3] = 0x80;                                       // RA, no error
        buf[6] = 0; buf[8] = 0; buf[9] = 0; buf[10] = 0; buf[11] = 0;
        if (qtype == 1) {
            buf[7] = 1;
            uint8_t ans[16] = {0xC0, 0x0C, 0, 1, 0, 1, 0, 0, 0, 60, 0, 4, 192, 168, 4, 1};
            memcpy(buf + p, ans, 16);
            p += 16;
        } else {
            buf[7] = 0;                                      // no AAAA etc.
        }
        sendto(sock, buf, p, 0, (struct sockaddr *)&from, fl);
    }
}

static void time_sync_cb(struct timeval *tv)
{
    time_t t = tv->tv_sec;
    struct tm lt;
    localtime_r(&t, &lt);
    char b[32];
    strftime(b, sizeof b, "%Y-%m-%d %H:%M:%S %Z", &lt);
    ESP_LOGI(TAG, "time set: %s", b);
}

static void sntp_start(void)
{
    esp_sntp_config_t c = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
    c.server_from_dhcp = true;
    c.renew_servers_after_new_IP = true;
    c.index_of_first_server = 1;
    c.ip_event_to_renew = IP_EVENT_STA_GOT_IP;
    c.sync_cb = time_sync_cb;
    esp_netif_sntp_init(&c);
}

static void start_ap(const settings_t *s)
{
    s_mode = NET_AP;
    wifi_config_t ap = {0};
    snprintf((char *)ap.ap.ssid, sizeof ap.ap.ssid, "%s", SETUP_AP_SSID);
    ap.ap.ssid_len = strlen(SETUP_AP_SSID);
    snprintf((char *)ap.ap.password, sizeof ap.ap.password, "%s", settings_ap_pass());
    ap.ap.authmode = WIFI_AUTH_WPA2_PSK;
    ap.ap.max_connection = 4;
    ap.ap.channel = 6;
    ESP_ERROR_CHECK(esp_wifi_set_mode(s->ssid[0] ? WIFI_MODE_APSTA : WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap));
    // Hand out our own address as the DNS server, so every name leads here.
    esp_netif_dns_info_t dns = {.ip.type = ESP_IPADDR_TYPE_V4};
    dns.ip.u_addr.ip4.addr = ESP_IP4TOADDR(192, 168, 4, 1);
    uint8_t offer = 1;
    esp_netif_dhcps_stop(s_ap);
    esp_netif_set_dns_info(s_ap, ESP_NETIF_DNS_MAIN, &dns);
    esp_netif_dhcps_option(s_ap, ESP_NETIF_OP_SET, ESP_NETIF_DOMAIN_NAME_SERVER, &offer, sizeof offer);
    esp_netif_dhcps_start(s_ap);
    xTaskCreatePinnedToCoreWithCaps(dns_task, "dns", 3072, NULL, 4, NULL, 0, MALLOC_CAP_SPIRAM);
    ESP_LOGI(TAG, "set-up AP %s up at %s, password %s", SETUP_AP_SSID, SETUP_AP_IP, settings_ap_pass());
}

net_mode_t net_start(const settings_t *s, int join_ms)
{
    s_ev = xEventGroupCreate();
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    setenv("TZ", "GMT0BST,M3.5.0/1,M10.5.0", 1);  // Europe/London
    tzset();
    s_sta = esp_netif_create_default_wifi_sta();
    s_ap = esp_netif_create_default_wifi_ap();
    esp_netif_set_hostname(s_sta, s->hostname[0] ? s->hostname : "desk-knob");

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_event, NULL);
    esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_event, NULL);

    if (!s->ssid[0]) {
        ESP_LOGI(TAG, "no Wi-Fi credentials");
        start_ap(s);
        ESP_ERROR_CHECK(esp_wifi_start());
        return s_mode;
    }

    wifi_config_t sta = {0};
    memcpy(sta.sta.ssid, s->ssid, strnlen(s->ssid, sizeof sta.sta.ssid));
    memcpy(sta.sta.password, s->pass, strnlen(s->pass, sizeof sta.sta.password));
    sta.sta.threshold.authmode = s->pass[0] ? WIFI_AUTH_WPA_PSK : WIFI_AUTH_OPEN;
    sta.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;
    sta.sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL;
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &sta));
    ESP_ERROR_CHECK(esp_wifi_start());
    esp_wifi_set_ps(WIFI_PS_NONE);  // steady ping times
    sntp_start();
    ESP_LOGI(TAG, "joining \"%s\" as %s", s->ssid, s->hostname);

    EventBits_t b = xEventGroupWaitBits(s_ev, BIT_GOT_IP, pdFALSE, pdTRUE, pdMS_TO_TICKS(join_ms));
    if (b & BIT_GOT_IP) return NET_STA;
    ESP_LOGW(TAG, "could not join \"%s\" in %d s", s->ssid, join_ms / 1000);
    start_ap(s);
    return s_mode;
}
