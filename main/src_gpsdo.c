// GPSDO status from MQTT: subscribes to one topic carrying the monitor's JSON
// state document and fills the model's gps_* fields. Subscribe only: the knob
// never publishes anything (no last will either).
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/task.h"
#include "gpsdo_parse.h"
#include "model.h"
#include "mqtt_client.h"
#include "net.h"
#include "platform.h"
#include "settings.h"
#include "sources.h"

static const char *TAG = "gpsdo";

#define STALE_MS 60000
#define MSG_MAX 4096

volatile int g_gps_state;  // 0 off, 1 down, 2 up
volatile uint32_t g_gps_msgs;

static esp_mqtt_client_handle_t s_client;
static volatile bool s_connected;
static char s_topic[64];
static char *s_msg;          // reassembly of a message that arrives in pieces
static size_t s_msg_len;
static model_t *s_tmp;       // parsed into this, then the gps_* fields copied over

static void take_message(const char *p, size_t n)
{
    if (!gpsdo_parse(p, n, s_tmp)) return;
    model_lock();
    // Copy the gps_* block (from gps_state to gps_alarms inclusive).
    size_t a = offsetof(model_t, gps_state), b = offsetof(model_t, gps_alarms) + sizeof g_model.gps_alarms;
    memcpy((char *)&g_model + a, (char *)s_tmp + a, b - a);
    g_model.gps_seq++;
    g_model.gps_msg_ms = model_now_ms();
    g_model.gps_up = true;
    model_unlock();
    g_gps_msgs++;
}

static void on_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    esp_mqtt_event_handle_t e = data;
    switch ((esp_mqtt_event_id_t)id) {
    case MQTT_EVENT_CONNECTED:
        s_connected = true;
        esp_mqtt_client_subscribe(s_client, s_topic, 0);
        ESP_LOGI(TAG, "connected, subscribed to %s", s_topic);
        break;
    case MQTT_EVENT_DISCONNECTED:
        if (s_connected) ESP_LOGW(TAG, "disconnected");
        s_connected = false;
        break;
    case MQTT_EVENT_DATA:
        if (e->current_data_offset == 0) s_msg_len = 0;
        if (e->total_data_len > MSG_MAX) break;  // not ours, or far too big
        if (e->current_data_offset == 0 && e->data_len == e->total_data_len) {
            take_message(e->data, e->data_len);
        } else {
            memcpy(s_msg + e->current_data_offset, e->data, e->data_len);
            s_msg_len = e->current_data_offset + e->data_len;
            if (s_msg_len == (size_t)e->total_data_len) take_message(s_msg, s_msg_len);
        }
        break;
    default:
        break;
    }
}

static void gps_task(void *arg)
{
    settings_t *s = heap_caps_malloc(sizeof *s, MALLOC_CAP_SPIRAM);
    uint32_t gen = 0xFFFFFFFF;
    int last_state = -1;
    for (;;) {
        if (settings_gen() != gen) {
            gen = settings_gen();
            settings_get(s);
            bool want = s->mqtt_url[0] && s->gpsdo_topic[0];
            bool same = s_client && !strcmp(s_topic, s->gpsdo_topic);
            if (s_client && (!want || !same)) {
                esp_mqtt_client_destroy(s_client);  // settings changed: start again
                s_client = NULL;
                s_connected = false;
            }
            model_lock();
            snprintf(g_model.gps_name, sizeof g_model.gps_name, "%s", want ? s->gpsdo_name : "");
            if (!want) g_model.gps_up = false;
            model_unlock();
            if (want && !s_client) {
                snprintf(s_topic, sizeof s_topic, "%s", s->gpsdo_topic);
                esp_mqtt_client_config_t c = {
                    .broker.address.uri = s->mqtt_url,
                    .network.reconnect_timeout_ms = 5000,
                    .session.keepalive = 30,
                    .buffer.size = 2048,
                    .task.stack_size = 6144,
                };
                s_client = esp_mqtt_client_init(&c);
                if (s_client) {
                    esp_mqtt_client_register_event(s_client, ESP_EVENT_ANY_ID, on_event, NULL);
                    esp_mqtt_client_start(s_client);
                } else {
                    ESP_LOGW(TAG, "MQTT URL not usable");
                }
            }
        }
        int state = !s_client ? 0 : s_connected ? 2 : 1;
        model_lock();
        bool fresh = g_model.gps_seq && model_now_ms() - g_model.gps_msg_ms < STALE_MS;
        g_model.gps_up = s_connected && fresh;
        model_unlock();
        if (state != last_state) {
            if (state == 0) ESP_LOGI(TAG, "off (no MQTT URL or topic)");
            last_state = state;
        }
        g_gps_state = state;
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

void src_gpsdo_start(void)
{
    s_msg = heap_caps_malloc(MSG_MAX, MALLOC_CAP_SPIRAM);
    s_tmp = heap_caps_calloc(1, sizeof *s_tmp, MALLOC_CAP_SPIRAM);
    xTaskCreatePinnedToCoreWithCaps(gps_task, "gpsdo", 4096, NULL, 3, NULL, 0, MALLOC_CAP_SPIRAM);
}
