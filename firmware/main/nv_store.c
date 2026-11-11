#include "nv_store.h"

#include <string.h>

#include "nvs.h"

#define NS "feeder"
#define KEY_GATE "gate" /* deployed v1 rollback image reads this exact prefix */
#define KEY_METADATA "gate_v2"
#define KEY_LEGACY "gate_legacy"
#define KEY_SCHED "sched"
#define METADATA_FORMAT 2u
#define LEGACY_FORMAT 1u
#define FLAG_PAUSED 1u
#define FLAG_UNTRUSTED 2u
#define EXPIRY_UNKNOWN (-INT64_C(549755813888)) /* signed40 minimum, outside RFC3339 years 0000..9999 */
#define EXPIRY_MAX INT64_C(549755813887)

/* Mutable recovery/expiry extension. Active ids already live in the v1 mirror. */
typedef struct {
    uint32_t format, faults;
    int64_t day;
    uint32_t used_g[GATE_MAX_HOPPERS], used_total;
    uint64_t mirror_hash, legacy_hash;
    int64_t retired_floor;
    uint16_t active_count, active_head, legacy_count;
    uint8_t flags;
    uint8_t expiries[GATE_SEEN_IDS][5];
} gate_metadata_t;

typedef struct {
    uint32_t format;
    uint16_t count, reserved;
    uint8_t ids[GATE_LEGACY_IDS][30];
} legacy_blob_t;

typedef char metadata_size_must_be_384[(sizeof(gate_metadata_t) == 384) ? 1 : -1];
typedef char legacy_size_must_be_1928[(sizeof(legacy_blob_t) == 1928) ? 1 : -1];
#if GATE_ID_MAX_LEN != 36
#error "legacy encoding requires the existing 36-character gate contract"
#endif

/* The feeder task is the sole caller; keep large snapshots off its stack. */
static gate_state_t s_v1;
static uint8_t s_mirror[GATE_STATE_V1_SIZE];
static gate_metadata_t s_metadata;
static legacy_blob_t s_legacy;

/* Pairing checksums detect interrupted writes; they are not authentication. */
static uint64_t snapshot_hash(const void *data, size_t len)
{
    const uint8_t *bytes = data;
    uint64_t hash = UINT64_C(14695981039346656037);
    for (size_t i = 0; i < len; i++) hash = (hash ^ bytes[i]) * UINT64_C(1099511628211);
    return hash;
}

/* Base95: zero terminates; 1..94 encode the complete printable 0x21..0x7e range.
 * 95^36 < 2^240, so no accepted gate id is truncated or replaced by a hash. */
static bool pack_legacy(const gate_state_t *st)
{
    memset(&s_legacy, 0, sizeof s_legacy);
    s_legacy.format = LEGACY_FORMAT;
    s_legacy.count = st->legacy_count;
    for (uint16_t i = 0; i < st->legacy_count; i++) {
        bool ended = false;
        if (!st->legacy_ids[i][0] || st->legacy_ids[i][GATE_ID_MAX_LEN]) return false;
        for (size_t j = 0; j < GATE_ID_MAX_LEN; j++) {
            const uint8_t c = ended ? 0 : (uint8_t)st->legacy_ids[i][j];
            if (!c) ended = true;
            else if (c < 0x21 || c > 0x7e) return false;
            unsigned carry = c ? c - 0x20u : 0;
            for (size_t b = 0; b < 30; b++) {
                const unsigned value = s_legacy.ids[i][b] * 95u + carry;
                s_legacy.ids[i][b] = (uint8_t)value;
                carry = value >> 8;
            }
            if (carry) return false;
        }
    }
    return true;
}

static bool unpack_legacy(gate_state_t *st)
{
    if (s_legacy.format != LEGACY_FORMAT || s_legacy.reserved || s_legacy.count > GATE_LEGACY_IDS) return false;
    st->legacy_count = s_legacy.count;
    for (uint16_t i = 0; i < GATE_LEGACY_IDS; i++) {
        if (i >= st->legacy_count) {
            for (size_t b = 0; b < 30; b++) if (s_legacy.ids[i][b]) return false;
            continue;
        }
        uint8_t value[30], digits[GATE_ID_MAX_LEN];
        memcpy(value, s_legacy.ids[i], sizeof value);
        for (size_t j = GATE_ID_MAX_LEN; j > 0; j--) {
            unsigned remainder = 0;
            for (size_t b = 30; b > 0; b--) {
                const unsigned n = (remainder << 8) | value[b - 1];
                value[b - 1] = (uint8_t)(n / 95u);
                remainder = n % 95u;
            }
            digits[j - 1] = (uint8_t)remainder;
        }
        for (size_t b = 0; b < 30; b++) if (value[b]) return false;
        bool ended = false;
        for (size_t j = 0; j < GATE_ID_MAX_LEN; j++) {
            if (ended && digits[j]) return false;
            if (!digits[j]) ended = true;
            st->legacy_ids[i][j] = digits[j] ? (char)(digits[j] + 0x20u) : '\0';
        }
        st->legacy_ids[i][GATE_ID_MAX_LEN] = '\0';
        if (!st->legacy_ids[i][0]) return false;
    }
    return true;
}

