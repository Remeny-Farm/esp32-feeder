/* Compiled against the ORIGINAL v1 headers + nv_store.c, reads v2's mirror. */
#include "mini_test.h"
#include "nv_store.h"
#include "nvs.h"
#include <stdlib.h>
static unsigned char blob[16384];
static size_t blob_size;
esp_err_t nvs_open(const char *ns, int mode, nvs_handle_t *h)
{ (void)ns; (void)mode; *h = 1; return ESP_OK; }
void nvs_close(nvs_handle_t h) { (void)h; }
esp_err_t nvs_get_blob(nvs_handle_t h, const char *key, void *dst, size_t *len)
{
    (void)h;
    if (strcmp(key, "gate") != 0) return ESP_ERR_NVS_NOT_FOUND;
    if (*len < blob_size) return ESP_ERR_NVS_INVALID_LENGTH;
    memcpy(dst, blob, blob_size); *len = blob_size; return ESP_OK;
}
esp_err_t nvs_set_blob(nvs_handle_t h, const char *key, const void *src, size_t len)
{ (void)h; (void)key; (void)src; (void)len; return ESP_OK; }
esp_err_t nvs_commit(nvs_handle_t h) { (void)h; return ESP_OK; }
bool sched_cfg_valid(const sched_cfg_t *s) { (void)s; return true; }
int main(int argc, char **argv)
{
    if (argc != 2) return 2;
    FILE *f = fopen(argv[1], "rb"); if (f == NULL) return 2;
    blob_size = fread(blob, 1, sizeof blob, f); fclose(f);
    gate_state_t loaded;
    memset(&loaded, 0, sizeof loaded);
    CHECK_EQ(GATE_STATE_VERSION, 1);
    CHECK(nv_load_gate(&loaded));
    CHECK_EQ(loaded.version, 1); CHECK(loaded.paused);
    CHECK_EQ(loaded.used_g[0], 7); CHECK_EQ(loaded.used_total_g, 7);
    CHECK(loaded.fault_mask & GATE_FAULT_STALL);
    CHECK_EQ(loaded.seen_count, 1); CHECK_STR(loaded.seen_ids[0], "rollback-id");
    gate_limits_t limits = {.hopper_count = 1, .max_dose_g = {10},
                            .daily_budget_g = {50}, .daily_total_g = 100};
    gate_t g; CHECK(gate_init(&g, &limits)); CHECK(gate_restore(&g, &loaded));
    CHECK(gate_paused(&g)); CHECK(gate_state(&g)->fault_mask & GATE_FAULT_STALL);
    return mt_summary();
}
