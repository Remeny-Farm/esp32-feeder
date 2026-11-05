/*
 * motor: dosing and spinner API used by the feeder task.
 *
 * All calls are blocking and run in the feeder task only. The safety gate has
 * already admitted the scatter before any of these is called.
 *
 * Backend status: motor_stub.c only logs. The real backend will talk to the
 * Einsy RAMBo motor controller over UART (docs/architecture.md, prototype
 * build), which adds its own hard limits and a heartbeat watchdog.
 */
#ifndef MOTOR_H
#define MOTOR_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    MOTOR_OK = 0,
    MOTOR_FAULT_STALL,         /* auger stalled (StallGuard / current) */
    MOTOR_FAULT_HOPPER_EMPTY,  /* ran dry before the dose was complete */
    MOTOR_FAULT_CONTROLLER     /* motor controller silent, kill relay, UART error */
} motor_result_t;

void motor_init(void);

/* Run the auger of `hopper` (0-based) until `grams` are dispensed. *dispensed_g
 * is always written; on a fault it may be less than `grams`. */
motor_result_t motor_dose(uint8_t hopper, uint32_t grams, uint32_t *dispensed_g);

/* Run the shared spinner for `ms` milliseconds. */
void motor_spin(uint32_t ms);

/* Stop every motor now. Safe to call at any time, also when nothing runs. */
void motor_stop_all(void);

#ifdef __cplusplus
}
#endif

#endif /* MOTOR_H */
