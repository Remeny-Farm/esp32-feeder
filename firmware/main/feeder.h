/*
 * Feeder task: the ONE owner of the gate_t. Everything that touches the gate
 * (remote commands, the random scheduler, faults, the service button, state
 * publishing) runs here, so check -> run -> commit is serialised. Other tasks
 * only post messages to the queue.
 */
#ifndef FEEDER_H
#define FEEDER_H

#include <stdbool.h>
#include <stdint.h>

#define FEEDER_CMD_MAX 2048 /* larger command payloads are dropped with a log */
#define FEEDER_QUEUE_LEN 6

typedef enum {
    FEEDER_MSG_CMD,       /* payload = raw JSON of a cmd message */
    FEEDER_MSG_CONNECTED, /* MQTT connected and subscribed */
    FEEDER_MSG_TICK,      /* 10 s tick for the random scheduler */
    FEEDER_MSG_LID        /* payload[0] != 0: lid open */
} feeder_msg_kind_t;

typedef struct {
    feeder_msg_kind_t kind;
    uint16_t len;
    char payload[FEEDER_CMD_MAX + 1]; /* NUL-terminated copy */
} feeder_msg_t;

/* Create the queue, init the gate, restore gate and schedule from NVS. Call
 * once, before MQTT starts (it posts to the queue). */
void feeder_init(void);

/* Start the feeder task and the scheduler ticker. */
void feeder_start(void);

/* Non-blocking copy into the queue, from any task. False when it is full. */
bool feeder_post(const feeder_msg_t *msg);

/* Lid input hook. There is no lid hardware yet: nothing calls this. When the
 * reed switch exists, call it from ONE task or ISR-safe wrapper on each change. */
void feeder_lid_changed(bool open);

#endif /* FEEDER_H */
