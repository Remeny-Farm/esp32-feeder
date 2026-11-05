/*
 * Treat feeder firmware entry point.
 *
 * Boot order: NVS -> event loop -> gate + schedule restore -> motor, cue ->
 * Wi-Fi -> (IP) SNTP -> MQTT. The feeder task starts right after Wi-Fi so the
 * service button and the queue work before the network is up. Random scatters
 * still wait for a valid clock (SNTP), because quiet hours are unknowable
 * without one.
 *
 * NOT implemented: OTA download and image signature check (only the rollback
 * confirmation is, see feeder.c). Secure Boot and flash encryption are off on
 * development boards (docs/architecture.md).
 */
#include "esp_event.h"
#include "esp_log.h"
#include "fclock.h"
#include "feeder.h"
#include "feeder_mqtt.h"
#include "freertos/FreeRTOS.h"
#include "gate_config.h"
#include "cue.h"
#include "motor.h"
#include "nvs_flash.h"
#include "sdkconfig.h"
#include "wifi_sta.h"

static const char *TAG = "app";

static void init_nvs(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        /* Erases the saved gate state too: replay protection restarts empty. */
        ESP_LOGW(TAG, "NVS needs erase: %s", esp_err_to_name(err));
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);
}

void app_main(void)
{
    init_nvs();
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    const bool config_ok = feeder_config_valid();
    if (!config_ok) {
        ESP_LOGE(TAG, "invalid Feeder config (device id ^[a-z0-9][a-z0-9-]{2,31}$, hopper names): MQTT disabled");
    }

    fclock_init();
    feeder_init(); /* gate + schedule restore, queue */
    motor_init();
    if (cue_init() != ESP_OK) {
        ESP_LOGE(TAG, "cue init failed: no sound cue");
    }

    wifi_sta_start();
    feeder_start();

    /* Wait for the first IP, then start SNTP and MQTT. Both reconnect by
     * themselves afterwards, so this runs once. */
    wifi_sta_wait_ip(portMAX_DELAY);
    fclock_sntp_start();
    if (config_ok) {
        const esp_err_t err = feeder_mqtt_start(CONFIG_FEEDER_DEVICE_ID);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "MQTT start failed: %s", esp_err_to_name(err));
        }
    }
    ESP_LOGI(TAG, "boot done");
}
