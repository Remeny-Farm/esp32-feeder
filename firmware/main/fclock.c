#include "fclock.h"

#include <stdlib.h>
#include <sys/time.h>
#include <time.h>

#include "esp_log.h"
#include "esp_sntp.h"
#include "feeder_util.h"
#include "sdkconfig.h"

static const char *TAG = "clock";
static volatile bool s_synced; /* written in the SNTP (tcpip) task */
static bool s_sntp_started;

void fclock_init(void)
{
    setenv("TZ", CONFIG_FEEDER_TZ, 1);
    tzset();
}

static void on_sync(struct timeval *tv)
{
    /* A reply before the gate's minimum epoch is not a usable clock. */
    if (tv != NULL && (int64_t)tv->tv_sec >= GATE_MIN_VALID_EPOCH) {
        s_synced = true;
        ESP_LOGI(TAG, "SNTP synced");
    } else {
        ESP_LOGW(TAG, "SNTP time looks wrong, clock stays invalid");
    }
}

esp_err_t fclock_sntp_start(void)
{
    if (s_sntp_started) {
        return ESP_OK;
    }
    s_sntp_started = true;
    esp_sntp_setoperatingmode(ESP_SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, CONFIG_FEEDER_NTP_SERVER); /* literal: lives forever */
    esp_sntp_set_time_sync_notification_cb(on_sync);
    esp_sntp_init();
    ESP_LOGI(TAG, "SNTP started, server %s", CONFIG_FEEDER_NTP_SERVER);
    return ESP_OK;
}

bool fclock_valid(void)
{
    return s_synced;
}

void fclock_get(gate_time_t *tm)
{
    const time_t now = time(NULL);
    struct tm lt;
    localtime_r(&now, &lt);
    tm->now = (int64_t)now;
    tm->clock_valid = s_synced && tm->now >= GATE_MIN_VALID_EPOCH;
    tm->local_minute = (uint16_t)(lt.tm_hour * 60 + lt.tm_min);
    /* Days of the local calendar date. Not (now + tm_gmtoff) / 86400: newlib
     * may not fill tm_gmtoff. The civil date already includes the DST offset. */
    tm->local_day = days_from_civil(lt.tm_year + 1900, (unsigned)(lt.tm_mon + 1), (unsigned)lt.tm_mday);
}

bool fclock_now_rfc3339(char *buf, size_t len)
{
    gate_time_t tm;
    fclock_get(&tm);
    if (!tm.clock_valid || rfc3339_format(tm.now, buf, len) == 0) {
        if (len > 0) {
            buf[0] = '\0';
        }
        return false;
    }
    return true;
}
