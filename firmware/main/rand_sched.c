#include "rand_sched.h"

#include <inttypes.h>
#include <string.h>

#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "feeder.h"
#include "sdkconfig.h"

#define TICK_US (10 * 1000 * 1000)
/* A planned minute that passed more than this long ago is skipped, not run late
 * (a long sequence or an outage must not release a burst of old scatters). */
#define LATE_MINUTES 10
#define MAX_GAP_MIN 720 /* sane upper bound for the planner gap */

static const char *TAG = "sched";

static sched_cfg_t s_cfg;
static uint16_t s_minutes[SCHED_MAX_PER_DAY];
static int64_t s_days[SCHED_MAX_PER_DAY];
static size_t s_count;
static size_t s_next;
static int64_t s_plan_day;
static bool s_planned; /* a plan exists for s_plan_day */
static bool s_replan;  /* config changed: plan again */

void sched_cfg_default(sched_cfg_t *c)
{
    memset(c, 0, sizeof *c);
    c->magic = SCHED_CFG_MAGIC;
    c->window_start_min = CONFIG_FEEDER_DEFAULT_WINDOW_START_MIN;
    c->window_end_min = CONFIG_FEEDER_DEFAULT_WINDOW_END_MIN;
    c->scatters_per_day = CONFIG_FEEDER_DEFAULT_SCATTERS_PER_DAY;
}

bool sched_cfg_valid(const sched_cfg_t *c)
{
    return c->magic == SCHED_CFG_MAGIC && c->window_start_min < GATE_MINUTES_PER_DAY &&
           c->window_end_min < GATE_MINUTES_PER_DAY && c->window_start_min != c->window_end_min &&
           c->scatters_per_day <= SCHED_MAX_PER_DAY;
}

void rand_sched_init(const sched_cfg_t *cfg)
{
    s_cfg = *cfg;
    s_planned = false;
}

void rand_sched_set_cfg(const sched_cfg_t *cfg)
{
    s_cfg = *cfg;
    s_replan = true;
}

const sched_cfg_t *rand_sched_cfg(void)
{
    return &s_cfg;
}

static bool quiet_cb(void *ctx, uint16_t minute)
{
    return gate_in_quiet_hours((const gate_t *)ctx, minute);
}

static uint32_t rng_cb(void *ctx)
{
    (void)ctx;
    return esp_random();
}

/* Plan the whole local day, then keep only the minutes still ahead of now. */
static void replan(const gate_t *g, const gate_time_t *tm)
{
    uint32_t gap = (gate_eff_min_interval(g) + 59) / 60; /* seconds -> minutes, rounded up */
    if (gap < 1) {
        gap = 1;
    }
    if (gap > MAX_GAP_MIN) {
        gap = MAX_GAP_MIN;
    }
    const sched_window_t win = {s_cfg.window_start_min, s_cfg.window_end_min};
    uint16_t all[SCHED_MAX_PER_DAY];
    const size_t n = sched_plan_day(&win, s_cfg.scatters_per_day, (uint16_t)gap, quiet_cb, (void *)g,
                                    rng_cb, NULL, all);
    s_count = sched_filter_remaining(&win, all, n, tm->local_day, tm->local_minute, s_minutes, s_days);
    s_next = 0;
    s_plan_day = sched_window_day(&win, tm->local_day, tm->local_minute);
    s_planned = true;
    s_replan = false;
    ESP_LOGI(TAG, "planned %u of %u scatters for the rest of the day (gap %" PRIu32 " min)",
             (unsigned)s_count, (unsigned)s_cfg.scatters_per_day, gap);
    for (size_t i = 0; i < s_count; i++) {
        ESP_LOGI(TAG, "  %02u:%02u", (unsigned)(s_minutes[i] / 60), (unsigned)(s_minutes[i] % 60));
    }
}

bool rand_sched_poll(const gate_t *g, const gate_time_t *tm)
{
    if (!tm->clock_valid) {
        s_planned = false; /* plan again once the clock is valid */
        return false;
    }
    const sched_window_t win = {s_cfg.window_start_min, s_cfg.window_end_min};
    const int64_t win_day = sched_window_day(&win, tm->local_day, tm->local_minute);
    if (!s_planned || s_replan || win_day != s_plan_day) {
        replan(g, tm);
    }
    const int64_t now_abs = tm->local_day * GATE_MINUTES_PER_DAY + (int64_t)tm->local_minute;
    while (s_next < s_count) {
        const uint16_t m = s_minutes[s_next];
        const int64_t slot_abs = s_days[s_next] * GATE_MINUTES_PER_DAY + (int64_t)m;
        if (slot_abs > now_abs) {
            break; /* not yet */
        }
        s_next++;
        if (now_abs - slot_abs > LATE_MINUTES) {
            ESP_LOGW(TAG, "skipped missed slot %02u:%02u", (unsigned)(m / 60), (unsigned)(m % 60));
            continue;
        }
        return true;
    }
    return false;
}

static void tick_cb(void *arg)
{
    (void)arg;
    static feeder_msg_t tick = {.kind = FEEDER_MSG_TICK}; /* single writer: this timer */
    feeder_post(&tick); /* a full queue drops the tick; the next one follows in 10 s */
}

void rand_sched_start_ticker(void)
{
    static const esp_timer_create_args_t args = {.callback = tick_cb, .name = "sched_tick"};
    esp_timer_handle_t timer;
    if (esp_timer_create(&args, &timer) != ESP_OK ||
        esp_timer_start_periodic(timer, TICK_US) != ESP_OK) {
        ESP_LOGE(TAG, "ticker start failed: no random scatters");
    }
}
