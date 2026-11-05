/*
 * feeder_util: pure C99 helpers. No ESP-IDF, no libc time functions, no
 * malloc. Host-testable (see firmware/test/host/test_util.c).
 */
#ifndef FEEDER_UTIL_H
#define FEEDER_UTIL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Buffer size for rfc3339_format(): "YYYY-MM-DDTHH:MM:SSZ" plus NUL. */
#define RFC3339_UTC_BUF 21

/* Days since 1970-01-01 of a proleptic Gregorian civil date. Month 1..12,
 * day 1..31 (not validated). Valid for any year; no timegm, no time_t. */
int64_t days_from_civil(int64_t year, unsigned month, unsigned day);

/* Parse "YYYY-MM-DDTHH:MM:SS[.frac](Z|+hh:mm|-hh:mm)" strictly: every field
 * range-checked, leap years honoured, no leading or trailing characters.
 * Fractional seconds are accepted and dropped. Leap second ":60" is rejected.
 * On success *epoch is UTC seconds since 1970. On failure *epoch is untouched. */
bool rfc3339_parse(const char *s, int64_t *epoch);

/* Write "YYYY-MM-DDTHH:MM:SSZ" (UTC). Returns the length written (20), or 0
 * when len < RFC3339_UTC_BUF or the year is outside 0000..9999. */
size_t rfc3339_format(int64_t epoch, char *buf, size_t len);

/* Strict "HH:MM" (24 h, 00:00..23:59). *minute = minutes since midnight. */
bool hhmm_parse(const char *s, uint16_t *minute);

/* ---- random day planner ---- */

#define SCHED_MAX_PER_DAY 24

/* Allowed window [start_min, end_min) in local minutes of day (0..1439). It
 * crosses midnight when start_min > end_min. start_min == end_min is invalid. */
typedef struct {
    uint16_t start_min;
    uint16_t end_min;
} sched_window_t;

/*
 * Pick up to `n` (at most SCHED_MAX_PER_DAY) distinct minutes of the local day:
 *  - inside the window,
 *  - for which is_quiet() (may be NULL) returns false,
 *  - at least `min_gap_min` apart (circular over 24 h; 0 is treated as 1).
 * Writes them ascending to out_minutes (room for min(n, 24) entries) and
 * returns how many were placed. Rejection sampling with a bounded number of
 * tries: it never loops forever and may place fewer than n. Returns 0 for
 * n == 0, an invalid window, or NULL rng/out_minutes.
 * The planner is a convenience: the safety gate still checks every scatter.
 */
size_t sched_plan_day(const sched_window_t *win, uint8_t n, uint16_t min_gap_min,
                      bool (*is_quiet)(void *ctx, uint16_t minute), void *ctx,
                      uint32_t (*rng)(void *rng_ctx), void *rng_ctx,
                      uint16_t *out_minutes);

/* First planned minute strictly greater than now_minute. `minutes` ascending.
 * Returns false when none is left today. */
bool sched_next_after(const uint16_t *minutes, size_t n, uint16_t now_minute, uint16_t *next);

#ifdef __cplusplus
}
#endif

#endif /* FEEDER_UTIL_H */
