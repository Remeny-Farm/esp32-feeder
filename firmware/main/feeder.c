#include "feeder.h"

#include <inttypes.h>
#include <stdlib.h>
#include <string.h>

#include "cue.h"
#include "driver/gpio.h"
#include "esp_app_desc.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_random.h"
#include "fclock.h"
#include "feeder_mqtt.h"
#include "feeder_util.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "gate_config.h"
#include "motor.h"
#include "nv_store.h"
#include "proto.h"
#include "rand_sched.h"
#include "safety_gate.h"
#include "sdkconfig.h"

#define FEEDER_TASK_STACK 6144
#define FEEDER_TASK_PRIO 5
#define LOOP_MS 100 /* queue wait; also the service button poll period */
#define SERVICE_BTN_GPIO GPIO_NUM_9 /* on-board BOOT button, active low */
#define SERVICE_BTN_HOLD_MS 3000

static const char *TAG = "feeder";

static gate_t s_gate;               /* owned by the feeder task after feeder_start() */
static QueueHandle_t s_queue;
static feeder_msg_t s_msg;          /* receive buffer: static, not on the task stack */
static bool s_save_pending;         /* the last NVS save of the gate failed: retry */
static bool s_ota_checked;
static proto_state_t s_last_state;  /* last state successfully queued */
static bool s_have_last_state;

/* ------------------------------------------------------------ helpers */

/* Returns false when a save was needed and failed (it is retried on the next call). */
static bool persist_gate(void)
{
    if (gate_take_dirty(&s_gate) || s_save_pending) {
        const esp_err_t err = nv_save_gate(gate_state(&s_gate));
        s_save_pending = (err != ESP_OK);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "gate save failed: %s (retry on next change)", esp_err_to_name(err));
            return false;
        }
    }
    return true;
}

/* `at` buffer helper: returns buf when the clock is valid, else NULL (omit `at`). */
static const char *now_str(char *buf, size_t len)
{
    return fclock_now_rfc3339(buf, len) ? buf : NULL;
}

static bool send_json(feeder_topic_t topic, char *json)
{
    if (json == NULL) {
        ESP_LOGE(TAG, "out of memory building a message");
        return false;
    }
    const bool queued = feeder_mqtt_publish(topic, json); /* failure is logged inside */
    proto_free(json);
    return queued;
}

static void send_ack(const char *id, gate_result_t r)
{
    char at[RFC3339_UTC_BUF];
    send_json(FEEDER_TOPIC_ACK,
              proto_build_ack(id, r == GATE_OK, r == GATE_OK ? NULL : gate_reason_str(r), now_str(at, sizeof at)));
}

static void send_cue_event(gate_source_t src, const char *cmd_id)
{
    char at[RFC3339_UTC_BUF];
    send_json(FEEDER_TOPIC_EVENT, proto_build_event_cue(src, cmd_id, now_str(at, sizeof at)));
}

static void send_fault_event(gate_source_t src, const char *cmd_id, proto_fault_t code, int hopper)
{
    char at[RFC3339_UTC_BUF];
    send_json(FEEDER_TOPIC_EVENT, proto_build_event_fault(src, cmd_id, code, hopper, now_str(at, sizeof at)));
}

/* ------------------------------------------------------------ state */

static void build_state(proto_state_t *v, const gate_time_t *tm)
{
    memset(v, 0, sizeof *v);
    v->paused = gate_paused(&s_gate);
    v->clock_valid = tm->clock_valid;
    v->fw_version = esp_app_get_description()->version;
    v->hopper_count = feeder_hopper_count();
    for (uint8_t i = 0; i < v->hopper_count; i++) {
        v->level[i] = gate_level(&s_gate, i);
        /* Rolls the day; the gate itself ignores an invalid clock. */
        v->used_g[i] = gate_used_today(&s_gate, tm, i);
    }
}