/* Signed40 is exact for all four-digit RFC3339 dates, including negative
 * epochs. Wider internal timestamps conservatively become unknown, never an
 * early expiry. INT64_MAX roundtrips as the same unknown marker. */
static void pack_expiry(int64_t expiry, uint8_t bytes[5])
{
    if (expiry <= EXPIRY_UNKNOWN || expiry > EXPIRY_MAX) expiry = EXPIRY_UNKNOWN;
    const uint64_t bits = (uint64_t)expiry & UINT64_C(0xffffffffff);
    for (size_t b = 0; b < 5; b++) bytes[b] = (uint8_t)(bits >> (b * 8));
}

static int64_t unpack_expiry(const uint8_t bytes[5])
{
    uint64_t bits = 0;
    for (size_t b = 0; b < 5; b++) bits |= (uint64_t)bytes[b] << (b * 8);
    if (bits == UINT64_C(0x8000000000)) return INT64_MAX;
    if (bits & UINT64_C(0x8000000000)) return -(int64_t)(UINT64_C(0x10000000000) - bits);
    return (int64_t)bits;
}

static void v1_snapshot(const gate_state_t *st)
{
    memcpy(s_mirror, st, sizeof s_mirror);
    const uint32_t version = 1u;
    memcpy(s_mirror + offsetof(gate_state_t, version), &version, sizeof version);
    char (*ids)[GATE_ID_BUF] = (void *)(s_mirror + offsetof(gate_state_t, seen_ids));
    uint16_t legacy = st->legacy_count;
    if (legacy > GATE_SEEN_IDS - st->seen_count) legacy = GATE_SEEN_IDS - st->seen_count;
    memset(ids, 0, GATE_SEEN_IDS * GATE_ID_BUF);
    memcpy(ids, st->legacy_ids + st->legacy_count - legacy, legacy * GATE_ID_BUF);
    memcpy(ids + legacy, st->seen_ids, st->seen_count * GATE_ID_BUF);
    const uint16_t count = legacy + st->seen_count;
    const uint16_t next = count == GATE_SEEN_IDS ? 0 : count;
    memcpy(s_mirror + offsetof(gate_state_t, seen_count), &count, sizeof count);
    memcpy(s_mirror + offsetof(gate_state_t, seen_head), &next, sizeof next);
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
    memset(&s_v1, 0, sizeof s_v1);
    memset(&s_metadata, 0, sizeof s_metadata);
    memset(&s_legacy, 0, sizeof s_legacy);
    out->magic = GATE_STATE_MAGIC; out->version = GATE_STATE_VERSION;
    gate_runtime_limits_clear(&out->runtime);
    nvs_handle_t h;
    const esp_err_t opened = nvs_open(NS, NVS_READONLY, &h);
    if (opened != ESP_OK) {
        if (opened == ESP_ERR_NVS_NOT_FOUND) return false;
        disable_replay(out); return true;
    }
    size_t v1_size = GATE_STATE_V1_SIZE, meta_size = sizeof s_metadata, legacy_size = sizeof s_legacy;
    const esp_err_t v1_err = nvs_get_blob(h, KEY_GATE, &s_v1, &v1_size);
    const esp_err_t meta_err = nvs_get_blob(h, KEY_METADATA, &s_metadata, &meta_size);
    const esp_err_t legacy_err = nvs_get_blob(h, KEY_LEGACY, &s_legacy, &legacy_size);
    nvs_close(h);
    if (v1_err == ESP_ERR_NVS_NOT_FOUND && meta_err == ESP_ERR_NVS_NOT_FOUND && legacy_err == ESP_ERR_NVS_NOT_FOUND) return false;
    const bool v1_ok = v1_err == ESP_OK && v1_size == GATE_STATE_V1_SIZE &&
                       s_v1.magic == GATE_STATE_MAGIC && s_v1.version == 1u;
    if (v1_ok) {
        memcpy(out, &s_v1, GATE_STATE_V1_SIZE);
        out->version = GATE_STATE_VERSION;
    }
    const bool meta_ok = meta_err == ESP_OK && meta_size == sizeof s_metadata &&
                         s_metadata.format == METADATA_FORMAT &&
                         s_metadata.active_count <= GATE_SEEN_IDS && s_metadata.active_head < GATE_SEEN_IDS &&
                         s_metadata.legacy_count <= GATE_LEGACY_IDS &&
                         !(s_metadata.flags & ~(FLAG_PAUSED | FLAG_UNTRUSTED));
    if (meta_ok) {
        if (!v1_ok) {
            out->day = s_metadata.day;
            memcpy(out->used_g, s_metadata.used_g, sizeof out->used_g);
            out->used_total_g = s_metadata.used_total;
            out->fault_mask = s_metadata.faults;
            out->paused = (s_metadata.flags & FLAG_PAUSED) != 0;
        }
        out->retired_expiry_floor = s_metadata.retired_floor;
        if (out->retired_expiry_floor < 0) {
            out->retired_expiry_floor = 0;
            disable_replay(out);
        }
        out->seen_count = s_metadata.active_count;
        out->seen_head = s_metadata.active_head;
        if (s_metadata.flags & FLAG_UNTRUSTED) disable_replay(out);
        for (uint16_t i = 0; i < GATE_SEEN_IDS; i++) {
            if (i < out->seen_count) out->seen_expires_at[i] = unpack_expiry(s_metadata.expiries[i]);
            else for (size_t b = 0; b < 5; b++) if (s_metadata.expiries[i][b]) disable_replay(out);
        }
        const bool legacy_ok = legacy_err == ESP_OK && legacy_size == sizeof s_legacy &&
                               unpack_legacy(out) && out->legacy_count == s_metadata.legacy_count;
        const bool ids_ok = v1_ok && s_v1.seen_count <= GATE_SEEN_IDS && s_v1.seen_count >= out->seen_count;
        memset(out->seen_ids, 0, sizeof out->seen_ids);
        if (ids_ok) memcpy(out->seen_ids, s_v1.seen_ids + s_v1.seen_count - out->seen_count, out->seen_count * GATE_ID_BUF);
        if (!ids_ok || !legacy_ok || snapshot_hash(&s_v1, GATE_STATE_V1_SIZE) != s_metadata.mirror_hash ||
            snapshot_hash(&s_legacy, sizeof s_legacy) != s_metadata.legacy_hash) {
            if (s_metadata.day > out->day) out->day = s_metadata.day;
            for (uint8_t i = 0; i < GATE_MAX_HOPPERS; i++) if (s_metadata.used_g[i] > out->used_g[i]) out->used_g[i] = s_metadata.used_g[i];
            if (s_metadata.used_total > out->used_total_g) out->used_total_g = s_metadata.used_total;
            out->fault_mask |= s_metadata.faults;
            disable_replay(out);
        }
        return true;
    }
    if (v1_ok && s_v1.seen_count <= GATE_LEGACY_IDS && s_v1.seen_head < GATE_SEEN_IDS) {
        out->legacy_count = s_v1.seen_count;
        for (uint16_t i = 0; i < out->legacy_count; i++) {
            const uint16_t slot = s_v1.seen_count == GATE_SEEN_IDS ? (s_v1.seen_head + i) % GATE_SEEN_IDS : i;
            memcpy(out->legacy_ids[i], s_v1.seen_ids[slot], GATE_ID_BUF);
        }
        memset(out->seen_ids, 0, sizeof out->seen_ids);
        out->seen_count = out->seen_head = 0;
        if (meta_err != ESP_ERR_NVS_NOT_FOUND || legacy_err != ESP_ERR_NVS_NOT_FOUND) disable_replay(out);
        return true;
    }
    disable_replay(out);
    return true;
}

