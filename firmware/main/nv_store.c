#include "nv_store.h"

#include <string.h>

#include "esp_log.h"
#include "nvs.h"

#define NS "feeder"
#define KEY_GATE "gate"
#define KEY_SCHED "sched"

static const char *TAG = "nv";

static bool load_blob_size(const char *key, void *buf, size_t size, size_t *actual)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NS, NVS_READONLY, &h);
    if (err != ESP_OK) {
        if (err != ESP_ERR_NVS_NOT_FOUND) {
            ESP_LOGW(TAG, "open: %s", esp_err_to_name(err));
        }
        return false;
    }
    size_t len = size;
    err = nvs_get_blob(h, key, buf, &len);
    nvs_close(h);
    if (err != ESP_OK || len > size) {
        if (err != ESP_ERR_NVS_NOT_FOUND) {
            ESP_LOGW(TAG, "read %s: %s (len %u, want %u)", key, esp_err_to_name(err), (unsigned)len,
                     (unsigned)size);
        }
        return false;
    }
    *actual = len;
    return true;
}

static bool load_blob(const char *key, void *buf, size_t size)
{
    size_t actual;
    return load_blob_size(key, buf, size, &actual) && actual == size;
}

static esp_err_t save_blob(const char *key, const void *buf, size_t size)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_set_blob(h, key, buf, size);
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    return err;
}

bool nv_load_gate(gate_state_t *out)
{
    memset(out, 0, sizeof *out);
    size_t actual;
    if (!load_blob_size(KEY_GATE, out, sizeof *out, &actual)) {
        return false;
    }
    if (actual == GATE_STATE_V1_SIZE && out->magic == GATE_STATE_MAGIC && out->version == 1u) {
        /* Preserve pause, budgets and every legacy id. Their expiries were not
         * stored, so none can safely be forgotten automatically after upgrade. */
        out->version = GATE_STATE_VERSION;
        out->seen_expires_at = out->seen_count ? INT64_MAX : 0;
        return true;
    }
    return actual == sizeof *out;
}

esp_err_t nv_save_gate(const gate_state_t *st)
{
    return save_blob(KEY_GATE, st, sizeof *st);
}

bool nv_load_sched(sched_cfg_t *out)
{
    return load_blob(KEY_SCHED, out, sizeof *out) && sched_cfg_valid(out);
}

esp_err_t nv_save_sched(const sched_cfg_t *cfg)
{
    return save_blob(KEY_SCHED, cfg, sizeof *cfg);
}