static bool state_differs(const proto_state_t *a, const proto_state_t *b)
{
    if (a->paused != b->paused || a->clock_valid != b->clock_valid) {
        return true;
    }
    for (uint8_t i = 0; i < a->hopper_count; i++) {
        if (a->level[i] != b->level[i] || a->used_g[i] != b->used_g[i]) {
            return true;
        }
    }
    return false;
}

/* Publish the retained state when mode, levels, daily usage or clock_valid
 * changed, or always when `force` (on MQTT connect). */
static void publish_state(bool force)
{
    gate_time_t tm;
    proto_state_t v;
    fclock_get(&tm);
    build_state(&v, &tm);
    if (!force && s_have_last_state && !state_differs(&v, &s_last_state)) {
        return;
    }
    s_have_last_state = false; /* a failed forced reconnect publish must retry too */
    char at[RFC3339_UTC_BUF];
    if (send_json(FEEDER_TOPIC_STATE, proto_build_state(&v, now_str(at, sizeof at)))) {
        s_last_state = v;
        s_have_last_state = true;
    }
}

/* ------------------------------------------------------------ OTA rollback */

static void mark_app_valid_once(void)
{
    if (s_ota_checked) {
        return;
    }
    s_ota_checked = true;
    /* NOT implemented: OTA download and image signature check. This only
     * confirms a freshly flashed image after MQTT connected and the task runs. */
    const esp_partition_t *running = esp_ota_get_running_partition();
    esp_ota_img_states_t state;
    if (running != NULL && esp_ota_get_state_partition(running, &state) == ESP_OK &&
        state == ESP_OTA_IMG_PENDING_VERIFY) {
        const esp_err_t err = esp_ota_mark_app_valid_cancel_rollback();
        ESP_LOGI(TAG, "new image confirmed: %s", esp_err_to_name(err));
    }
}

/* ------------------------------------------------------------ motor sequence */

static void handle_motor_fault(gate_source_t src, const char *cmd_id, uint8_t hopper, motor_result_t mr)
{
    switch (mr) {
    case MOTOR_FAULT_STALL:
        gate_fault_raise(&s_gate, GATE_FAULT_STALL);
        send_fault_event(src, cmd_id, PROTO_FAULT_STALL, hopper);
        break;
    case MOTOR_FAULT_HOPPER_EMPTY:
        /* A level, not a latched fault: the gate rejects this hopper from now on. */
        gate_set_level(&s_gate, hopper, GATE_LEVEL_EMPTY);
        send_fault_event(src, cmd_id, PROTO_FAULT_HOPPER_EMPTY, hopper);
        break;
    case MOTOR_FAULT_CONTROLLER:
    case MOTOR_OK:
    default:
        gate_fault_raise(&s_gate, GATE_FAULT_MOTOR_CONTROLLER);
        send_fault_event(src, cmd_id, PROTO_FAULT_MOTOR_CONTROLLER, -1);
        break;
    }
}

/* reserve+save -> cue -> delay -> doses -> spin -> settle -> report. The gate has admitted it.
 * Fail closed: the requested grams and last_scatter_at are reserved and saved to NVS BEFORE any
 * motor or buzzer runs, so a reset while dispensing still counts them. */
