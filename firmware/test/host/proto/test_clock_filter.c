/* Real fclock.c + real gate accounting. Only SNTP/OS/monotonic/RTOS boundaries
 * are replaced; no host system clock is changed. One SNTP writer, many readers. */
#include "mini_test.h"
#include "fclock.h"
#include "feeder_util.h"
#include "proto.h"
#include "esp_sntp.h"
#include "freertos/FreeRTOS.h"
#include <pthread.h>
#include <stdlib.h>
#include <sys/time.h>
#include <time.h>

#define T INT64_C(1793872800)
#define W INT64_C(4102444800) /* 2100-01-01: isolated untrusted sample */
static pthread_mutex_t env_lock = PTHREAD_MUTEX_INITIALIZER;
static int64_t mono_us, os_wall;
static unsigned applied, interval_ms, start_calls;
static __thread int critical_depth;
static int writer_errors;

void clock_test_enter(pthread_mutex_t *lock) { pthread_mutex_lock(lock); critical_depth++; }
void clock_test_exit(pthread_mutex_t *lock) { critical_depth--; pthread_mutex_unlock(lock); }
int64_t esp_timer_get_time(void)
{
    pthread_mutex_lock(&env_lock); int64_t v = mono_us; pthread_mutex_unlock(&env_lock); return v;
}
static void advance(unsigned seconds)
{
    pthread_mutex_lock(&env_lock); mono_us += (int64_t)seconds * 1000000; os_wall += seconds; pthread_mutex_unlock(&env_lock);
}
int clock_test_settimeofday(const struct timeval *tv, const struct timezone *tz)
{
    (void)tz;
    if (critical_depth) abort(); /* setter may block: never hold the clock critical section */
    pthread_mutex_lock(&env_lock); os_wall = tv->tv_sec; applied++; pthread_mutex_unlock(&env_lock); return 0;
}
int clock_test_gettimeofday(struct timeval *tv, void *tz)
{
    (void)tz; pthread_mutex_lock(&env_lock); tv->tv_sec = os_wall; tv->tv_usec = 0; pthread_mutex_unlock(&env_lock); return 0;
}
int clock_test_adjtime(const struct timeval *delta, struct timeval *old)
{ (void)delta; if (old) memset(old, 0, sizeof *old); return 0; }
time_t clock_test_time(time_t *out)
{
    pthread_mutex_lock(&env_lock); time_t v = (time_t)os_wall; pthread_mutex_unlock(&env_lock); if (out) *out = v; return v;
}
void sntp_set_sync_interval(uint32_t value) { if (critical_depth) abort(); interval_ms = value; }
void sntp_set_sync_status(sntp_sync_status_t value) { (void)value; if (critical_depth) abort(); }
void esp_sntp_setoperatingmode(int mode) { (void)mode; }
void esp_sntp_setservername(int index, const char *name) { (void)index; (void)name; }
void esp_sntp_init(void) { start_calls++; }

static void sample(int64_t wall)
{
    struct timeval tv = {.tv_sec = (time_t)wall, .tv_usec = 0};
    sntp_sync_time(&tv); /* actual SDK weak default or production strong override */
}
static void setup(gate_t *g)
{
    fclock_init(); CHECK_EQ(fclock_sntp_start(), ESP_OK); CHECK_EQ(fclock_sntp_start(), ESP_OK);
    CHECK_EQ(start_calls, 1);
    gate_limits_t limits = {.hopper_count = 1, .max_dose_g = {10}, .daily_budget_g = {50}, .daily_total_g = 100};
    CHECK(gate_init(g, &limits));
}
static gate_result_t command(gate_t *g, const char *id, int64_t now)
{
    gate_time_t tm; fclock_get(&tm);
    const gate_cmd_meta_t m = {.id = id, .issued_at = now, .expires_at = now + 300};
    return gate_check_command(g, &tm, &m);
}
static void establish(void)
{
    sample(T); advance(15); sample(T + 15);
}
static gate_t s_gate;
static const struct { const char *version; } app_description = {"host-clock-proof"};
#define esp_app_get_description() (&app_description)
uint8_t feeder_hopper_count(void) { return 1; }
/* @BUILD_STATE@ */

