#include "proto.h"

#include <string.h>

#include "cJSON.h"
#include "feeder.h"
#include "feeder_util.h"
#include "gate_config.h"

#define JSON_MAX_DEPTH 8
#define MAX_GRAMS 1000000 /* sanity bound for any gram value in a command */
#define MAX_SECONDS 1000000

/* ------------------------------------------------------------ parsing */

/* cJSON recurses per nesting level. Reject deep input before parsing so a
 * short payload cannot overflow the feeder task stack. */
static bool depth_ok(const char *s, size_t len)
{
    int depth = 0;
    bool in_str = false, esc = false;
    for (size_t i = 0; i < len; i++) {
        const char c = s[i];
        if (in_str) {
            if (esc) {
                esc = false;
            } else if (c == '\\') {
                esc = true;
            } else if (c == '"') {
                in_str = false;
            }
        } else if (c == '"') {
            in_str = true;
        } else if (c == '{' || c == '[') {
            if (++depth > JSON_MAX_DEPTH) {
                return false;
            }
        } else if ((c == '}' || c == ']') && depth > 0) {
            depth--;
        }
    }
    return true;
}

/* A JSON number that is a whole number in [lo, hi]. NaN fails every compare. */
static bool json_int(const cJSON *item, int64_t lo, int64_t hi, int64_t *out)
{
    if (!cJSON_IsNumber(item)) {
        return false;
    }
    const double d = item->valuedouble;
    if (!(d >= (double)lo && d <= (double)hi)) {
        return false;
    }
    const int64_t v = (int64_t)d; /* safe: hi is far below INT64_MAX */
    if ((double)v != d) {
        return false; /* fractional */
    }
    *out = v;
    return true;
}

static const char *json_str(const cJSON *item)
{
    return (cJSON_IsString(item) && item->valuestring != NULL) ? item->valuestring : NULL;
}

static bool valid_id(const char *s)
{
    if (s == NULL) {
        return false;
    }
    const size_t n = strlen(s);
    if (n < 1 || n > GATE_ID_MAX_LEN) {
        return false;
    }
    for (size_t i = 0; i < n; i++) {
        const char c = s[i];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '-' ||
              c == '_')) {
            return false;
        }
    }
    return true;
}

static bool parse_time(const cJSON *obj, const char *key, int64_t *out)
{
    const char *s = json_str(cJSON_GetObjectItemCaseSensitive(obj, key));
    return s != NULL && rfc3339_parse(s, out);
}

static bool parse_doses(const cJSON *arr, proto_cmd_t *out)
{
    if (!cJSON_IsArray(arr)) {
        return false;
    }
    const int n = cJSON_GetArraySize(arr);
    if (n < 1 || n > (int)feeder_hopper_count()) {
        return false;
    }
    size_t i = 0;
    const cJSON *it;
    cJSON_ArrayForEach(it, arr)
    {
        if (!cJSON_IsObject(it) || i >= GATE_MAX_HOPPERS) {
            return false;
        }
        const char *name = json_str(cJSON_GetObjectItemCaseSensitive(it, "hopper"));
        const int idx = feeder_hopper_index(name); /* NULL name -> -1 */
        int64_t grams;
        if (idx < 0 || !json_int(cJSON_GetObjectItemCaseSensitive(it, "grams"), 1, MAX_GRAMS, &grams)) {
            return false;
        }
        out->doses[i].hopper = (uint8_t)idx;
        out->doses[i].grams = grams;
        i++;
    }
    out->n_doses = i; /* uniqueness is the gate's check */
    return i > 0;
}

/* {"grain": 5, ...}: unknown hopper names are invalid. */
static bool parse_hopper_map(const cJSON *obj, uint32_t dst[GATE_MAX_HOPPERS])
{
    if (!cJSON_IsObject(obj)) {
        return false;
    }
    const cJSON *it;
    cJSON_ArrayForEach(it, obj)
    {
        const int idx = feeder_hopper_index(it->string);
        int64_t v;
        if (idx < 0 || !json_int(it, 0, MAX_GRAMS, &v)) {
            return false;
        }
        dst[idx] = (uint32_t)v;
    }
    return true;
}