static void run_sequence(gate_source_t src, const char *cmd_id, const gate_dose_t *doses, size_t n,
                         bool with_cue)
{
    uint32_t actual[GATE_MAX_HOPPERS] = {0};
    uint32_t total = 0;
    if (n > GATE_MAX_HOPPERS) {
        n = GATE_MAX_HOPPERS; /* cannot happen after the gate admitted the list */
    }
    bool faulted = false;

    gate_time_t tm;
    fclock_get(&tm);
    if (!gate_reserve_scatter(&s_gate, &tm, doses, n)) {
        ESP_LOGE(TAG, "scatter reserve failed: not dispensing");
        return;
    }
    if (!persist_gate()) {
        /* The reservation is not durable: a reset now would lose it. Do not dispense.
         * Give the grams back; min_interval stays (last_scatter_at is kept). */
        ESP_LOGE(TAG, "scatter reservation not saved: not dispensing");
        gate_settle_scatter(&s_gate, &tm, actual); /* actual is all 0 here */
        return;
    }

    if (with_cue) {
        cue_play();
        send_cue_event(src, cmd_id);
    }
    vTaskDelay(pdMS_TO_TICKS(CONFIG_FEEDER_CUE_DELAY_MS));

    for (size_t i = 0; i < n; i++) {
        const uint32_t want = (uint32_t)doses[i].grams; /* gate checked 1..max dose */
        uint32_t got = 0;
        const motor_result_t mr = motor_dose(doses[i].hopper, want, &got);
        if (got > want) {
            got = want;
        }
        actual[doses[i].hopper] = got;
        total += got;
        if (mr != MOTOR_OK) {
            motor_stop_all();
            handle_motor_fault(src, cmd_id, doses[i].hopper, mr);
            faulted = true;
            break; /* later doses stay 0 */
        }
    }
    if (!faulted || total > 0) {
        motor_spin(CONFIG_FEEDER_SPIN_MS); /* also clears the chute after a partial dose */
    }
    motor_stop_all();

    fclock_get(&tm);
    gate_settle_scatter(&s_gate, &tm, actual); /* lowers the reservation to the actual grams (0 allowed) */

    uint32_t reported[GATE_MAX_HOPPERS];
    for (size_t i = 0; i < n; i++) {
        reported[i] = actual[doses[i].hopper];
    }
    char at[RFC3339_UTC_BUF];
    send_json(FEEDER_TOPIC_EVENT,
              proto_build_event_scatter(src, cmd_id, doses, reported, n, now_str(at, sizeof at)));
    persist_gate();
    publish_state(false);
}

/* ------------------------------------------------------------ random scatter */

static void handle_random_scatter(void)
{
    gate_time_t tm;
    fclock_get(&tm);

    /* One hopper, chosen at random among those not known to be empty. */
    uint8_t candidates[GATE_MAX_HOPPERS];
    uint32_t n = 0;
    for (uint8_t i = 0; i < feeder_hopper_count(); i++) {
        if (gate_level(&s_gate, i) != GATE_LEVEL_EMPTY) {
            candidates[n++] = i;
        }
    }
    if (n == 0) {
        ESP_LOGW(TAG, "random scatter skipped: all hoppers empty");
        return;
    }
    const uint8_t hopper = candidates[esp_random() % n];
    const gate_dose_t dose = {.hopper = hopper, .grams = feeder_random_dose_g(hopper)};

    const gate_result_t r = gate_check_scatter(&s_gate, &tm, GATE_SRC_RANDOM, NULL, &dose, 1);
    persist_gate();
    if (r != GATE_OK) {
        /* No ack for a random scatter and no retry: the next planned slot tries again. */
        ESP_LOGI(TAG, "random scatter rejected: %s", gate_reason_str(r));
        return;
    }
    run_sequence(GATE_SRC_RANDOM, NULL, &dose, 1, true); /* random scatters always cue */
}

/* ------------------------------------------------------------ remote commands */

