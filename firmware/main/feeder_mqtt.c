#include "feeder_mqtt.h"

#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "feeder.h"
#include "mqtt_client.h"
#include "sdkconfig.h"

/* Embedded by EMBED_TXTFILES in main/CMakeLists.txt (NUL-terminated PEM text). */
extern const char ca_crt_start[] asm("_binary_ca_crt_start");
extern const char device_crt_start[] asm("_binary_device_crt_start");
extern const char device_key_start[] asm("_binary_device_key_start");

#define LAST_WILL_MSG "{\"status\":\"offline\"}"

static const char *TAG = "mqtt";

static esp_mqtt_client_handle_t s_client;
static volatile bool s_connected;
static char s_topic_cmd[64];
static char s_topic_ack[64];
static char s_topic_event[64];
static char s_topic_state[64];

/* Reassembly of a (possibly fragmented) cmd payload. Only the MQTT task touches it. */
static feeder_msg_t s_in;
static bool s_in_active;

/* Only copy the raw payload into the feeder queue. Parsing and the gate run in
 * the feeder task. */
static void on_data(const esp_mqtt_event_handle_t ev)
{
    if (ev->current_data_offset == 0) {
        s_in_active = false;
        if ((size_t)ev->topic_len != strlen(s_topic_cmd) ||
            memcmp(ev->topic, s_topic_cmd, (size_t)ev->topic_len) != 0) {
            return; /* not ours; we only subscribe to cmd anyway */
        }
        if (ev->total_data_len < 0 || ev->total_data_len > FEEDER_CMD_MAX) {
            ESP_LOGW(TAG, "cmd payload %d bytes > %d: dropped, no ack", ev->total_data_len,
                     FEEDER_CMD_MAX);
            return;
        }
        s_in.len = 0;
        s_in_active = true;
    }
    if (!s_in_active) {
        return;
    }
    /* Never trust the lengths: re-check the fragment against our own buffer. */
    if (ev->data_len < 0 || (size_t)ev->current_data_offset != s_in.len ||
        s_in.len + (size_t)ev->data_len > FEEDER_CMD_MAX) {
        s_in_active = false;
        ESP_LOGW(TAG, "bad cmd fragment: dropped");
        return;
    }
    memcpy(s_in.payload + s_in.len, ev->data, (size_t)ev->data_len);
    s_in.len = (uint16_t)(s_in.len + (size_t)ev->data_len);
    if ((int)s_in.len == ev->total_data_len) {
        s_in_active = false;
        s_in.payload[s_in.len] = '\0';
        s_in.kind = FEEDER_MSG_CMD;
        if (!feeder_post(&s_in)) {
            ESP_LOGW(TAG, "feeder queue full: cmd dropped, no ack");
        }
    }
}

static void on_event(void *arg, esp_event_base_t base, int32_t event_id, void *event_data)
{
    (void)arg;
    (void)base;
    esp_mqtt_event_handle_t ev = event_data;
    switch ((esp_mqtt_event_id_t)event_id) {
    case MQTT_EVENT_CONNECTED: {
        ESP_LOGI(TAG, "connected (session_present=%d)", ev->session_present);
        s_connected = true;
        if (esp_mqtt_client_subscribe(s_client, s_topic_cmd, 1) < 0) {
            ESP_LOGE(TAG, "subscribe failed");
        }
        /* The feeder task publishes the state after this message. */
        static feeder_msg_t connected = {.kind = FEEDER_MSG_CONNECTED};
        feeder_post(&connected);
        break;
    }
    case MQTT_EVENT_DISCONNECTED:
        ESP_LOGW(TAG, "disconnected");
        s_connected = false;
        s_in_active = false;
        break;
    case MQTT_EVENT_DATA:
        on_data(ev);
        break;
    case MQTT_EVENT_ERROR:
        if (ev->error_handle != NULL) {
            ESP_LOGW(TAG, "error type %d, tls esp_err 0x%x, tls stack 0x%x",
                     (int)ev->error_handle->error_type, (unsigned)ev->error_handle->esp_tls_last_esp_err,
                     (unsigned)ev->error_handle->esp_tls_stack_err);
        }
        break;
    default:
        break;
    }
}

esp_err_t feeder_mqtt_start(const char *device_id)
{
    if (s_client != NULL) {
        return ESP_OK;
    }
    if (snprintf(s_topic_cmd, sizeof s_topic_cmd, "feeder/%s/cmd", device_id) >= (int)sizeof s_topic_cmd ||
        snprintf(s_topic_ack, sizeof s_topic_ack, "feeder/%s/ack", device_id) >= (int)sizeof s_topic_ack ||
        snprintf(s_topic_event, sizeof s_topic_event, "feeder/%s/event", device_id) >= (int)sizeof s_topic_event ||
        snprintf(s_topic_state, sizeof s_topic_state, "feeder/%s/state", device_id) >= (int)sizeof s_topic_state) {
        return ESP_ERR_INVALID_SIZE;
    }

    const esp_mqtt_client_config_t cfg = {
        .broker.address.uri = CONFIG_FEEDER_MQTT_BROKER_URI,
        /* Server verification stays on: our CA only, host name checked. */
        .broker.verification.certificate = ca_crt_start,
        .credentials.client_id = device_id,
        .credentials.authentication.certificate = device_crt_start,
        .credentials.authentication.key = device_key_start,
        .session.keepalive = 60,
        /* Persistent session: QoS 1 commands sent while offline are redelivered.
         * The gate rejects replayed and expired ones. */
        .session.disable_clean_session = true,
        .session.last_will =
            {
                .topic = s_topic_state,
                .msg = LAST_WILL_MSG,
                .msg_len = 0, /* 0 = strlen */
                .qos = 1,
                .retain = 1,
            },
        .network.reconnect_timeout_ms = 5000, /* the client reconnects by itself */
    };
    s_client = esp_mqtt_client_init(&cfg);
    if (s_client == NULL) {
        return ESP_FAIL;
    }
    esp_err_t err = esp_mqtt_client_register_event(s_client, ESP_EVENT_ANY_ID, on_event, NULL);
    if (err == ESP_OK) {
        err = esp_mqtt_client_start(s_client);
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "start: %s", esp_err_to_name(err));
        esp_mqtt_client_destroy(s_client);
        s_client = NULL;
    }
    return err;
}

bool feeder_mqtt_connected(void)
{
    return s_connected;
}

bool feeder_mqtt_publish(feeder_topic_t topic, const char *json)
{
    if (s_client == NULL || json == NULL) {
        return false;
    }
    const char *t = s_topic_event;
    int retain = 0;
    switch (topic) {
    case FEEDER_TOPIC_ACK:
        t = s_topic_ack;
        break;
    case FEEDER_TOPIC_STATE:
        if (!s_connected) {
            return false;
        }
        t = s_topic_state;
        retain = 1;
        break;
    case FEEDER_TOPIC_EVENT:
    default:
        break;
    }
    /* enqueue: the mqtt task sends it, so the feeder task never blocks on the network. */
    const int id = esp_mqtt_client_enqueue(s_client, t, json, 0, 1, retain, true);
    if (id < 0) {
        ESP_LOGW(TAG, "enqueue to %s failed (%d)", t, id);
        return false;
    }
    return true;
}
