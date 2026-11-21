#include "ota.h"

#include <stdio.h>
#include <string.h>

#include "board.h"
#include "esp_app_desc.h"
#include "esp_app_format.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/task.h"
#include "net.h"
#include "setup_screen.h"

static const char *TAG = "ota";

#define HEALTH_MIN_S 30
#define HEALTH_MAX_S 90

volatile bool g_ota_active;
static volatile bool s_web_up;
static esp_ota_handle_t s_handle;
static const esp_partition_t *s_target;
static size_t s_total, s_done;
static int s_last_pct = -1;

void ota_note_web_up(void) {}

// The web server really answers: ask it for /api/version over loopback.
static bool web_answers(void)
{
    esp_http_client_config_t c = {.url = "http://127.0.0.1/api/version", .timeout_ms = 3000};
    esp_http_client_handle_t h = esp_http_client_init(&c);
    if (!h) return false;
    bool ok = esp_http_client_perform(h) == ESP_OK && esp_http_client_get_status_code(h) == 200;
    esp_http_client_cleanup(h);
    return ok;
}

const char *ota_running_slot(void)
{
    const esp_partition_t *p = esp_ota_get_running_partition();
    return p ? p->label : "?";
}

static void health_task(void *arg)
{
    for (;;) {
        uint32_t up = (uint32_t)(esp_timer_get_time() / 1000000);
        bool net = net_mode() == NET_AP || net_up();
        bool frames = board_frames_total() > 10;
        if (!s_web_up && up >= 10) s_web_up = web_answers();
        bool healthy = up >= HEALTH_MIN_S && net && s_web_up && frames;
#ifdef OTA_TEST_NEVER_HEALTHY
        // Rollback test build only: never confirms itself, and crashes 40 s in
        // (a crash dump for /api/crash, then the bootloader rolls back).
        healthy = false;
        if (up >= 40) *(volatile int *)0 = 1;
#endif
        if (healthy) {
            esp_err_t e = esp_ota_mark_app_valid_cancel_rollback();
            ESP_LOGI(TAG, "new image %s is healthy after %lus: kept (%s)", esp_app_get_description()->version,
                     (unsigned long)up, esp_err_to_name(e));
            vTaskDelete(NULL);
        }
        if (up >= HEALTH_MAX_S) {
            ESP_LOGE(TAG, "new image not healthy after %ds (network %d, web %d, frames %d): restarting to roll back",
                     HEALTH_MAX_S, net, s_web_up, frames);
            vTaskDelay(pdMS_TO_TICKS(200));
            esp_restart();
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

void ota_boot_check(void)
{
    const esp_partition_t *run = esp_ota_get_running_partition();
    esp_ota_img_states_t st = ESP_OTA_IMG_UNDEFINED;
    esp_ota_get_state_partition(run, &st);
    ESP_LOGI(TAG, "running %s from %s (%s)", esp_app_get_description()->version, run ? run->label : "?",
             st == ESP_OTA_IMG_PENDING_VERIFY ? "new, on trial" : st == ESP_OTA_IMG_VALID ? "confirmed" : "factory or USB");
    const esp_partition_t *last_bad = esp_ota_get_last_invalid_partition();
    if (last_bad) {
        esp_app_desc_t d;
        if (esp_ota_get_partition_description(last_bad, &d) == ESP_OK)
            ESP_LOGW(TAG, "%s in %s was rolled back", d.version, last_bad->label);
    }
    if (st == ESP_OTA_IMG_PENDING_VERIFY) {
        // Internal stack: confirming the image writes flash, which a task with
        // its stack in PSRAM must not do.
        xTaskCreatePinnedToCore(health_task, "health", 4096, NULL, 3, NULL, 0);
    }
}

const char *ota_begin(const uint8_t *first, size_t n, size_t total)
{
    if (g_ota_active) return "an update is already in progress";
    // The image header names the chip; the app description names the project.
    size_t desc_at = sizeof(esp_image_header_t) + sizeof(esp_image_segment_header_t);
    if (n < desc_at + sizeof(esp_app_desc_t)) return "image too short";
    const esp_image_header_t *ih = (const esp_image_header_t *)first;
    const esp_app_desc_t *d = (const esp_app_desc_t *)(first + desc_at);
    if (ih->magic != ESP_IMAGE_HEADER_MAGIC || d->magic_word != ESP_APP_DESC_MAGIC_WORD) return "not an app image";
    if (ih->chip_id != CONFIG_IDF_FIRMWARE_CHIP_ID) return "image is for another chip";
    if (strncmp(d->project_name, esp_app_get_description()->project_name, sizeof d->project_name))
        return "image is for another project";
    s_target = esp_ota_get_next_update_partition(NULL);
    if (!s_target) return "no update slot";
    if (total > s_target->size) return "image too big for the slot";
    g_ota_active = true;
    esp_err_t e = esp_ota_begin(s_target, OTA_WITH_SEQUENTIAL_WRITES, &s_handle);
    if (e != ESP_OK) {
        g_ota_active = false;
        return "cannot start writing the slot";
    }
    s_total = total;
    s_done = 0;
    s_last_pct = -1;
    ESP_LOGI(TAG, "receiving %s (%u bytes) into %s", d->version, (unsigned)total, s_target->label);
    setup_screen_update(0, d->version);
    return ota_write(first, n);
}

const char *ota_write(const uint8_t *p, size_t n)
{
    if (!g_ota_active) return "no update in progress";
    if (esp_ota_write(s_handle, p, n) != ESP_OK) return "flash write failed";
    s_done += n;
    int pct = s_total ? (int)(s_done * 100 / s_total) : 0;
    if (pct / 5 != s_last_pct / 5) {
        s_last_pct = pct;
        setup_screen_update(pct, NULL);
    }
    return NULL;
}

const char *ota_finish(char *version, size_t vn)
{
    if (!g_ota_active) return "no update in progress";
    esp_err_t e = esp_ota_end(s_handle);  // checks the image (hash, signature, segments)
    if (e != ESP_OK) {
        g_ota_active = false;
        setup_screen_update(-1, NULL);
        return e == ESP_ERR_OTA_VALIDATE_FAILED ? "image failed its checks" : "could not finish the image";
    }
    esp_app_desc_t d;
    if (esp_ota_get_partition_description(s_target, &d) == ESP_OK) snprintf(version, vn, "%s", d.version);
    if (esp_ota_set_boot_partition(s_target) != ESP_OK) {
        g_ota_active = false;
        setup_screen_update(-1, NULL);
        return "could not select the new image";
    }
    ESP_LOGI(TAG, "%s written to %s, restarting into it", version, s_target->label);
    setup_screen_update(100, "restarting");
    return NULL;
}

void ota_abort(void)
{
    if (!g_ota_active) return;
    esp_ota_abort(s_handle);
    g_ota_active = false;
    setup_screen_update(-1, NULL);
    ESP_LOGW(TAG, "update abandoned");
}

bool ota_ready_for_sleep(void)
{
    const esp_partition_t *run = esp_ota_get_running_partition();
    esp_ota_img_states_t st = ESP_OTA_IMG_UNDEFINED;
    esp_ota_get_state_partition(run, &st);
    if (st != ESP_OTA_IMG_PENDING_VERIFY) return true;
    bool net = net_mode() == NET_AP || net_up();
    if (!s_web_up) s_web_up = web_answers();
    if (net && s_web_up && board_frames_total() > 10) {
        ESP_LOGI(TAG, "confirming the new image before sleeping");
        return esp_ota_mark_app_valid_cancel_rollback() == ESP_OK;
    }
    return false;
}