static void handle_cmd(const char *buf, size_t len)
{
    proto_cmd_t cmd;
    const proto_parse_t pr = proto_parse_cmd(buf, len, &cmd);
    if (pr == PROTO_PARSE_DROP) {
        ESP_LOGW(TAG, "cmd dropped: no usable id or not JSON (%u bytes)", (unsigned)len);
        return;
    }
    gate_time_t tm;
    fclock_get(&tm);
    const gate_cmd_meta_t meta = {.id = cmd.id, .issued_at = cmd.issued_at, .expires_at = cmd.expires_at};
    if (pr == PROTO_PARSE_INVALID) {
        /* A schema-invalid payload can never become eligible. Consume its id
         * at receive time without trusting its expiry to pin the whole cache. */
        const gate_cmd_meta_t invalid = {.id = cmd.id, .issued_at = tm.now, .expires_at = tm.now};
        gate_check_command(&s_gate, &tm, &invalid);
        send_ack(cmd.id, GATE_INVALID);
        return;
    }

    gate_result_t r;

    switch (cmd.type) {
    case CMD_SCATTER:
        r = gate_check_scatter(&s_gate, &tm, GATE_SRC_REMOTE, &meta, cmd.doses, cmd.n_doses);
        send_ack(cmd.id, r);
        persist_gate();
        if (r == GATE_OK) {
            run_sequence(GATE_SRC_REMOTE, cmd.id, cmd.doses, cmd.n_doses, cmd.cue);
        }
        break;
    case CMD_CUE:
        r = gate_check_cue(&s_gate, &tm, GATE_SRC_REMOTE, &meta);
        send_ack(cmd.id, r);
        persist_gate();
        if (r == GATE_OK) {
            cue_play();
            fclock_get(&tm);
            gate_commit_cue(&s_gate, &tm);
            send_cue_event(GATE_SRC_REMOTE, cmd.id);
        }
        break;
    case CMD_PAUSE:
    case CMD_RESUME:
        r = gate_check_command(&s_gate, &tm, &meta);
        if (r == GATE_OK) {
            gate_set_paused(&s_gate, cmd.type == CMD_PAUSE);
        }
        send_ack(cmd.id, r);
        break;
    case CMD_SET_SCHEDULE:
        r = gate_check_command(&s_gate, &tm, &meta);
        if (r == GATE_OK) {
            int clamped = 0;
            r = gate_set_runtime_limits(&s_gate, &cmd.limits, &clamped); /* GATE_INVALID: nothing changed */
            if (r == GATE_OK) {
                sched_cfg_t sc;
                memset(&sc, 0, sizeof sc);
                sc.magic = SCHED_CFG_MAGIC;
                sc.window_start_min = cmd.window_start_min;
                sc.window_end_min = cmd.window_end_min;
                sc.scatters_per_day = cmd.scatters_per_day;
                rand_sched_set_cfg(&sc); /* re-plans today */
                const esp_err_t err = nv_save_sched(&sc);
                if (err != ESP_OK) {
                    ESP_LOGE(TAG, "schedule save failed: %s", esp_err_to_name(err));
                }
                ESP_LOGI(TAG, "schedule set: %u scatters, %d limit(s) clamped", (unsigned)sc.scatters_per_day,
                         clamped);
            }
        }
        send_ack(cmd.id, r);
        break;
    default:
        send_ack(cmd.id, GATE_INVALID);
        break;
    }
}

/* ------------------------------------------------------------ faults and inputs */

static void on_lid(bool open)
{
    /* Hook for a future lid switch (no hardware yet): raise while open, clear on close. */
    if (open) {
        gate_fault_raise(&s_gate, GATE_FAULT_LID_OPEN);
        send_fault_event(GATE_SRC_RANDOM, NULL, PROTO_FAULT_LID_OPEN, -1);
    } else {
        gate_fault_clear(&s_gate, GATE_FAULT_LID_OPEN);
    }
}

