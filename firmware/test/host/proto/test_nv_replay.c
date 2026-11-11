/* Exercise the real NVS adapter with in-memory boundary stubs, no device. */
#include "mini_test.h"
#include "nv_store.h"
#include "nvs.h"
#include <stdlib.h>

/* Exact deployed v1 layout, retained here as a migration fixture. */
typedef struct {
    uint32_t magic, version;
    int64_t day;
    uint32_t used_g[GATE_MAX_HOPPERS], used_total_g;
    int64_t last_scatter_at, last_cue_at;
    uint32_t fault_mask;
    bool paused;
    gate_runtime_limits_t runtime;
    char seen_ids[GATE_SEEN_IDS][GATE_ID_BUF];
    uint16_t seen_head, seen_count;
} legacy_gate_state_t;
static unsigned char blobs[2][sizeof(gate_state_t)];
static size_t sizes[2];
static int commits, fail_mirror;
static int slot(const char *key) { return strcmp(key, "gate_v2") == 0 ? 1 : 0; }
esp_err_t nvs_open(const char *ns, int mode, nvs_handle_t *h)
{ (void)ns; (void)mode; *h = 1; return ESP_OK; }
void nvs_close(nvs_handle_t h) { (void)h; }
esp_err_t nvs_get_blob(nvs_handle_t h, const char *key, void *dst, size_t *len)
{
    (void)h; int i = slot(key);
    if (!sizes[i]) return ESP_ERR_NVS_NOT_FOUND;
    if (*len < sizes[i]) return ESP_ERR_NVS_INVALID_LENGTH;
    memcpy(dst, blobs[i], sizes[i]); *len = sizes[i]; return ESP_OK;
}
esp_err_t nvs_set_blob(nvs_handle_t h, const char *key, const void *src, size_t len)
{
    (void)h; int i = slot(key);
    if (i == 0 && fail_mirror) return ESP_ERR_NVS_INVALID_LENGTH;
    memcpy(blobs[i], src, len); sizes[i] = len; return ESP_OK;
}
esp_err_t nvs_commit(nvs_handle_t h) { (void)h; commits++; return ESP_OK; }
bool sched_cfg_valid(const sched_cfg_t *s) { (void)s; return true; }

