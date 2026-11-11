/* Actual ESP-IDF v5.3 NVS Storage/Page/PageManager, RAM flash boundary only.
 * No device, SDK installation, partition resize or always-success allocator.
 */
#include <cstdio>
#include <cstring>
#include <memory>
#include <vector>
#include "nvs_storage.hpp"
extern "C" {
#include "nv_store.h"
}

class RamPartition final : public nvs::Partition {
    std::vector<unsigned char> bytes = std::vector<unsigned char>(4 * 4096, 0xff);
public:
    const char *get_partition_name() override { return "nvs"; }
    uint32_t get_address() override { return 0; }
    uint32_t get_size() override { return static_cast<uint32_t>(bytes.size()); }
    bool get_readonly() override { return false; }
    esp_err_t read_raw(size_t offset, void *dst, size_t size) override {
        if (offset > bytes.size() || size > bytes.size() - offset) return ESP_ERR_INVALID_SIZE;
        std::memcpy(dst, bytes.data() + offset, size); return ESP_OK;
    }
    esp_err_t read(size_t offset, void *dst, size_t size) override { return read_raw(offset, dst, size); }
    esp_err_t write_raw(size_t offset, const void *src, size_t size) override {
        if (offset > bytes.size() || size > bytes.size() - offset) return ESP_ERR_INVALID_SIZE;
        const auto *data = static_cast<const unsigned char *>(src);
        for (size_t i = 0; i < size; i++) {
            if ((bytes[offset + i] & data[i]) != data[i]) return ESP_FAIL; // flash cannot set 0 -> 1
            bytes[offset + i] = data[i];
        }
        return ESP_OK;
    }
    esp_err_t write(size_t offset, const void *src, size_t size) override { return write_raw(offset, src, size); }
    esp_err_t erase_range(size_t offset, size_t size) override {
        if (offset % 4096 || size % 4096 || offset > bytes.size() || size > bytes.size() - offset) return ESP_ERR_INVALID_SIZE;
        std::memset(bytes.data() + offset, 0xff, size); return ESP_OK;
    }
};

static nvs::Storage *storage;
static size_t mirror_size, metadata_size;
extern "C" esp_err_t nvs_open(const char *name, nvs_open_mode_t mode, nvs_handle_t *handle)
{
    uint8_t index;
    const auto err = storage->createOrOpenNamespace(name, mode == NVS_READWRITE, index);
    if (err == ESP_OK) *handle = index;
    return err;
}
extern "C" void nvs_close(nvs_handle_t handle) { (void)handle; }
extern "C" esp_err_t nvs_set_blob(nvs_handle_t handle, const char *key, const void *data, size_t len)
{
    if (std::strcmp(key, "gate") == 0) mirror_size = len;
    if (std::strcmp(key, "gate_v2") == 0) metadata_size = len;
    return storage->writeItem(static_cast<uint8_t>(handle), nvs::ItemType::BLOB, key, data, len);
}
extern "C" esp_err_t nvs_get_blob(nvs_handle_t handle, const char *key, void *data, size_t *len)
{
    size_t actual = 0;
    auto err = storage->getItemDataSize(static_cast<uint8_t>(handle), nvs::ItemType::BLOB, key, actual);
    if (err != ESP_OK) return err;
    if (data == nullptr) { *len = actual; return ESP_OK; }
    if (*len < actual) { *len = actual; return ESP_ERR_NVS_INVALID_LENGTH; }
    *len = actual;
    return storage->readItem(static_cast<uint8_t>(handle), nvs::ItemType::BLOB, key, data, actual);
}
/* v5.3 NVSHandleSimple::commit() returns ESP_OK for a valid handle; allocation
 * and COW happen synchronously in the unmodified Storage::writeItem above. */
extern "C" esp_err_t nvs_commit(nvs_handle_t handle) { (void)handle; return ESP_OK; }
extern "C" bool sched_cfg_valid(const sched_cfg_t *cfg) { return cfg->magic == SCHED_CFG_MAGIC; }

static bool require(esp_err_t error, const char *operation)
{
    if (error == ESP_OK) return true;
    std::printf("FAIL %s: actual NVS error 0x%x; mirror %zu B, extension %zu B\n",
                operation, static_cast<unsigned>(error), mirror_size, metadata_size);
    return false;
}

