#include "feeder_util.h"

#define MINUTES_PER_DAY 1440u

/* Distance on the 24 h circle, so 23:50 and 00:05 are 15 minutes apart. */
static uint16_t circ_dist(uint16_t a, uint16_t b)
{
    const uint16_t d = (uint16_t)(a > b ? a - b : b - a);
    return d <= MINUTES_PER_DAY / 2 ? d : (uint16_t)(MINUTES_PER_DAY - d);
}

size_t sched_plan_day(const sched_window_t *win, uint8_t n, uint16_t min_gap_min,
                      bool (*is_quiet)(void *ctx, uint16_t minute), void *ctx,
                      uint32_t (*rng)(void *rng_ctx), void *rng_ctx, uint16_t *out_minutes)
{
    if (win == NULL || rng == NULL || out_minutes == NULL || n == 0) {
        return 0;
    }
    if (win->start_min >= MINUTES_PER_DAY || win->end_min >= MINUTES_PER_DAY ||
        win->start_min == win->end_min) {
        return 0;
    }
    if (n > SCHED_MAX_PER_DAY) {
        n = SCHED_MAX_PER_DAY;
    }
    if (min_gap_min < 1) {
        min_gap_min = 1; /* minutes are distinct at least */
    }
    /* Window length in minutes, 1..1439; the end minute is exclusive. */
    const uint32_t wlen = (win->end_min + MINUTES_PER_DAY - win->start_min) % MINUTES_PER_DAY;

    size_t placed = 0;
    for (uint32_t tries = 200u * n; tries > 0 && placed < n; tries--) {
        const uint16_t minute = (uint16_t)((win->start_min + rng(rng_ctx) % wlen) % MINUTES_PER_DAY);
        if (is_quiet != NULL && is_quiet(ctx, minute)) {
            continue;
        }
        bool ok = true;
        size_t pos = 0; /* insertion point that keeps the list ascending */
        for (size_t i = 0; i < placed; i++) {
            if (circ_dist(out_minutes[i], minute) < min_gap_min) {
                ok = false;
                break;
            }
            if (out_minutes[i] < minute) {
                pos = i + 1;
            }
        }
        if (!ok) {
            continue;
        }
        for (size_t i = placed; i > pos; i--) {
            out_minutes[i] = out_minutes[i - 1];
        }
        out_minutes[pos] = minute;
        placed++;
    }
    return placed;
}

bool sched_next_after(const uint16_t *minutes, size_t n, uint16_t now_minute, uint16_t *next)
{
    if (minutes == NULL || next == NULL) {
        return false;
    }
    for (size_t i = 0; i < n; i++) {
        if (minutes[i] > now_minute) {
            *next = minutes[i];
            return true;
        }
    }
    return false;
}
