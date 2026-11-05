/*
 * STUB motor backend. It moves nothing: it logs, waits a short time to mimic
 * the motion, and reports the requested grams as dispensed. It never reports a
 * fault. The real backend (Einsy RAMBo over UART) replaces this file later.
 */
#include "motor.h"

#include <inttypes.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "motor(stub)";

void motor_init(void)
{
    ESP_LOGW(TAG, "stub backend: no motor will move");
}

motor_result_t motor_dose(uint8_t hopper, uint32_t grams, uint32_t *dispensed_g)
{
    ESP_LOGI(TAG, "dose hopper=%u grams=%" PRIu32, (unsigned)hopper, grams);
    vTaskDelay(pdMS_TO_TICKS(200));
    if (dispensed_g != NULL) {
        *dispensed_g = grams;
    }
    return MOTOR_OK;
}

void motor_spin(uint32_t ms)
{
    ESP_LOGI(TAG, "spin %" PRIu32 " ms", ms);
    vTaskDelay(pdMS_TO_TICKS(ms));
}

void motor_stop_all(void)
{
    ESP_LOGI(TAG, "stop all");
}
