#include "fclock.h"

#include <stdlib.h>
#include <sys/time.h>
#include <time.h>

#include "esp_log.h"
#include "esp_sntp.h"
#include "esp_timer.h"
#include "feeder_util.h"
#include "freertos/FreeRTOS.h"
#include "sdkconfig.h"

#define CONFIRM_INTERVAL_MS 15000u /* SDK's minimum supported SNTP interval */
static const char *TAG = "clock";
static bool s_sntp_started;
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static struct {
    bool valid, candidate;
    int64_t wall, candidate_wall;
    uint64_t mono_us, candidate_mono_us;
} s_clock;

/* One writer (the SNTP tcpip task); feeder readers copy the entire established
 * pair under the lock. Never expose the global system clock's intermediate
 * update to gate_used_today(), the scheduler, or replay retirement. */
static bool project(int64_t wall, uint64_t mono, uint64_t now, int64_t *out)
{
    if (now < mono) return false;
    const uint64_t elapsed = (now - mono) / 1000000u;
    if (wall < 0 || elapsed > (uint64_t)(INT64_MAX - wall)) return false;
    *out = wall + (int64_t)elapsed;
    return true;
}

static bool agrees(int64_t sample, int64_t wall, uint64_t mono, uint64_t now)
{
    int64_t expected;
    if (!project(wall, mono, now, &expected)) return false;
    const uint64_t difference = sample >= expected ? (uint64_t)sample - (uint64_t)expected :
                                                   (uint64_t)expected - (uint64_t)sample;
    return difference <= GATE_MAX_FUTURE_S;
}

void fclock_init(void)
{
    setenv("TZ", CONFIG_FEEDER_TZ, 1);
    tzset();
}

/* ESP-IDF explicitly provides this weak hook BEFORE settimeofday(). A callback
 * after the SDK's setter is too late: it would already affect daily budgets.
 * Two consistent replies establish the same configured, trusted SNTP source;
 * this is isolated-outlier filtering, not authentication of malicious peers. */
void sntp_sync_time(struct timeval *tv)
{
    const uint64_t mono = (uint64_t)esp_timer_get_time();
    if (tv == NULL || tv->tv_sec < GATE_MIN_VALID_EPOCH ||
        (uint64_t)tv->tv_sec > (uint64_t)INT64_MAX / 1000000u || tv->tv_usec < 0 || tv->tv_usec >= 1000000) {
        esp_sntp_set_sync_interval(CONFIRM_INTERVAL_MS);
        return;
    }
    const int64_t sample = (int64_t)tv->tv_sec;
    bool accept;
    portENTER_CRITICAL(&s_lock);
    const bool same_candidate = s_clock.candidate &&
        agrees(sample, s_clock.candidate_wall, s_clock.candidate_mono_us, mono);
    const bool confirmed = same_candidate && mono >= s_clock.candidate_mono_us &&
        mono - s_clock.candidate_mono_us >= (uint64_t)CONFIRM_INTERVAL_MS * 1000u;
    accept = (s_clock.valid && agrees(sample, s_clock.wall, s_clock.mono_us, mono)) || confirmed;
    if (!accept && !same_candidate) {
        s_clock.candidate = true;
        s_clock.candidate_wall = sample;
        s_clock.candidate_mono_us = mono;
    }
    portEXIT_CRITICAL(&s_lock);
    if (!accept) {
        esp_sntp_set_sync_interval(CONFIRM_INTERVAL_MS);
        esp_sntp_set_sync_status(SNTP_SYNC_STATUS_RESET);
        ESP_LOGW(TAG, "SNTP sample held for confirmation");
        return;
    }
    /* The SDK invokes the hook serially from its tcpip task. Keep blocking OS
     * work outside the lock; readers continue using the old established pair. */
    if (settimeofday(tv, NULL) != 0) {
        esp_sntp_set_sync_interval(CONFIRM_INTERVAL_MS);
        ESP_LOGW(TAG, "SNTP system clock update failed");
        return;
    }
    portENTER_CRITICAL(&s_lock);
    s_clock.wall = sample;
    s_clock.mono_us = mono;
    s_clock.valid = true;
    s_clock.candidate = false;
    portEXIT_CRITICAL(&s_lock);
    esp_sntp_set_sync_status(SNTP_SYNC_STATUS_COMPLETED);
    esp_sntp_set_sync_interval(CONFIG_LWIP_SNTP_UPDATE_DELAY);
    ESP_LOGI(TAG, "SNTP clock established");
}

esp_err_t fclock_sntp_start(void)
{
    if (s_sntp_started) return ESP_OK;
    s_sntp_started = true;
    esp_sntp_setoperatingmode(ESP_SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, CONFIG_FEEDER_NTP_SERVER);
    esp_sntp_set_sync_interval(CONFIRM_INTERVAL_MS);
    esp_sntp_init();
    ESP_LOGI(TAG, "SNTP started, server %s", CONFIG_FEEDER_NTP_SERVER);
    return ESP_OK;
}

void fclock_get(gate_time_t *tm)
{
    const uint64_t mono = (uint64_t)esp_timer_get_time();
    int64_t wall = 0;
    bool valid;
    portENTER_CRITICAL(&s_lock);
    valid = s_clock.valid && project(s_clock.wall, s_clock.mono_us, mono, &wall);
    portEXIT_CRITICAL(&s_lock);
    *tm = (gate_time_t){.now = wall, .monotonic_s = mono / 1000000u, .clock_valid = false};
    if (!valid) return;
    const time_t now = (time_t)wall;
    struct tm lt;
    if (localtime_r(&now, &lt) == NULL) return;
    tm->clock_valid = true;
    tm->local_minute = (uint16_t)(lt.tm_hour * 60 + lt.tm_min);
    tm->local_day = days_from_civil(lt.tm_year + 1900, (unsigned)(lt.tm_mon + 1), (unsigned)lt.tm_mday);
}

bool fclock_valid(void)
{
    gate_time_t tm;
    fclock_get(&tm);
    return tm.clock_valid;
}

bool fclock_now_rfc3339(char *buf, size_t len)
{
    gate_time_t tm;
    fclock_get(&tm);
    if (!tm.clock_valid || rfc3339_format(tm.now, buf, len) == 0) {
        if (len > 0) buf[0] = '\0';
        return false;
    }
    return true;
}