static void button_init(void)
{
    const gpio_config_t io = {
        .pin_bit_mask = 1ULL << SERVICE_BTN_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    if (gpio_config(&io) != ESP_OK) {
        ESP_LOGE(TAG, "service button gpio_config failed: faults cannot be cleared locally");
    }
}

/* Held 3 s: clear every latched fault (service action). Once per press. */
static void button_poll(void)
{
    static bool pressed, fired;
    static TickType_t since;
    if (gpio_get_level(SERVICE_BTN_GPIO) != 0) {
        pressed = false;
        fired = false;
        return;
    }
    const TickType_t now = xTaskGetTickCount();
    if (!pressed) {
        pressed = true;
        since = now;
    } else if (!fired && (now - since) >= pdMS_TO_TICKS(SERVICE_BTN_HOLD_MS)) {
        fired = true;
        gate_fault_clear_all(&s_gate);
        ESP_LOGW(TAG, "service button: all faults cleared");
    }
}

/* ------------------------------------------------------------ task */

static void dispatch(const feeder_msg_t *m)
{
    switch (m->kind) {
    case FEEDER_MSG_CMD:
        handle_cmd(m->payload, m->len);
        break;
    case FEEDER_MSG_CONNECTED:
        mark_app_valid_once();
        publish_state(true);
        break;
    case FEEDER_MSG_TICK: {
        gate_time_t tm;
        fclock_get(&tm);
        if (rand_sched_poll(&s_gate, &tm)) {
            handle_random_scatter();
        }
        break;
    }
    case FEEDER_MSG_LID:
        on_lid(m->payload[0] != 0);
        break;
    default:
        break;
    }
}

static void feeder_task(void *arg)
{
    (void)arg;
    button_init();
    for (;;) {
        if (xQueueReceive(s_queue, &s_msg, pdMS_TO_TICKS(LOOP_MS)) == pdTRUE) {
            dispatch(&s_msg);
        }
        button_poll();
        persist_gate();      /* after every handled message and every commit */
        publish_state(false); /* only publishes when something changed */
    }
}

/* ------------------------------------------------------------ public API */

void feeder_init(void)
{
    s_queue = xQueueCreate(FEEDER_QUEUE_LEN, sizeof(feeder_msg_t));
    if (s_queue == NULL) {
        ESP_LOGE(TAG, "queue alloc failed");
        abort();
    }

    gate_limits_t limits;
    gate_config_limits(&limits);
    if (!gate_init(&s_gate, &limits)) {
        /* Inconsistent compiled-in limits: the gate rejects everything (fault). */
        ESP_LOGE(TAG, "gate_init failed: limits inconsistent, all scatters will be rejected");
    } else {
        static gate_state_t saved; /* full persisted snapshot: static, not on the stack */
        if (nv_load_gate(&saved)) {
            if (gate_restore(&s_gate, &saved)) {
                ESP_LOGI(TAG, "gate state restored");
            } else {
                ESP_LOGE(TAG, "saved gate state failed validation: admission disabled");
                gate_set_paused(&s_gate, true);
                gate_fault_raise(&s_gate, GATE_FAULT_WATCHDOG);
                s_gate.ready = false; /* never replace invalid persisted safety state with a running gate */
            }
        } else {
            ESP_LOGI(TAG, "no saved gate state: starting empty");
        }
    }

    sched_cfg_t sc;
    if (!nv_load_sched(&sc)) {
        sched_cfg_default(&sc);
        if (!sched_cfg_valid(&sc)) {
            ESP_LOGE(TAG, "default schedule window invalid (Kconfig): no random scatters");
            sc.scatters_per_day = 0;
            sc.window_start_min = 0;
            sc.window_end_min = 1;
        }
    }
    rand_sched_init(&sc);
}

void feeder_start(void)
{
    if (xTaskCreate(feeder_task, "feeder", FEEDER_TASK_STACK, NULL, FEEDER_TASK_PRIO, NULL) != pdPASS) {
        ESP_LOGE(TAG, "task create failed");
        abort();
    }
    rand_sched_start_ticker();
}

bool feeder_post(const feeder_msg_t *msg)
{
    return s_queue != NULL && xQueueSend(s_queue, msg, 0) == pdTRUE;
}

void feeder_lid_changed(bool open)
{
    static feeder_msg_t m; /* call from one task only */
    m.kind = FEEDER_MSG_LID;
    m.payload[0] = open ? 1 : 0;
    m.len = 1;
    feeder_post(&m);
}
