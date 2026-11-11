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
static unsigned char blob[sizeof(gate_state_t)];
static size_t blob_size;
static int commits;
esp_err_t nvs_open(const char *ns, int mode, nvs_handle_t *h)
{ (void)ns; (void)mode; *h = 1; return ESP_OK; }
void nvs_close(nvs_handle_t h) { (void)h; }
esp_err_t nvs_get_blob(nvs_handle_t h, const char *key, void *dst, size_t *len)
{
    (void)h; (void)key;
    if (*len < blob_size) return ESP_ERR_NVS_INVALID_LENGTH;
    memcpy(dst, blob, blob_size); *len = blob_size; return ESP_OK;
}
esp_err_t nvs_set_blob(nvs_handle_t h, const char *key, const void *src, size_t len)
{ (void)h; (void)key; memcpy(blob, src, len); blob_size = len; return ESP_OK; }
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
    strcpy(old.seen_ids[0], "legacy-future-resume"); old.seen_count = old.seen_head = 1;
    memcpy(blob, &old, sizeof old); blob_size = sizeof old;
    gate_state_t loaded;
    CHECK(nv_load_gate(&loaded));
    CHECK_EQ(loaded.version, GATE_STATE_VERSION);
    CHECK(loaded.paused); CHECK_EQ(loaded.used_g[0], 7);
    CHECK_EQ(loaded.used_total_g, 7); CHECK_EQ(loaded.runtime.daily_total_g, 9);
    CHECK_EQ(loaded.day, 3); CHECK_EQ(loaded.seen_count, 1);
    CHECK_STR(loaded.seen_ids[0], "legacy-future-resume");
#if GATE_STATE_VERSION >= 2
    CHECK_EQ(sizeof old, GATE_STATE_V1_SIZE);
    CHECK_EQ(loaded.seen_expires_at, INT64_MAX); /* expiry cannot be inferred */
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
    CHECK_EQ(commits, 1); CHECK_EQ(blob_size, sizeof state);
    CHECK(nv_load_gate(&loaded));
    CHECK(memcmp(&state, &loaded, sizeof state) == 0);
#endif
}

int main(void)
{
    RUN_TEST(test_legacy_load_preserves_state_and_ids);
    RUN_TEST(test_v2_roundtrip_retains_expiry_and_wall_floor);
    return mt_summary();
}