static bool parse_limits(const cJSON *lim, gate_runtime_limits_t *rt)
{
    gate_runtime_limits_clear(rt);
    if (!cJSON_IsObject(lim)) {
        return false;
    }
    const cJSON *it;
    int64_t v;
    if ((it = cJSON_GetObjectItemCaseSensitive(lim, "max_dose_g")) != NULL &&
        !parse_hopper_map(it, rt->max_dose_g)) {
        return false;
    }
    if ((it = cJSON_GetObjectItemCaseSensitive(lim, "daily_budget_g")) != NULL &&
        !parse_hopper_map(it, rt->daily_budget_g)) {
        return false;
    }
    if ((it = cJSON_GetObjectItemCaseSensitive(lim, "daily_total_g")) != NULL) {
        if (!json_int(it, 0, MAX_GRAMS, &v)) {
            return false;
        }
        rt->daily_total_g = (uint32_t)v;
    }
    if ((it = cJSON_GetObjectItemCaseSensitive(lim, "min_interval_s")) != NULL) {
        if (!json_int(it, 0, MAX_SECONDS, &v)) {
            return false;
        }
        rt->min_interval_s = (uint32_t)v;
    }
    if ((it = cJSON_GetObjectItemCaseSensitive(lim, "quiet_hours")) != NULL) {
        if (!cJSON_IsObject(it) ||
            !hhmm_parse(json_str(cJSON_GetObjectItemCaseSensitive(it, "start")), &rt->quiet_start_min) ||
            !hhmm_parse(json_str(cJSON_GetObjectItemCaseSensitive(it, "end")), &rt->quiet_end_min)) {
            return false;
        }
        rt->has_quiet = true; /* start == end is rejected by gate_set_runtime_limits() */
    }
    return true;
}

static bool parse_schedule(const cJSON *root, proto_cmd_t *out)
{
    const cJSON *win = cJSON_GetObjectItemCaseSensitive(root, "window");
    int64_t n;
    if (!cJSON_IsObject(win) ||
        !hhmm_parse(json_str(cJSON_GetObjectItemCaseSensitive(win, "start")), &out->window_start_min) ||
        !hhmm_parse(json_str(cJSON_GetObjectItemCaseSensitive(win, "end")), &out->window_end_min) ||
        out->window_start_min == out->window_end_min ||
        !json_int(cJSON_GetObjectItemCaseSensitive(root, "scatters_per_day"), 0, SCHED_MAX_PER_DAY, &n)) {
        return false;
    }
    out->scatters_per_day = (uint8_t)n;
    const cJSON *lim = cJSON_GetObjectItemCaseSensitive(root, "limits");
    return lim == NULL || parse_limits(lim, &out->limits);
}

static bool parse_body(const cJSON *root, proto_cmd_t *out)
{
    const char *type = json_str(cJSON_GetObjectItemCaseSensitive(root, "type"));
    if (type == NULL || !parse_time(root, "issued_at", &out->issued_at) ||
        !parse_time(root, "expires_at", &out->expires_at)) {
        return false;
    }
    if (strcmp(type, "scatter") == 0) {
        out->type = CMD_SCATTER;
        const cJSON *cue = cJSON_GetObjectItemCaseSensitive(root, "cue");
        if (cue != NULL) {
            if (!cJSON_IsBool(cue)) {
                return false;
            }
            out->cue = cJSON_IsTrue(cue);
        }
        return parse_doses(cJSON_GetObjectItemCaseSensitive(root, "doses"), out);
    }
    if (strcmp(type, "cue") == 0) {
        out->type = CMD_CUE;
        return true;
    }
    if (strcmp(type, "pause") == 0) {
        out->type = CMD_PAUSE;
        return true;
    }
    if (strcmp(type, "resume") == 0) {
        out->type = CMD_RESUME;
        return true;
    }
    if (strcmp(type, "set_schedule") == 0) {
        out->type = CMD_SET_SCHEDULE;
        return parse_schedule(root, out);
    }
    return false; /* unknown type */
}

proto_parse_t proto_parse_cmd(const char *buf, size_t len, proto_cmd_t *out)
{
    memset(out, 0, sizeof *out);
    gate_runtime_limits_clear(&out->limits);
    out->cue = true;
    if (buf == NULL || len == 0 || len > FEEDER_CMD_MAX || !depth_ok(buf, len)) {
        return PROTO_PARSE_DROP;
    }
    cJSON *root = cJSON_ParseWithLength(buf, len);
    proto_parse_t res = PROTO_PARSE_DROP;
    if (cJSON_IsObject(root)) {
        const char *id = json_str(cJSON_GetObjectItemCaseSensitive(root, "id"));
        if (valid_id(id)) {
            memcpy(out->id, id, strlen(id) + 1); /* length checked: <= GATE_ID_MAX_LEN */
            res = parse_body(root, out) ? PROTO_PARSE_OK : PROTO_PARSE_INVALID;
        }
    }
    cJSON_Delete(root); /* NULL is fine */
    return res;
}

/* ------------------------------------------------------------ building */

#define NEED(x)          \
    do {                 \
        if (!(x)) {      \
            goto fail;   \
        }                \
    } while (0)

static const char *source_str(gate_source_t src)
{
    return src == GATE_SRC_REMOTE ? "remote" : "random";
}

static const char *level_str(gate_level_t l)
{
    switch (l) {
    case GATE_LEVEL_OK:
        return "ok";
    case GATE_LEVEL_LOW:
        return "low";
    case GATE_LEVEL_EMPTY:
        return "empty";
    case GATE_LEVEL_UNKNOWN:
    default:
        return "unknown";
    }
}