static void test_legacy_load_preserves_state_and_ids(void)
{
    legacy_gate_state_t old;
    memset(&old, 0, sizeof old);
    old.magic = GATE_STATE_MAGIC; old.version = 1; old.day = 3;
    old.used_g[0] = old.used_total_g = 7;
    old.paused = true;
    gate_runtime_limits_clear(&old.runtime); old.runtime.daily_total_g = 9;
    for (int i = 0; i < GATE_SEEN_IDS; i++) snprintf(old.seen_ids[i], GATE_ID_BUF, "legacy-%d", i);
    strcpy(old.seen_ids[0], "legacy-future-resume"); old.seen_count = GATE_SEEN_IDS; old.seen_head = 0;
    memcpy(blobs[0], &old, sizeof old); sizes[0] = sizeof old; sizes[1] = 0;
    gate_state_t loaded;
    CHECK(nv_load_gate(&loaded));
    CHECK_EQ(loaded.version, GATE_STATE_VERSION);
    CHECK(loaded.paused); CHECK_EQ(loaded.used_g[0], 7);
    CHECK_EQ(loaded.used_total_g, 7); CHECK_EQ(loaded.runtime.daily_total_g, 9);
    CHECK_EQ(loaded.day, 3);
#ifdef GATE_LEGACY_IDS
    CHECK_EQ(loaded.seen_count, 0); CHECK_EQ(loaded.legacy_count, GATE_SEEN_IDS);
    CHECK_STR(loaded.legacy_ids[0], "legacy-future-resume");
#else
    CHECK_EQ(loaded.seen_count, GATE_SEEN_IDS);
    CHECK_STR(loaded.seen_ids[0], "legacy-future-resume");
#endif
#if GATE_STATE_VERSION >= 2
    CHECK_EQ(sizeof old, GATE_STATE_V1_SIZE);

#endif
    gate_limits_t limits = {.hopper_count = 1, .max_dose_g = {10},
                            .daily_budget_g = {50}, .daily_total_g = 100};
    gate_t g; CHECK(gate_init(&g, &limits)); CHECK(gate_restore(&g, &loaded));
    gate_time_t t = {.now = GATE_MIN_VALID_EPOCH + 10000, .clock_valid = true,
                     .local_minute = 720, .local_day = 3};
    gate_cmd_meta_t m = {.id = "legacy-future-resume", .issued_at = t.now, .expires_at = t.now + 300};
    CHECK_EQ(gate_check_command(&g, &t, &m), GATE_DUPLICATE);
    t.now += 100000;
#if GATE_STATE_VERSION >= 2
    t.monotonic_s = 100000;
#endif
    m.issued_at = t.now; m.expires_at = t.now + 300;
    CHECK_EQ(gate_check_command(&g, &t, &m), GATE_DUPLICATE);
    CHECK(gate_paused(&g));
    m.id = "new-explicit-resume";
    CHECK_EQ(gate_check_command(&g, &t, &m), GATE_OK);
    gate_set_paused(&g, false);
    CHECK(!gate_paused(&g));
    /* A normal v2 save/reload retains quarantine and a working active cache. */
    CHECK_EQ(nv_save_gate(gate_state(&g)), ESP_OK);
    CHECK(nv_load_gate(&loaded));
    gate_t again; CHECK(gate_init(&again, &limits)); CHECK(gate_restore(&again, &loaded));
    m.id = "legacy-future-resume";
    CHECK_EQ(gate_check_command(&again, &t, &m), GATE_DUPLICATE);
    m.id = "new-after-reboot";
    CHECK_EQ(gate_check_command(&again, &t, &m), GATE_OK);
}

static void test_v2_roundtrip_retains_expiry_and_wall_floor(void)
{
#if GATE_STATE_VERSION >= 2
    gate_state_t state, loaded;
    memset(&state, 0, sizeof state);
    state.magic = GATE_STATE_MAGIC; state.version = GATE_STATE_VERSION;
    state.paused = true; state.seen_expires_at = GATE_MIN_VALID_EPOCH + 1300;
    state.clock_high_water = GATE_MIN_VALID_EPOCH + 1000;
    gate_runtime_limits_clear(&state.runtime);
    CHECK_EQ(nv_save_gate(&state), ESP_OK);
#ifdef GATE_LEGACY_IDS
    CHECK_EQ(sizes[0], sizeof(legacy_gate_state_t));
    CHECK(sizes[1] < sizeof state); /* extension metadata omits the active-cache prefix */
#else
    CHECK_EQ(sizes[0], sizeof(legacy_gate_state_t));
#endif
    CHECK(nv_load_gate(&loaded));
    CHECK(memcmp(&state, &loaded, sizeof state) == 0);
#endif
}

