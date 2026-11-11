#include "nv_store.h"

#include <string.h>

#include "esp_log.h"
#include "nvs.h"

#define NS "feeder"
#define KEY_GATE "gate" /* v1-compatible snapshot, also read by a rolled-back image */
#define KEY_GATE_V2 "gate_v2"
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

/* The feeder task is the sole caller. Keep snapshot buffers off its stack. */
static gate_state_t s_legacy;
static unsigned char s_v1_blob[GATE_STATE_V1_SIZE];

/* Keep the last active ids plus as many old quarantined ids as fit in v1's
 * bounded ring. A rollback must not discard all legacy ids on its first boot. */
static void v1_snapshot(const gate_state_t *st)
{
    memcpy(s_v1_blob, st, sizeof s_v1_blob);
    const uint32_t version = 1u;
    memcpy(s_v1_blob + offsetof(gate_state_t, version), &version, sizeof version);
    char (*ids)[GATE_ID_BUF] = (void *)(s_v1_blob + offsetof(gate_state_t, seen_ids));
    uint16_t old_count = st->legacy_count;
    if (old_count > GATE_SEEN_IDS - st->seen_count) old_count = GATE_SEEN_IDS - st->seen_count;
    memset(ids, 0, GATE_SEEN_IDS * GATE_ID_BUF);
    memcpy(ids, st->legacy_ids + st->legacy_count - old_count, old_count * GATE_ID_BUF);
    memcpy(ids + old_count, st->seen_ids, st->seen_count * GATE_ID_BUF);
    const uint16_t count = old_count + st->seen_count;
    const uint16_t next = count == GATE_SEEN_IDS ? 0 : count;
    memcpy(s_v1_blob + offsetof(gate_state_t, seen_count), &count, sizeof count);
    memcpy(s_v1_blob + offsetof(gate_state_t, seen_head), &next, sizeof next);
}

static void disable_replay(gate_state_t *out)
{
    out->replay_untrusted = true;
    out->paused = true;
    out->fault_mask |= GATE_FAULT_WATCHDOG;
}

bool nv_load_gate(gate_state_t *out)
{
    memset(out, 0, sizeof *out);
    memset(&s_legacy, 0, sizeof s_legacy);
    nvs_handle_t h;
    const esp_err_t opened = nvs_open(NS, NVS_READONLY, &h);
    if (opened != ESP_OK) {
        if (opened == ESP_ERR_NVS_NOT_FOUND) return false;
        goto corrupt;
    }
    size_t v2_size = sizeof *out, v1_size = GATE_STATE_V1_SIZE;
    const esp_err_t v2_err = nvs_get_blob(h, KEY_GATE_V2, out, &v2_size);
    const esp_err_t v1_err = nvs_get_blob(h, KEY_GATE, &s_legacy, &v1_size);
    nvs_close(h);
    if (v2_err == ESP_ERR_NVS_NOT_FOUND && v1_err == ESP_ERR_NVS_NOT_FOUND) return false;
    const bool v1_ok = v1_err == ESP_OK && v1_size == GATE_STATE_V1_SIZE &&
                       s_legacy.magic == GATE_STATE_MAGIC && s_legacy.version == 1u;
    const bool v2_ok = v2_err == ESP_OK && v2_size == sizeof *out &&
                       out->magic == GATE_STATE_MAGIC && out->version == GATE_STATE_VERSION &&
                       out->seen_count <= GATE_SEEN_IDS && out->legacy_count <= GATE_LEGACY_IDS;
    if (v2_ok) {
        /* NVS commits individual keys; a reset may interrupt a multi-key save.
         * A mismatched pair must never start from an empty, running gate. */
        v1_snapshot(out);
        if (!v1_ok || memcmp(s_v1_blob, &s_legacy, GATE_STATE_V1_SIZE) != 0) {
            if (v1_ok) {
                if (s_legacy.day > out->day) out->day = s_legacy.day;
                for (uint8_t i = 0; i < GATE_MAX_HOPPERS; i++) {
                    if (s_legacy.used_g[i] > out->used_g[i]) out->used_g[i] = s_legacy.used_g[i];
                }
                if (s_legacy.used_total_g > out->used_total_g) out->used_total_g = s_legacy.used_total_g;
                out->fault_mask |= s_legacy.fault_mask;
            }
            disable_replay(out);
        }
        return true;
    }
    if (v1_ok) {
        /* Unknown legacy expiries stay in a separate bounded quarantine. They
         * do not consume slots in the new expiring active cache. */
        *out = s_legacy;
        out->version = GATE_STATE_VERSION;
        if (out->seen_count > GATE_LEGACY_IDS || out->seen_head >= GATE_SEEN_IDS) goto corrupt;
        out->legacy_count = out->seen_count;
        for (uint16_t i = 0; i < out->legacy_count; i++) {
            const uint16_t slot = out->seen_count == GATE_SEEN_IDS ?
                                  (out->seen_head + i) % GATE_SEEN_IDS : i;
            memcpy(out->legacy_ids[i], out->seen_ids[slot], GATE_ID_BUF);
        }
        memset(out->seen_ids, 0, sizeof out->seen_ids);
        out->seen_count = out->seen_head = 0;
        if (v2_err != ESP_ERR_NVS_NOT_FOUND) disable_replay(out);
        return true;
    }
corrupt:
    /* No trustworthy full snapshot. Preserve whatever counters were read and
     * prevent all admission, including after a service-button fault clear. */
    out->magic = GATE_STATE_MAGIC;
    out->version = GATE_STATE_VERSION;
    gate_runtime_limits_clear(&out->runtime);
    disable_replay(out);
    return true;
}

esp_err_t nv_save_gate(const gate_state_t *st)
{
    if (st->seen_count > GATE_SEEN_IDS || st->legacy_count > GATE_LEGACY_IDS) {
        return ESP_ERR_INVALID_ARG;
    }
    nvs_handle_t h;
    esp_err_t err = nvs_open(NS, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    v1_snapshot(st);
    /* Keep rollback's original key and layout usable. Save succeeds only after
     * both snapshots have been written and committed; actuation waits for it. */
    err = nvs_set_blob(h, KEY_GATE_V2, st, sizeof *st);
    if (err == ESP_OK) err = nvs_set_blob(h, KEY_GATE, s_v1_blob, sizeof s_v1_blob);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    return err;
}

bool nv_load_sched(sched_cfg_t *out)
{
    return load_blob(KEY_SCHED, out, sizeof *out) && sched_cfg_valid(out);
}

esp_err_t nv_save_sched(const sched_cfg_t *cfg)
{
    return save_blob(KEY_SCHED, cfg, sizeof *cfg);
}
