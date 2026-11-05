#include "cue.h"

#include <stdbool.h>
#include <stddef.h>

#include "driver/ledc.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define CUE_GPIO 20 /* buzzer through a transistor, see docs/architecture.md */
#define CUE_MODE LEDC_LOW_SPEED_MODE /* the ESP32-C3 has no high-speed mode */
#define CUE_TIMER LEDC_TIMER_0
#define CUE_CHANNEL LEDC_CHANNEL_0
#define CUE_RES LEDC_TIMER_10_BIT
#define CUE_DUTY_ON (1 << 9) /* 50 % of 10 bit */
#define CUE_DEFAULT_HZ 2000

#define NOTE_MS 150
#define GAP_MS 50

/* The fixed pattern: A5, D6, G6. */
static const uint32_t k_notes_hz[] = {880, 1175, 1568};

static const char *TAG = "cue";
static bool s_ready;

esp_err_t cue_init(void)
{
    const ledc_timer_config_t timer = {
        .speed_mode = CUE_MODE,
        .duty_resolution = CUE_RES,
        .timer_num = CUE_TIMER,
        .freq_hz = CUE_DEFAULT_HZ,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    esp_err_t err = ledc_timer_config(&timer);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "ledc timer: %s", esp_err_to_name(err));
        return err;
    }
    const ledc_channel_config_t ch = {
        .gpio_num = CUE_GPIO,
        .speed_mode = CUE_MODE,
        .channel = CUE_CHANNEL,
        .intr_type = LEDC_INTR_DISABLE,
        .timer_sel = CUE_TIMER,
        .duty = 0, /* silent until cue_play() */
        .hpoint = 0,
    };
    err = ledc_channel_config(&ch);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "ledc channel: %s", esp_err_to_name(err));
        return err;
    }
    s_ready = true;
    return ESP_OK;
}

static void silence(void)
{
    /* idle level 0 so the transistor is off and the buzzer draws nothing */
    ledc_stop(CUE_MODE, CUE_CHANNEL, 0);
}

void cue_play(void)
{
    if (!s_ready) {
        ESP_LOGW(TAG, "cue_play before cue_init");
        return;
    }
    for (size_t i = 0; i < sizeof k_notes_hz / sizeof k_notes_hz[0]; i++) {
        if (ledc_set_freq(CUE_MODE, CUE_TIMER, k_notes_hz[i]) != ESP_OK ||
            ledc_set_duty(CUE_MODE, CUE_CHANNEL, CUE_DUTY_ON) != ESP_OK ||
            ledc_update_duty(CUE_MODE, CUE_CHANNEL) != ESP_OK) {
            ESP_LOGE(TAG, "ledc call failed at note %u", (unsigned)i);
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(NOTE_MS));
        silence();
        vTaskDelay(pdMS_TO_TICKS(GAP_MS));
    }
    silence();
}
