/* Wall clock: SNTP sync flag, local time, and the gate_time_t the gate needs. */
#ifndef FCLOCK_H
#define FCLOCK_H

#include <stdbool.h>
#include <stddef.h>

#include "esp_err.h"
#include "safety_gate.h"

/* Set TZ from Kconfig. Call once at boot, before the first fclock_get(). */
void fclock_init(void);

/* Start SNTP (after an IP address exists). Safe to call more than once. */
esp_err_t fclock_sntp_start(void);

/* True once consistent SNTP replies establish time. A held outlier is never applied. */
bool fclock_valid(void);

/* Established SNTP time projected with monotonic elapsed time for the safety gate. local_day is the day number in LOCAL time,
 * built from the broken-down local date, so it is correct across DST changes. */
void fclock_get(gate_time_t *tm);

/* "YYYY-MM-DDTHH:MM:SSZ" of now in buf (>= RFC3339_UTC_BUF). Returns false
 * and leaves buf empty when the clock is not valid: callers then omit `at`. */
bool fclock_now_rfc3339(char *buf, size_t len);

#endif /* FCLOCK_H */