esp_err_t nv_save_gate(const gate_state_t *st)
{
    if (st->seen_count > GATE_SEEN_IDS || st->seen_head >= GATE_SEEN_IDS || st->legacy_count > GATE_LEGACY_IDS) return ESP_ERR_INVALID_ARG;
    v1_snapshot(st);
    if (!pack_legacy(st)) return ESP_ERR_INVALID_ARG;
    memset(&s_metadata, 0, sizeof s_metadata);
    s_metadata.format = METADATA_FORMAT;
    s_metadata.faults = st->fault_mask; s_metadata.day = st->day;
    memcpy(s_metadata.used_g, st->used_g, sizeof s_metadata.used_g);
    s_metadata.used_total = st->used_total_g;
    s_metadata.mirror_hash = snapshot_hash(s_mirror, sizeof s_mirror);
    s_metadata.legacy_hash = snapshot_hash(&s_legacy, sizeof s_legacy);
    s_metadata.retired_floor = st->retired_expiry_floor;
    s_metadata.active_count = st->seen_count; s_metadata.active_head = st->seen_head;
    s_metadata.legacy_count = st->legacy_count;
    s_metadata.flags = (st->paused ? FLAG_PAUSED : 0) | (st->replay_untrusted ? FLAG_UNTRUSTED : 0);
    for (uint16_t i = 0; i < st->seen_count; i++) pack_expiry(st->seen_expires_at[i], s_metadata.expiries[i]);
    nvs_handle_t h;
    esp_err_t err = nvs_open(NS, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    /* The actual allocator pilot only accepts this order: legacy, METADATA,
     * then mirror. Keys are not an atomic transaction; load verifies all three. */
    err = nvs_set_blob(h, KEY_LEGACY, &s_legacy, sizeof s_legacy);
    if (err == ESP_OK) err = nvs_set_blob(h, KEY_METADATA, &s_metadata, sizeof s_metadata);
    if (err == ESP_OK) err = nvs_set_blob(h, KEY_GATE, s_mirror, sizeof s_mirror);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    return err;
}

static bool load_blob(const char *key, void *buf, size_t size)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NS, NVS_READONLY, &h);
    if (err != ESP_OK) return false;
    size_t actual = size;
    err = nvs_get_blob(h, key, buf, &actual);
    nvs_close(h);
    return err == ESP_OK && actual == size;
}

static esp_err_t save_blob(const char *key, const void *data, size_t size)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NS, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    err = nvs_set_blob(h, key, data, size);
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