int main()
{
    static_assert(nvs::Page::ENTRY_COUNT == 126, "pinned NVS page geometry changed");
    static_assert(nvs::Page::ENTRY_SIZE == 32, "pinned NVS entry geometry changed");
    RamPartition flash;
    std::unique_ptr<nvs::Storage> instance(new (std::nothrow) nvs::Storage(&flash));
    storage = instance.get();
    if (!require(storage->init(0, 4), "init four-page partition")) return 1;
    // Deliberate headroom load: 64 scalar Wi-Fi settings, a 256-byte PHY blob,
    // and the actual schedule type. This is not a claim about every SDK config.
    uint8_t wifi, phy;
    if (!require(storage->createOrOpenNamespace("wifi", true, wifi), "wifi namespace")) return 1;
    if (!require(storage->createOrOpenNamespace("phy", true, phy), "phy namespace")) return 1;
    for (uint32_t i = 0; i < 64; i++) {
        char key[16]; std::snprintf(key, sizeof key, "setting%u", static_cast<unsigned>(i));
        if (!require(storage->writeItem(wifi, key, i), "64 scalar SDK settings")) return 1;
    }
    unsigned char calibration[256] = {};
    if (!require(storage->writeItem(phy, nvs::ItemType::BLOB, "cal", calibration, sizeof calibration), "PHY blob")) return 1;
    sched_cfg_t schedule = {};
    schedule.magic = SCHED_CFG_MAGIC; schedule.window_start_min = 420; schedule.window_end_min = 1140;
    if (!require(nv_save_sched(&schedule), "schedule")) return 1;

    gate_state_t state = {};
    state.magic = GATE_STATE_MAGIC; state.version = GATE_STATE_VERSION;
    state.paused = true; state.fault_mask = GATE_FAULT_STALL;
    gate_runtime_limits_clear(&state.runtime);
    state.seen_count = GATE_SEEN_IDS;
#ifdef GATE_LEGACY_IDS
    state.legacy_count = GATE_LEGACY_IDS;
#endif
    for (int i = 0; i < GATE_SEEN_IDS; i++) {
        std::snprintf(state.seen_ids[i], GATE_ID_BUF, "active-%d", i);
#ifdef GATE_LEGACY_IDS
        std::snprintf(state.legacy_ids[i], GATE_ID_BUF, "legacy-%d", i);
#endif
    }
    for (uint32_t update = 1; update <= 512; update++) {
        state.used_g[0] = state.used_total_g = update;
        state.clock_high_water = GATE_MIN_VALID_EPOCH + update;
        state.seen_expires_at = state.clock_high_water + 300;
        if (!require(nv_save_gate(&state), "changed snapshot save")) {
            std::printf("failed at changed save %u\n", static_cast<unsigned>(update)); return 1;
        }
        gate_state_t loaded = {};
        if (!nv_load_gate(&loaded) || loaded.replay_untrusted || loaded.used_g[0] != update ||
            !loaded.paused || loaded.fault_mask != GATE_FAULT_STALL ||
            std::strcmp(loaded.seen_ids[63], "active-63") != 0 ||
            std::strcmp(loaded.legacy_ids[63], "legacy-63") != 0) {
            std::printf("FAIL roundtrip after changed save %u\n", static_cast<unsigned>(update)); return 1;
        }
        if (update % 64 == 0) {
            instance.reset(new (std::nothrow) nvs::Storage(&flash));
            storage = instance.get();
            if (!require(storage->init(0, 4), "reopen allocated pages")) return 1;
        }
    }
    // Verify GC and reopen did not trade other consumers' data for gate space.
    for (uint32_t i = 0; i < 64; i++) {
        char key[16]; std::snprintf(key, sizeof key, "setting%u", static_cast<unsigned>(i));
        uint32_t value = UINT32_MAX;
        if (!require(storage->readItem(wifi, key, value), "read SDK setting") || value != i) return 1;
    }
    unsigned char loaded_calibration[256];
    if (!require(storage->readItem(phy, nvs::ItemType::BLOB, "cal", loaded_calibration, sizeof loaded_calibration), "read PHY blob") ||
        std::memcmp(calibration, loaded_calibration, sizeof calibration) != 0) return 1;
    sched_cfg_t loaded_schedule = {};
    if (!nv_load_sched(&loaded_schedule) || loaded_schedule.window_start_min != 420 || loaded_schedule.window_end_min != 1140) return 1;
    gate_state_t final_state = {};
    if (!nv_load_gate(&final_state) || final_state.replay_untrusted || final_state.used_g[0] != 512) return 1;
    nvs_stats_t stats = {};
    if (!require(storage->fillStats(stats), "allocator stats")) return 1;
    std::printf("PASS actual ESP-IDF v5.3 allocator: 512 changed snapshot pairs + 8 reopen cycles; "
                "4 x 4096 B; mirror %zu B, extension %zu B; used entries %zu, free entries %zu; "
                "64 SDK scalars + 256 B PHY + schedule retained\n",
                mirror_size, metadata_size, stats.used_entries, stats.free_entries);
    return 0;
}