static void test_partial_save_disables_admission_and_preserves_budgets(void)
{
#ifdef GATE_LEGACY_IDS
    gate_state_t state, loaded;
    memset(&state, 0, sizeof state);
    state.magic = GATE_STATE_MAGIC; state.version = GATE_STATE_VERSION;
    state.paused = true; state.fault_mask = GATE_FAULT_STALL;
    state.used_g[0] = state.used_total_g = 7;
    gate_runtime_limits_clear(&state.runtime);
    CHECK_EQ(nv_save_gate(&state), ESP_OK);
    state.paused = false; state.used_g[0] = state.used_total_g = 9;
    fail_mirror = 1;
    CHECK(nv_save_gate(&state) != ESP_OK);
    fail_mirror = 0;
    CHECK(nv_load_gate(&loaded)); CHECK(loaded.replay_untrusted);
    CHECK(loaded.paused); CHECK_EQ(loaded.used_g[0], 9);
    CHECK(loaded.fault_mask & GATE_FAULT_STALL);
    gate_limits_t limits = {.hopper_count = 1, .max_dose_g = {10},
                            .daily_budget_g = {50}, .daily_total_g = 100};
    gate_t g; CHECK(gate_init(&g, &limits)); CHECK(gate_restore(&g, &loaded));
    gate_fault_clear_all(&g); /* existing service action cannot clear integrity failure */
    gate_time_t t = {.now = GATE_MIN_VALID_EPOCH + 100, .clock_valid = true, .local_minute = 720};
    gate_cmd_meta_t m = {.id = "resume", .issued_at = t.now, .expires_at = t.now + 300};
    CHECK_EQ(gate_check_command(&g, &t, &m), GATE_FAULT);
    /* Also handle a newer v1 snapshot written after rolling back. */
    legacy_gate_state_t newer;
    memcpy(&newer, blobs[0], sizeof newer);
    newer.used_g[0] = newer.used_total_g = 12;
    newer.fault_mask |= GATE_FAULT_LID_OPEN;
    memcpy(blobs[0], &newer, sizeof newer);
    CHECK(nv_load_gate(&loaded));
    CHECK(loaded.replay_untrusted && loaded.paused);
    CHECK_EQ(loaded.used_g[0], 12);
    CHECK_EQ(loaded.used_total_g, 12);
    CHECK(loaded.fault_mask & GATE_FAULT_LID_OPEN);
#endif
}

static void test_export_rollback_snapshot(void)
{
    gate_state_t state;
    memset(&state, 0, sizeof state);
    state.magic = GATE_STATE_MAGIC; state.version = GATE_STATE_VERSION;
    state.paused = true; state.fault_mask = GATE_FAULT_STALL;
    state.used_g[0] = state.used_total_g = 7;
    strcpy(state.seen_ids[0], "rollback-id"); state.seen_count = state.seen_head = 1;
    gate_runtime_limits_clear(&state.runtime);
    CHECK_EQ(nv_save_gate(&state), ESP_OK);
    const char *path = getenv("FEEDER_TEST_ROLLBACK_BLOB");
    if (path != NULL) {
        FILE *f = fopen(path, "wb"); CHECK(f != NULL);
        if (f != NULL) { CHECK_EQ(fwrite(blobs[0], 1, sizes[0], f), sizes[0]); fclose(f); }
    }
}

static void test_full_length_printable_legacy_ids_roundtrip(void)
{
#ifdef GATE_LEGACY_IDS
    gate_state_t state, loaded;
    memset(&state, 0, sizeof state);
    state.magic = GATE_STATE_MAGIC; state.version = GATE_STATE_VERSION;
    gate_runtime_limits_clear(&state.runtime);
    state.legacy_count = GATE_LEGACY_IDS;
    for (uint16_t i = 0; i < GATE_LEGACY_IDS; i++) {
        for (size_t j = 0; j < GATE_ID_MAX_LEN; j++) {
            state.legacy_ids[i][j] = (char)(0x21 + (i * 36 + j) % 94);
        }
        state.legacy_ids[i][0] = (char)(0x21 + i); /* unique ids, full printable range */
        state.legacy_ids[i][GATE_ID_MAX_LEN] = '\0';
    }
    strcpy(state.legacy_ids[0], "!"); /* shortest id and maximum-length ids */
    CHECK_EQ(nv_save_gate(&state), ESP_OK);
    CHECK(nv_load_gate(&loaded));
    CHECK(!loaded.replay_untrusted);
    for (uint16_t i = 0; i < GATE_LEGACY_IDS; i++) CHECK_STR(state.legacy_ids[i], loaded.legacy_ids[i]);
#endif
}

int main(void)
{
    RUN_TEST(test_legacy_load_preserves_state_and_ids);
    RUN_TEST(test_v2_roundtrip_retains_expiry_and_wall_floor);
    RUN_TEST(test_partial_save_disables_admission_and_preserves_budgets);
    RUN_TEST(test_full_length_printable_legacy_ids_roundtrip);
    RUN_TEST(test_export_rollback_snapshot);
    return mt_summary();
}
