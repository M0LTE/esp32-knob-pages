// Desk status display for the Guition JC3636K718 round knob.
// Start-up: settings, board, Wi-Fi (or the set-up AP), web page, then the
// pages and their data sources.
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "audio.h"
#include "board.h"
#include "esp_app_desc.h"
#include "cJSON.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_lvgl_port.h"
#include "esp_psram.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "haptic.h"
#include "model.h"
#include "net.h"
#include "ota.h"
#include "platform.h"
#include "settings.h"
#include "setup_screen.h"
#include "sources.h"
#include "ui.h"
#include "web.h"

static const char *TAG = "desk";

#define STATUS_MS 30000
#define JOIN_MS 30000

static void backlight_fade_in(void)
{
    for (int i = 0; i <= 100; i += 4) {
        board_backlight(i);
        vTaskDelay(pdMS_TO_TICKS(15));
    }
}


static void status_line(void)
{
    static int64_t last_us;
    static uint32_t last_rows, last_busy;
    int64_t now_us = esp_timer_get_time();
    float secs = last_us ? (now_us - last_us) / 1e6f : (float)(STATUS_MS / 1000);
    last_us = now_us;
    uint32_t wf_rows = g_wf_rows - last_rows, wf_busy = g_wf_busy_us - last_busy;
    last_rows = g_wf_rows;
    last_busy = g_wf_busy_us;
    char audio_note[160] = "";
    if (!isnan(audio_stream_dbfs())) {
        uint32_t fill, under, trims;
        float corr;
        audio_stream_stats(&fill, &under, &trims, &corr);
        snprintf(audio_note, sizeof audio_note,
                 ", audio %s %.0f dBFS buffer %lu ms rate %+.0f ppm underruns %lu trims %lu gap %lu ms blocks %lu",
                 audio_stream_playing() ? "playing" : "waiting", audio_stream_dbfs(), (unsigned long)fill, corr * 1e6,
                 (unsigned long)under, (unsigned long)trims, (unsigned long)g_wf_audio_gap_ms,
                 (unsigned long)g_wf_audio_block);
        g_wf_audio_gap_ms = 0;
    }
    disp_perf_t p;
    board_disp_perf(&p);
    int up = 0, n, nst;
    bool ku, mu;
    model_lock();
    n = g_model.n_hosts;
    for (int i = 0; i < n; i++) up += g_model.hosts[i].state == HOST_UP;
    ku = g_model.kiss_up;
    mu = g_model.modem_up;
    nst = g_model.n_mstations;
    bool cu = g_model.climate_up;
    int nac = g_model.n_ac;
    model_unlock();
    ESP_LOGI(TAG,
             "status: %s up %lus, heap %uK int (min %uK) %uK psram, fps %.1f, frame %.1f ms avg %.1f max, bus wait %.1f ms, "
             "wifi %s %d dBm, hosts %d/%d up, kiss %s %lu frames, metrics %s %lu ok %lu fail %d stations, energy %s, rooms %s %d, aircraft %s %d, waterfall %s %.1f rows/s %lu tags cpu %.1f%%%s",
             esp_app_get_description()->version, (unsigned long)(esp_timer_get_time() / 1000000),
             (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024),
             (unsigned)(heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL) / 1024),
             (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024), p.fps, p.frame_ms_avg, p.frame_ms_max,
             p.wait_ms_avg, net_up() ? net_ip() : "down", net_rssi(), up, n, ku ? "up" : "down",
             (unsigned long)g_kiss_frames, mu ? "up" : "down", (unsigned long)g_metrics_ok,
             (unsigned long)g_metrics_fail, nst,
             g_energy_state == 2 ? "up" : g_energy_state == 1 ? "failing" : "not configured",
             cu ? "up" : "down", g_rooms_n,
             g_adsb_state == 2 ? "up" : g_adsb_state == 1 ? "failing" : "off", nac,
             g_wf_state == 2 ? "up" : g_wf_state == 1 ? "down" : "off", wf_rows / secs, (unsigned long)g_wf_tags,
             wf_busy / 10000.0f / secs, audio_note);
}

// cJSON's many small allocations (settings, rooms, aircraft) would
// otherwise all land in internal RAM.
static void *json_malloc(size_t n) { return heap_caps_malloc_prefer(n, 2, MALLOC_CAP_SPIRAM, MALLOC_CAP_INTERNAL); }

