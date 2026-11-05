/* MQTT link: mTLS client, subscription, raw command intake, publishing. */
#ifndef FEEDER_MQTT_H
#define FEEDER_MQTT_H

#include <stdbool.h>

#include "esp_err.h"

typedef enum { FEEDER_TOPIC_ACK, FEEDER_TOPIC_EVENT, FEEDER_TOPIC_STATE } feeder_topic_t;

/* Create and start the client. Safe to call more than once. The device id must
 * already be validated (gate_config.h). Needs an IP address. */
esp_err_t feeder_mqtt_start(const char *device_id);

bool feeder_mqtt_connected(void);

/* QoS 1. Acks and events are queued in the client outbox, so they survive a
 * short outage. State is retained and dropped when offline (the device
 * publishes it again on connect). Returns false when nothing was queued. */
bool feeder_mqtt_publish(feeder_topic_t topic, const char *json);

#endif /* FEEDER_MQTT_H */
