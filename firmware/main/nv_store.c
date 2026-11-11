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

/* Active ids already live in the v1 snapshot. Only persist its extension;
 * duplicating that 2472-byte prefix exhausts the default four-page NVS during
 * copy-on-write replacement. The small safety prefix preserves conservative
 * counters/faults when a multi-key save was interrupted. */
typedef struct {
    unsigned char safety[offsetof(gate_state_t, seen_ids)];
    uint32_t format;
    int64_t seen_expires_at;
    int64_t clock_high_water;
    uint64_t mirror_hash;
    /* 36 printable ASCII symbols at 7 bits each fit in 32 bytes. Code zero
     * terminates shorter ids; the original 37-byte RAM representation stays. */
    unsigned char legacy_ids[GATE_LEGACY_IDS][32];
    uint16_t legacy_count;
    uint16_t active_count;
    uint16_t active_head;
    bool replay_untrusted;
} gate_metadata_t;

#define GATE_METADATA_FORMAT 1u
#if GATE_ID_MAX_LEN != 36
#error "packed legacy-id format requires the existing 36-character gate contract"
#endif
static gate_metadata_t s_metadata;

/* Pairing checksum for interrupted writes, not an authentication mechanism. */
static uint64_t snapshot_hash(const void *data, size_t len)
{
    const unsigned char *bytes = data;
    uint64_t hash = UINT64_C(14695981039346656037);
    for (size_t i = 0; i < len; i++) {
        hash = (hash ^ bytes[i]) * UINT64_C(1099511628211);
    }
    return hash;
}

/* Lossless representation of the gate's existing printable-ASCII contract.
 * No UUID restriction: every 0x21..0x7e character remains representable. */
static bool pack_legacy(const gate_state_t *st)
{
    for (uint16_t i = 0; i < st->legacy_count; i++) {
        bool ended = false;
        if (st->legacy_ids[i][0] == '\0' || st->legacy_ids[i][GATE_ID_MAX_LEN] != '\0') return false;
        for (size_t j = 0; j < GATE_ID_MAX_LEN; j++) {
            unsigned char c = ended ? 0 : (unsigned char)st->legacy_ids[i][j];
            if (c == 0) ended = true;
            else if (c < 0x21 || c > 0x7e) return false;
            const unsigned value = c ? c - 0x20u : 0;
            const size_t bit = j * 7;
            s_metadata.legacy_ids[i][bit / 8] |= (unsigned char)(value << (bit % 8));
            if (bit % 8 > 1) s_metadata.legacy_ids[i][bit / 8 + 1] |= (unsigned char)(value >> (8 - bit % 8));
        }
    }
    return true;
}

static bool unpack_legacy(gate_state_t *st)
{
    for (uint16_t i = 0; i < st->legacy_count; i++) {
        bool ended = false;
        if (s_metadata.legacy_ids[i][31] & 0xf0u) return false;
        for (size_t j = 0; j < GATE_ID_MAX_LEN; j++) {
            const size_t bit = j * 7;
            unsigned value = s_metadata.legacy_ids[i][bit / 8] >> (bit % 8);
            if (bit % 8 > 1) value |= (unsigned)s_metadata.legacy_ids[i][bit / 8 + 1] << (8 - bit % 8);
            value &= 0x7fu;
            if (value > 94 || (ended && value)) return false;
            if (!value) ended = true;
            st->legacy_ids[i][j] = value ? (char)(value + 0x20u) : '\0';
        }
        st->legacy_ids[i][GATE_ID_MAX_LEN] = '\0';
        if (st->legacy_ids[i][0] == '\0') return false;
    }
    return true;
}

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
    size_t v2_size = sizeof s_metadata, v1_size = GATE_STATE_V1_SIZE;
    const esp_err_t v2_err = nvs_get_blob(h, KEY_GATE_V2, &s_metadata, &v2_size);
    const esp_err_t v1_err = nvs_get_blob(h, KEY_GATE, &s_legacy, &v1_size);
    nvs_close(h);
    if (v2_err == ESP_ERR_NVS_NOT_FOUND && v1_err == ESP_ERR_NVS_NOT_FOUND) return false;
    const bool v1_ok = v1_err == ESP_OK && v1_size == GATE_STATE_V1_SIZE &&
                       s_legacy.magic == GATE_STATE_MAGIC && s_legacy.version == 1u;
    const bool v2_ok = v2_err == ESP_OK && v2_size == sizeof s_metadata &&
                       s_metadata.format == GATE_METADATA_FORMAT &&
                       s_metadata.active_count <= GATE_SEEN_IDS &&
                       s_metadata.legacy_count <= GATE_LEGACY_IDS;
    if (v2_ok) {
        memcpy(out, s_metadata.safety, sizeof s_metadata.safety);
        out->seen_expires_at = s_metadata.seen_expires_at;
        out->clock_high_water = s_metadata.clock_high_water;
        out->legacy_count = s_metadata.legacy_count;
        out->seen_count = s_metadata.active_count;
        out->seen_head = s_metadata.active_head;
        out->replay_untrusted = s_metadata.replay_untrusted;
        if (!unpack_legacy(out)) goto corrupt;
        const bool ids_ok = v1_ok && s_legacy.seen_count <= GATE_SEEN_IDS &&
                            s_legacy.seen_count >= out->seen_count;
        if (ids_ok) {
            memcpy(out->seen_ids, s_legacy.seen_ids + s_legacy.seen_count - out->seen_count,
                   out->seen_count * GATE_ID_BUF);
        }
        /* NVS commits individual keys; a reset may interrupt a multi-key save.
         * A mismatched pair must never start from an empty, running gate. */
        if (!ids_ok || snapshot_hash(&s_legacy, GATE_STATE_V1_SIZE) != s_metadata.mirror_hash) {
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
    memset(&s_metadata, 0, sizeof s_metadata);
    memcpy(s_metadata.safety, st, sizeof s_metadata.safety);
    s_metadata.format = GATE_METADATA_FORMAT;
    s_metadata.seen_expires_at = st->seen_expires_at;
    s_metadata.clock_high_water = st->clock_high_water;
    s_metadata.mirror_hash = snapshot_hash(s_v1_blob, sizeof s_v1_blob);
    if (!pack_legacy(st)) {
        nvs_close(h);
        return ESP_ERR_INVALID_ARG;
    }
    s_metadata.legacy_count = st->legacy_count;
    s_metadata.active_count = st->seen_count;
    s_metadata.active_head = st->seen_head;
    s_metadata.replay_untrusted = st->replay_untrusted;
    err = nvs_set_blob(h, KEY_GATE_V2, &s_metadata, sizeof s_metadata);
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
