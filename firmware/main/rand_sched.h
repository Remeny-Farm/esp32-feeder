/*
 * Random scatter scheduler.
 *
 * It has two parts:
 *  - a 10 s ticker (esp_timer) that only posts FEEDER_MSG_TICK to the feeder
 *    queue;
 *  - the planner, which runs inside the feeder task. It reads the gate (quiet
 *    hours, min interval), and the gate has no lock, so only the feeder task
 *    may touch it.
 * The planner is a convenience: the gate still checks every scatter.
 */
#ifndef RAND_SCHED_H
#define RAND_SCHED_H

#include <stdbool.h>
#include <stdint.h>

#include "feeder_util.h"
#include "safety_gate.h"

#define SCHED_CFG_MAGIC 0x53434844u /* "SCHD" */

/* Persisted schedule (window + count). */
typedef struct {
    uint32_t magic;
    uint16_t window_start_min; /* local minute of day, 0..1439 */
    uint16_t window_end_min;   /* exclusive; != start */
    uint8_t scatters_per_day;  /* 0..SCHED_MAX_PER_DAY */
    uint8_t reserved[3];
} sched_cfg_t;

/* Compiled-in default (Kconfig), used before any set_schedule. */
void sched_cfg_default(sched_cfg_t *c);
bool sched_cfg_valid(const sched_cfg_t *c);

/* Feeder task only. */
void rand_sched_init(const sched_cfg_t *cfg);
void rand_sched_set_cfg(const sched_cfg_t *cfg); /* also forces a re-plan */
const sched_cfg_t *rand_sched_cfg(void);

/* Feeder task only, on each tick. Returns true when a random scatter is due
 * now. Plans the day first when needed. False while the clock is invalid. */
bool rand_sched_poll(const gate_t *g, const gate_time_t *tm);

/* Start the 10 s ticker. Any task. */
void rand_sched_start_ticker(void);

#endif /* RAND_SCHED_H */
