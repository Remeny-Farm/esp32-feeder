#include "gate_config.h"

#include <string.h>

#include "sdkconfig.h"

static const char *const k_names[GATE_MAX_HOPPERS] = {
    CONFIG_FEEDER_HOPPER_NAME_0, CONFIG_FEEDER_HOPPER_NAME_1, CONFIG_FEEDER_HOPPER_NAME_2};
static const uint32_t k_random_dose[GATE_MAX_HOPPERS] = {
    CONFIG_FEEDER_RANDOM_DOSE_G_0, CONFIG_FEEDER_RANDOM_DOSE_G_1, CONFIG_FEEDER_RANDOM_DOSE_G_2};

void gate_config_limits(gate_limits_t *out)
{
    memset(out, 0, sizeof *out);
    out->hopper_count = CONFIG_FEEDER_HOPPER_COUNT;
    out->max_dose_g[0] = CONFIG_FEEDER_MAX_DOSE_G_0;
    out->max_dose_g[1] = CONFIG_FEEDER_MAX_DOSE_G_1;
    out->max_dose_g[2] = CONFIG_FEEDER_MAX_DOSE_G_2;
    out->daily_budget_g[0] = CONFIG_FEEDER_DAILY_BUDGET_G_0;
    out->daily_budget_g[1] = CONFIG_FEEDER_DAILY_BUDGET_G_1;
    out->daily_budget_g[2] = CONFIG_FEEDER_DAILY_BUDGET_G_2;
    out->daily_total_g = CONFIG_FEEDER_DAILY_TOTAL_G;
    out->min_interval_s = CONFIG_FEEDER_MIN_INTERVAL_S;
    out->quiet_start_min = CONFIG_FEEDER_QUIET_START_MIN;
    out->quiet_end_min = CONFIG_FEEDER_QUIET_END_MIN;
}

uint8_t feeder_hopper_count(void)
{
    return CONFIG_FEEDER_HOPPER_COUNT;
}

const char *feeder_hopper_name(uint8_t i)
{
    return i < CONFIG_FEEDER_HOPPER_COUNT ? k_names[i] : "";
}

int feeder_hopper_index(const char *name)
{
    if (name == NULL) {
        return -1;
    }
    for (int i = 0; i < CONFIG_FEEDER_HOPPER_COUNT; i++) {
        if (strcmp(name, k_names[i]) == 0) {
            return i;
        }
    }
    return -1;
}

uint32_t feeder_random_dose_g(uint8_t i)
{
    return i < GATE_MAX_HOPPERS ? k_random_dose[i] : 0;
}

bool feeder_device_id_valid(const char *id)
{
    if (id == NULL) {
        return false;
    }
    const size_t n = strlen(id);
    if (n < 3 || n > 32) {
        return false;
    }
    for (size_t i = 0; i < n; i++) {
        const char c = id[i];
        const bool alnum = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9');
        if (!alnum && !(i > 0 && c == '-')) {
            return false;
        }
    }
    return true;
}

bool feeder_config_valid(void)
{
    if (CONFIG_FEEDER_HOPPER_COUNT < 1 || CONFIG_FEEDER_HOPPER_COUNT > GATE_MAX_HOPPERS) {
        return false;
    }
    if (!feeder_device_id_valid(CONFIG_FEEDER_DEVICE_ID)) {
        return false;
    }
    for (int i = 0; i < CONFIG_FEEDER_HOPPER_COUNT; i++) {
        if (k_names[i][0] == '\0') {
            return false;
        }
        for (int j = 0; j < i; j++) {
            if (strcmp(k_names[i], k_names[j]) == 0) {
                return false;
            }
        }
    }
    return true;
}
