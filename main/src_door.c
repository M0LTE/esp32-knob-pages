// Doorbell: rings from a Home Assistant binary sensor (polled every second,
// off -> on is a ring), and camera frames from Home Assistant's camera proxy
// (JPEG, resized by HA), decoded to RGB565 in the display's byte order while
// the page wants video. Frames are only ever held in the knob's RAM.
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "energy_parse.h"
#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_jpeg_dec.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/task.h"
#include "model.h"
#include "net.h"
#include "ota.h"
#include "platform.h"
#include "settings.h"
#include "sources.h"

static const char *TAG = "door";

#define POLL_MS 1000
#define FRAME_W 360          // asked of Home Assistant (the stream is 4:3)
#define FRAME_H 270
#define MAX_W 640            // largest frame decoded
#define MAX_H 480
#define JPEG_MAX (256 * 1024)

volatile int g_door_state;           // 0 off, 1 down, 2 up
volatile bool g_door_test_ring;      // set by POST /api/test/ring
volatile uint32_t g_door_frames;

// The JPEG library takes its working memory (about 10 KB) with
// heap_caps_calloc_prefer(), internal RAM first. Internal RAM is the knob's
// scarce resource, so calls from the doorbell task are steered to PSRAM; all
// other callers get the preference order they asked for.
static TaskHandle_t s_door_task;

void *__real_heap_caps_aligned_calloc(size_t alignment, size_t n, size_t size, uint32_t caps);