void app_main(void)
{
    cJSON_Hooks hooks = {.malloc_fn = json_malloc, .free_fn = free};
    cJSON_InitHooks(&hooks);
    platform_early_init();
    // Give USB a few seconds before touching any hardware, so esptool can
    // always get in even if something below crashes.
    for (int i = 5; i > 0; i--) {
        ESP_LOGI(TAG, "desk knob %s starting in %d, PSRAM %u KB, reset reason %d",
                 esp_app_get_description()->version, i, (unsigned)(esp_psram_get_size() / 1024),
                 (int)esp_reset_reason());
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
    settings_init();
    ota_boot_check();
    board_init();
    haptic_init();
    audio_init();
    platform_init();
    board_set_knob_cb(platform_knob);
    ESP_LOGI(TAG, "hardware up, heap %uK internal (largest %uK), %uK psram",
             (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024),
             (unsigned)(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL) / 1024),
             (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024));

    settings_t *sp = heap_caps_malloc(sizeof *sp, MALLOC_CAP_SPIRAM);  // a few KB: not in internal RAM
    settings_get(sp);
#define s (*sp)
#ifdef DESK_BENCH
    {
        extern void bench_start(void);
        ESP_LOGW(TAG, "BENCH BUILD: no network, made-up data");
        backlight_fade_in();
        static ui_page_cfg_t bp[] = {{"hosts", "Network"}, {"station", "Station"}, {"modem", "Modem"}};
        bench_start();
        setup_screen_clear();
        lvgl_port_lock(0);
        ui_init(bp, 3);
        lvgl_port_unlock();
        platform_page_changed(0, ui_page_count());
        for (int k = 0;; k++) {
            vTaskDelay(pdMS_TO_TICKS(5000));
            if (k % 3 == 2) ui_knob(1);  // visit every page
            status_line();
        }
    }
#endif
    if (s.ssid[0]) setup_screen_joining(s.ssid);
    backlight_fade_in();

    net_mode_t mode = net_start(&s, JOIN_MS);
    web_start();

    if (mode == NET_AP) {
        setup_screen_ap(SETUP_AP_SSID, settings_ap_pass(), SETUP_AP_IP,
                        s.ssid[0] ? "Could not join the saved network" : "No Wi-Fi network set yet");
        for (;;) {
            vTaskDelay(pdMS_TO_TICKS(STATUS_MS));
            ESP_LOGI(TAG, "status: set-up AP %s, password %s, heap %uK int", SETUP_AP_SSID, settings_ap_pass(),
                     (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024));
        }
    }

    setup_screen_joined(net_ip(), s.hostname);
    vTaskDelay(pdMS_TO_TICKS(2500));

    static ui_page_cfg_t pages[SET_MAX_PAGES];
    for (int i = 0; i < s.n_pages; i++) {
        pages[i].type = s.pages[i].type;
        pages[i].title = s.pages[i].title[0] ? s.pages[i].title : NULL;
    }
#ifdef WF_TEST_FIRST
    // Bench check only (-DWF_TEST_FIRST=1): show the waterfall page first.
    {
        int at = -1, n = s.n_pages;
        for (int i = 0; i < n; i++)
            if (!strcmp(s.pages[i].type, "waterfall")) at = i;
        if (at < 0 && n < SET_MAX_PAGES) {
            snprintf(s.pages[n].type, sizeof s.pages[n].type, "waterfall");
            snprintf(s.pages[n].title, sizeof s.pages[n].title, "Waterfall");
            at = n++;
            s.n_pages = n;
        }
        if (at > 0) {
            set_page_t w = s.pages[at];
            memmove(&s.pages[1], &s.pages[0], sizeof(set_page_t) * at);
            s.pages[0] = w;
        }
        for (int i = 0; i < s.n_pages; i++) {
            pages[i].type = s.pages[i].type;
            pages[i].title = s.pages[i].title[0] ? s.pages[i].title : NULL;
        }
    }
#endif
    model_lock();
    snprintf(g_model.kiss_name, sizeof g_model.kiss_name, "%s", s.kiss_name);
    snprintf(g_model.modem_name, sizeof g_model.modem_name, "%s", s.metrics_name);
    model_unlock();
    src_ping_start();
    src_kiss_start();
    src_metrics_start();
    src_energy_start();
    src_rooms_start();
    src_adsb_start();
    src_waterfall_start();

    setup_screen_clear();
    lvgl_port_lock(0);
    ui_init(pages, s.n_pages);
    lvgl_port_unlock();
    platform_page_changed(0, ui_page_count());
    ESP_LOGI(TAG, "running, %d pages, heap %uK internal (min %uK)", ui_page_count(),
             (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024),
             (unsigned)(heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL) / 1024));

    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(STATUS_MS));
        status_line();
    }
}
