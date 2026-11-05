/*
 * MQTT message formats: command parsing and ack/event/state JSON building.
 * Contract: protocol/ schemas and docs/architecture.md. Uses cJSON; every
 * builder returns a malloc'ed string that the caller frees with proto_free().
 */
#ifndef PROTO_H
#define PROTO_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "safety_gate.h"

typedef enum { CMD_SCATTER, CMD_CUE, CMD_PAUSE, CMD_RESUME, CMD_SET_SCHEDULE } proto_cmd_type_t;

typedef struct {
    char id[GATE_ID_BUF];
    proto_cmd_type_t type;
    int64_t issued_at;  /* UTC epoch seconds */
    int64_t expires_at;
    /* scatter */
    gate_dose_t doses[GATE_MAX_HOPPERS];
    size_t n_doses;
    bool cue; /* scatter: play the cue (default true) */
    /* set_schedule */
    uint16_t window_start_min;
    uint16_t window_end_min;
    uint8_t scatters_per_day;
    gate_runtime_limits_t limits; /* cleared when the command has no `limits` */
} proto_cmd_t;

typedef enum {
    PROTO_PARSE_OK,
    PROTO_PARSE_INVALID, /* the id was read (cmd->id is set) but the rest is bad: ack invalid */
    PROTO_PARSE_DROP     /* no usable id, oversized or not JSON: log, no ack */
} proto_parse_t;

/* Parse one command payload (not necessarily NUL-terminated). Hard bounds:
 * len 1..FEEDER_CMD_MAX, nesting depth, integer ranges. Never trusts a length. */
proto_parse_t proto_parse_cmd(const char *buf, size_t len, proto_cmd_t *out);

/* ---- builders ---- `at` is an RFC 3339 UTC string, or NULL to omit the field
 * (clock invalid: never invent a time). They return NULL on out-of-memory. */
char *proto_build_ack(const char *id, bool accepted, const char *reason, const char *at);

typedef enum {
    PROTO_FAULT_STALL,
    PROTO_FAULT_HOPPER_EMPTY,
    PROTO_FAULT_LID_OPEN,
    PROTO_FAULT_MOTOR_CONTROLLER,
    PROTO_FAULT_WATCHDOG
} proto_fault_t;

/* command_id may be NULL (random source). */
char *proto_build_event_cue(gate_source_t src, const char *command_id, const char *at);
/* doses: ACTUAL grams per requested hopper, in request order. */
char *proto_build_event_scatter(gate_source_t src, const char *command_id, const gate_dose_t *doses,
                                const uint32_t *actual_g, size_t n, const char *at);
/* hopper < 0: no hopper in the fault object. */
char *proto_build_event_fault(gate_source_t src, const char *command_id, proto_fault_t code,
                              int hopper, const char *at);

typedef struct {
    bool paused;
    bool clock_valid;
    const char *fw_version;
    uint8_t hopper_count;
    gate_level_t level[GATE_MAX_HOPPERS];
    uint32_t used_g[GATE_MAX_HOPPERS];
} proto_state_t;

char *proto_build_state(const proto_state_t *s, const char *at);

void proto_free(char *s);

#endif /* PROTO_H */