// Same for its aligned allocations (internal-only requests are honoured).
IRAM_ATTR void *__wrap_heap_caps_aligned_calloc(size_t alignment, size_t n, size_t size, uint32_t caps)
{
    if (s_door_task && xTaskGetCurrentTaskHandle() == s_door_task && !(caps & (MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL))) {
        void *p = __real_heap_caps_aligned_calloc(alignment, n, size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (p) return p;
    }
    return __real_heap_caps_aligned_calloc(alignment, n, size, caps);
}

IRAM_ATTR void *__wrap_heap_caps_calloc_prefer(size_t n, size_t size, size_t num, ...)
{
    if (s_door_task && xTaskGetCurrentTaskHandle() == s_door_task) {
        void *p = heap_caps_calloc(n, size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (p) return p;
    }
    va_list ap;
    va_start(ap, num);
    void *p = NULL;
    for (size_t i = 0; i < num && !p; i++) p = heap_caps_calloc(n, size, va_arg(ap, uint32_t));
    va_end(ap);
    return p;
}

typedef struct {
    char *p;
    size_t n, cap;
    bool overflow;
} body_t;

static esp_err_t on_http(esp_http_client_event_t *e)
{
    body_t *b = e->user_data;
    if (e->event_id == HTTP_EVENT_ON_DATA && b) {
        if (b->n + e->data_len + 1 > b->cap) {
            size_t cap = b->cap * 2;
            while (cap < b->n + e->data_len + 1) cap *= 2;
            char *np = cap <= JPEG_MAX ? heap_caps_realloc(b->p, cap, MALLOC_CAP_SPIRAM) : NULL;
            if (!np) {
                b->overflow = true;
                return ESP_OK;
            }
            b->p = np;
            b->cap = cap;
        }
        memcpy(b->p + b->n, e->data, e->data_len);
        b->n += e->data_len;
        b->p[b->n] = 0;
    }
    return ESP_OK;
}

// A kept-alive client; GET url into b. Returns the HTTP status or -1.
static int get(esp_http_client_handle_t *h, const char *url, const char *auth, body_t *b)
{
    b->n = 0;
    b->overflow = false;
    if (b->p) b->p[0] = 0;
    if (!*h) {
        esp_http_client_config_t c = {
            .url = url, .timeout_ms = 5000, .event_handler = on_http, .user_data = b,
            .buffer_size = 4096, .keep_alive_enable = true,
        };
        *h = esp_http_client_init(&c);
        if (!*h) return -1;
    } else {
        esp_http_client_set_url(*h, url);
    }
    esp_http_client_set_user_data(*h, b);
    esp_http_client_set_header(*h, "Authorization", auth);
    esp_err_t err = esp_http_client_perform(*h);
    int st = esp_http_client_get_status_code(*h);
    if (err != ESP_OK || b->overflow) {
        esp_http_client_cleanup(*h);
        *h = NULL;
        return -1;
    }
    return st;
}

static void close_client(esp_http_client_handle_t *h)
{
    if (*h) esp_http_client_cleanup(*h);
    *h = NULL;
}

// Decodes one JPEG into out (MAX_W x MAX_H room). Sets w, h. False on failure.
static bool decode(const uint8_t *jpg, size_t n, uint16_t *out, uint16_t *w, uint16_t *h)
{
    jpeg_dec_config_t cfg = DEFAULT_JPEG_DEC_CONFIG();
    cfg.output_type = JPEG_PIXEL_FORMAT_RGB565_BE;  // the panel's byte order (RGB565_SWAPPED)
    jpeg_dec_handle_t d = NULL;
    if (jpeg_dec_open(&cfg, &d) != JPEG_ERR_OK) return false;
    jpeg_dec_io_t io = {.inbuf = (uint8_t *)jpg, .inbuf_len = (int)n};
    jpeg_dec_header_info_t info;
    bool ok = jpeg_dec_parse_header(d, &io, &info) == JPEG_ERR_OK && info.width <= MAX_W && info.height <= MAX_H &&
              info.width && info.height;
    if (ok) {
        io.outbuf = (uint8_t *)out;
        ok = jpeg_dec_process(d, &io) == JPEG_ERR_OK;
        *w = info.width;
        *h = info.height;
    }
    jpeg_dec_close(d);
    return ok;
}

static void set_up(bool up, const char *name)
{
    model_lock();
    g_model.door_up = up;
    if (name) snprintf(g_model.door_name, sizeof g_model.door_name, "%s", name);
    model_unlock();
}

static void ring(const char *why)
{
    time_t t = time(NULL);
    model_lock();
    g_model.door_ring_seq++;
    g_model.door_ring_wall = t > 1700000000 ? (uint32_t)t : 0;
    model_unlock();
    ESP_LOGI(TAG, "ring (%s)", why);
}

static void door_task(void *arg)
{
    s_door_task = xTaskGetCurrentTaskHandle();
    settings_t *s = heap_caps_malloc(sizeof *s, MALLOC_CAP_SPIRAM);
    body_t st = {.cap = 4096}, jb = {.cap = 64 * 1024};
    st.p = heap_caps_malloc(st.cap, MALLOC_CAP_SPIRAM);
    jb.p = heap_caps_malloc(jb.cap, MALLOC_CAP_SPIRAM);
    uint16_t *fb[2];
    for (int i = 0; i < 2; i++) fb[i] = heap_caps_aligned_alloc(16, MAX_W * MAX_H * 2, MALLOC_CAP_SPIRAM);
    int back = 0;
    esp_http_client_handle_t hs = NULL, hf = NULL;
    static char auth[320];
    uint32_t gen = 0xFFFFFFFF, last_poll = 0, fps_t0 = 0, fps_n = 0;
    int prev = -1, last_state = -1;  // previous sensor state: -1 unknown, 0 off, 1 on
    bool video = false;
    for (;;) {
        uint32_t now = model_now_ms();
        if (settings_gen() != gen) {
            gen = settings_gen();
            settings_get(s);
            close_client(&hs);
            close_client(&hf);
            prev = -1;
        }
        bool configured = s->door_ring[0] && s->energy_url[0] && s->energy_token[0];
        int state = 0;
        if (g_door_test_ring) {
            g_door_test_ring = false;
            ring("test");
        }
        if (!configured) {
            set_up(false, "");
        } else if (!net_up() || g_ota_active) {
            state = 1;
            set_up(false, s->door_name);
        } else {
            snprintf(auth, sizeof auth, "Bearer %s", s->energy_token);
            state = g_door_state == 2 ? 2 : 1;
            if (now - last_poll >= POLL_MS) {
                last_poll = now;
                char url[200];
                snprintf(url, sizeof url, "%s/api/states/%s", s->energy_url, s->door_ring);
                int code = get(&hs, url, auth, &st);
                char v[16];
                if (code == 200 && ha_state_str(st.p, v, sizeof v)) {
                    int on = !strcmp(v, "on") ? 1 : !strcmp(v, "off") ? 0 : -1;
                    if (on == 1 && prev == 0) ring(s->door_ring);
                    if (on >= 0) prev = on;
                    state = 2;
                    set_up(true, s->door_name);
                } else {
                    if (last_state == 2) ESP_LOGW(TAG, "%s: status %d", s->door_ring, code);
                    state = 1;
                    set_up(false, s->door_name);
                }
            }
            // Frames, as fast as they come, while the page wants them.
            model_lock();
            bool want = g_model.door_want_video;
            model_unlock();
            if (want && s->door_camera[0] && state == 2) {
                if (!video) ESP_LOGI(TAG, "video on");
                video = true;
                char url[200];
                snprintf(url, sizeof url, "%s/api/camera_proxy/%s?width=%d&height=%d", s->energy_url, s->door_camera,
                         FRAME_W, FRAME_H);
                int code = get(&hf, url, auth, &jb);
                uint16_t w, h;
                if (code == 200 && fb[back] && decode((const uint8_t *)jb.p, jb.n, fb[back], &w, &h)) {
                    model_lock();
                    uint16_t *old = g_model.door_frame;
                    g_model.door_frame = fb[back];
                    g_model.door_w = w;
                    g_model.door_h = h;
                    g_model.door_frame_seq++;
                    model_unlock();
                    (void)old;
                    back = g_model.door_frame == fb[0] ? 1 : 0;  // the one the page is not looking at
                    g_door_frames++;
                    fps_n++;
                } else if (code != 200) {
                    ESP_LOGW(TAG, "camera %s: status %d", s->door_camera, code);
                    vTaskDelay(pdMS_TO_TICKS(1000));
                }
                if (now - fps_t0 >= 3000) {
                    float fps = fps_n * 1000.0f / (now - fps_t0);
                    model_lock();
                    g_model.door_fps = fps;
                    model_unlock();
                    fps_t0 = now;
                    fps_n = 0;
                }
                if (last_state != state) last_state = state;
                g_door_state = state;
                continue;  // next frame straight away
            }
            if (video) {
                ESP_LOGI(TAG, "video off");
                video = false;
                close_client(&hf);
                model_lock();
                g_model.door_fps = 0;
                model_unlock();
            }
        }
        if (state != last_state) {
            if (state == 0) ESP_LOGI(TAG, "off (no ring entity, or no Home Assistant URL and token)");
            last_state = state;
        }
        g_door_state = state;
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

void src_door_start(void)
{
    xTaskCreatePinnedToCoreWithCaps(door_task, "door", 8192, NULL, 4, NULL, 0, MALLOC_CAP_SPIRAM);
}