static const char *fault_str(proto_fault_t f)
{
    switch (f) {
    case PROTO_FAULT_STALL:
        return "stall";
    case PROTO_FAULT_HOPPER_EMPTY:
        return "hopper_empty";
    case PROTO_FAULT_LID_OPEN:
        return "lid_open";
    case PROTO_FAULT_MOTOR_CONTROLLER:
        return "motor_controller";
    case PROTO_FAULT_WATCHDOG:
    default:
        return "watchdog";
    }
}

/* Print and free the tree; NULL root -> NULL. */
static char *finish(cJSON *root)
{
    if (root == NULL) {
        return NULL;
    }
    char *s = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return s;
}

char *proto_build_ack(const char *id, bool accepted, const char *reason, const char *at)
{
    cJSON *o = cJSON_CreateObject();
    NEED(o);
    NEED(cJSON_AddStringToObject(o, "id", id));
    NEED(cJSON_AddStringToObject(o, "status", accepted ? "accepted" : "rejected"));
    if (!accepted && reason != NULL) {
        NEED(cJSON_AddStringToObject(o, "reason", reason));
    }
    if (at != NULL) {
        NEED(cJSON_AddStringToObject(o, "at", at));
    }
    return finish(o);
fail:
    cJSON_Delete(o);
    return NULL;
}

/* type + source + at + command_id: the start of every event. */
static cJSON *event_base(const char *type, gate_source_t src, const char *command_id, const char *at)
{
    cJSON *o = cJSON_CreateObject();
    NEED(o);
    NEED(cJSON_AddStringToObject(o, "type", type));
    NEED(cJSON_AddStringToObject(o, "source", source_str(src)));
    if (at != NULL) {
        NEED(cJSON_AddStringToObject(o, "at", at));
    }
    if (command_id != NULL) {
        NEED(cJSON_AddStringToObject(o, "command_id", command_id));
    }
    return o;
fail:
    cJSON_Delete(o);
    return NULL;
}

char *proto_build_event_cue(gate_source_t src, const char *command_id, const char *at)
{
    return finish(event_base("cue", src, command_id, at));
}

char *proto_build_event_scatter(gate_source_t src, const char *command_id, const gate_dose_t *doses,
                                const uint32_t *actual_g, size_t n, const char *at)
{
    cJSON *o = event_base("scatter", src, command_id, at);
    if (o == NULL) {
        return NULL;
    }
    cJSON *arr = cJSON_AddArrayToObject(o, "doses");
    NEED(arr);
    for (size_t i = 0; i < n; i++) {
        cJSON *d = cJSON_CreateObject();
        NEED(d);
        cJSON_AddItemToArray(arr, d); /* owned by the array from here on */
        NEED(cJSON_AddStringToObject(d, "hopper", feeder_hopper_name(doses[i].hopper)));
        NEED(cJSON_AddNumberToObject(d, "grams", (double)actual_g[i]));
    }
    return finish(o);
fail:
    cJSON_Delete(o);
    return NULL;
}

char *proto_build_event_fault(gate_source_t src, const char *command_id, proto_fault_t code,
                              int hopper, const char *at)
{
    cJSON *o = event_base("fault", src, command_id, at);
    if (o == NULL) {
        return NULL;
    }
    cJSON *f = cJSON_AddObjectToObject(o, "fault");
    NEED(f);
    NEED(cJSON_AddStringToObject(f, "code", fault_str(code)));
    if (hopper >= 0 && hopper < (int)feeder_hopper_count()) {
        NEED(cJSON_AddStringToObject(f, "hopper", feeder_hopper_name((uint8_t)hopper)));
    }
    return finish(o);
fail:
    cJSON_Delete(o);
    return NULL;
}

char *proto_build_state(const proto_state_t *s, const char *at)
{
    cJSON *o = cJSON_CreateObject();
    NEED(o);
    NEED(cJSON_AddStringToObject(o, "status", "online"));
    NEED(cJSON_AddStringToObject(o, "mode", s->paused ? "paused" : "running"));
    NEED(cJSON_AddStringToObject(o, "fw_version", s->fw_version != NULL ? s->fw_version : "unknown"));
    if (at != NULL) {
        NEED(cJSON_AddStringToObject(o, "at", at));
    }
    cJSON *hoppers = cJSON_AddArrayToObject(o, "hoppers");
    NEED(hoppers);
    cJSON *used = cJSON_AddObjectToObject(o, "daily_usage_g");
    NEED(used);
    for (uint8_t i = 0; i < s->hopper_count && i < GATE_MAX_HOPPERS; i++) {
        cJSON *h = cJSON_CreateObject();
        NEED(h);
        cJSON_AddItemToArray(hoppers, h);
        NEED(cJSON_AddStringToObject(h, "hopper", feeder_hopper_name(i)));
        NEED(cJSON_AddStringToObject(h, "level", level_str(s->level[i])));
        NEED(cJSON_AddNumberToObject(used, feeder_hopper_name(i), (double)s->used_g[i]));
    }
    NEED(cJSON_AddBoolToObject(o, "clock_valid", s->clock_valid));
    return finish(o);
fail:
    cJSON_Delete(o);
    return NULL;
}

void proto_free(char *s)
{
    cJSON_free(s);
}