static void test_bad_initial_sample_then_correction_and_restore(void)
{
    gate_t g, restored; setup(&g);
    sample(W); sample(W); /* one early repeated callback cannot establish time */
    gate_time_t bad; fclock_get(&bad);
    CHECK(!bad.clock_valid); CHECK_EQ(applied, 0); CHECK_EQ(interval_ms, 15000);
    CHECK_EQ(gate_used_today(&g, &bad, 0), 0);
    CHECK_EQ(gate_state(&g)->day, 0);
    s_gate = g; proto_state_t view; build_state(&view, &bad);
    CHECK(!view.clock_valid); CHECK_EQ(gate_state(&s_gate)->day, 0); /* actual build_state path */
    CHECK_EQ(command(&g, "clock-probe", T), GATE_NO_CLOCK);
    advance(15); sample(T);
    CHECK(!fclock_valid()); CHECK_EQ(applied, 0);
    advance(15); sample(T + 15);
    CHECK(fclock_valid()); CHECK_EQ(applied, 1); CHECK_EQ(interval_ms, 3600000);
    CHECK_EQ(command(&g, "resume-once", T + 15), GATE_OK);
    CHECK_EQ(command(&g, "operator-pause", T + 15), GATE_OK); gate_set_paused(&g, true);
    gate_state_t saved = *gate_state(&g);
    gate_limits_t limits = {.hopper_count = 1, .max_dose_g = {10}, .daily_budget_g = {50}, .daily_total_g = 100};
    CHECK(gate_init(&restored, &limits)); CHECK(gate_restore(&restored, &saved));
    CHECK_EQ(command(&restored, "resume-once", T + 15), GATE_DUPLICATE);
    CHECK_EQ(command(&restored, "normal-after-correction", T + 15), GATE_OK);
    CHECK(gate_paused(&restored));
    const char *path = getenv("FEEDER_CLOCK_STATE_FIXTURE");
    if (path) { FILE *f = fopen(path, "wb"); CHECK(f != NULL); if (f) { CHECK_EQ(fwrite(&saved, 1, sizeof saved, f), sizeof saved); fclose(f); } }
}
static void test_established_outlier_cannot_change_day_or_retirement(void)
{
    gate_t g; setup(&g); establish();
    gate_time_t before; fclock_get(&before);
    uint32_t grams[GATE_MAX_HOPPERS] = {7}; gate_commit_scatter(&g, &before, grams);
    gate_set_paused(&g, true);
    const int64_t day = gate_state(&g)->day;
    const unsigned good_applied = applied;
    advance(15); sample(W);
    gate_time_t held; fclock_get(&held);
    CHECK(held.clock_valid); CHECK_EQ(held.now, T + 30); CHECK_EQ(applied, good_applied);
    CHECK_EQ(gate_used_today(&g, &held, 0), 7); CHECK_EQ(gate_state(&g)->day, day);
    s_gate = g; proto_state_t view; build_state(&view, &held);
    CHECK_EQ(view.used_g[0], 7); CHECK_EQ(gate_state(&s_gate)->day, day);
    CHECK_EQ(gate_check_cue(&g, &held, GATE_SRC_RANDOM, NULL), GATE_PAUSED);
    CHECK_EQ(command(&g, "control-after-outlier", T + 30), GATE_OK);
    advance(15); sample(T + 45);
    CHECK_EQ(applied, good_applied + 1); CHECK_EQ(interval_ms, 3600000);
    CHECK_EQ(gate_state(&g)->day, day); CHECK_EQ(gate_used_today(&g, &held, 0), 7);
}
static void test_rebooted_clock_reestablishes_without_losing_pause(void)
{
    gate_t g; setup(&g);
    gate_state_t saved; memset(&saved, 0, sizeof saved);
    const char *path = getenv("FEEDER_CLOCK_STATE_FIXTURE");
    FILE *f = path ? fopen(path, "rb") : NULL; CHECK(f != NULL);
    if (!f) return;
    CHECK_EQ(fread(&saved, 1, sizeof saved, f), sizeof saved); fclose(f);
    CHECK(gate_restore(&g, &saved));
    CHECK(!fclock_valid()); establish();
    CHECK(fclock_valid()); CHECK(gate_paused(&g));
    CHECK_EQ(command(&g, "resume-once", T + 15), GATE_DUPLICATE);
    CHECK_EQ(command(&g, "fresh-after-reboot", T + 15), GATE_OK);
}
static void *reader(void *ignored)
{
    (void)ignored;
    for (int i = 0; i < 20000; i++) {
        gate_time_t tm; fclock_get(&tm);
        if (tm.clock_valid && (tm.now < T || tm.now > T + 10000 ||
            tm.local_day != tm.now / 86400 || tm.local_minute != (tm.now % 86400) / 60)) return (void *)1;
    }
    return NULL;
}
static void test_concurrent_clock_snapshot(void)
{
    gate_t g; setup(&g); establish();
    pthread_t thread; CHECK_EQ(pthread_create(&thread, NULL, reader, NULL), 0);
    for (int i = 0; i < 128; i++) {
        advance(15); sample(W);
        gate_time_t held; fclock_get(&held); if (held.clock_valid && held.now >= W) writer_errors++;
        sample(T + 30 + (int64_t)i * 15);
    }
    void *result = NULL; CHECK_EQ(pthread_join(thread, &result), 0);
    CHECK(result == NULL); CHECK_EQ(writer_errors, 0);
}
int main(int argc, char **argv)
{
    if (argc != 2) return 2;
    if (!strcmp(argv[1], "initial")) RUN_TEST(test_bad_initial_sample_then_correction_and_restore);
    else if (!strcmp(argv[1], "outlier")) RUN_TEST(test_established_outlier_cannot_change_day_or_retirement);
    else if (!strcmp(argv[1], "reboot")) RUN_TEST(test_rebooted_clock_reestablishes_without_losing_pause);
    else if (!strcmp(argv[1], "concurrent")) RUN_TEST(test_concurrent_clock_snapshot);
    else return 2;
    return mt_summary();
}
