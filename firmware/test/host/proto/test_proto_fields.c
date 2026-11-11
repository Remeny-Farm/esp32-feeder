/* Compile with real proto.c + cJSON; run from repository root for fixtures. */
#include "mini_test.h"
#include "proto.h"
#include "cJSON.h"
#include <stdlib.h>

static const char *names[] = {"grain", "pellet", "mealworm"};
uint8_t feeder_hopper_count(void) { return 3; }
const char *feeder_hopper_name(uint8_t i) { return i < 3 ? names[i] : ""; }
int feeder_hopper_index(const char *s)
{
    for (int i = 0; s != NULL && i < 3; i++) {
        if (strcmp(s, names[i]) == 0) return i;
    }
    return -1;
}

static void fixture(const char *name, proto_parse_t expected)
{
    char path[256], data[4096];
    snprintf(path, sizeof path, "protocol/examples/%s", name);
    FILE *f = fopen(path, "rb");
    CHECK(f != NULL);
    if (f == NULL) return;
    size_t n = fread(data, 1, sizeof data, f);
    fclose(f);
    proto_cmd_t cmd;
    CHECK_EQ(proto_parse_cmd(data, n, &cmd), expected);
}

static void test_committed_fixtures(void)
{
    fixture("invalid/cmd-schedule-unknown-limit.json", PROTO_PARSE_INVALID);
    fixture("invalid/cmd-extra-field.json", PROTO_PARSE_INVALID);
    fixture("invalid/cmd-cue-with-doses.json", PROTO_PARSE_INVALID);
    fixture("valid/cmd-set-schedule-limits.json", PROTO_PARSE_OK);
    fixture("valid/cmd-scatter.json", PROTO_PARSE_OK);
    fixture("valid/cmd-cue.json", PROTO_PARSE_OK);
    fixture("valid/cmd-pause.json", PROTO_PARSE_OK);
    fixture("valid/cmd-resume.json", PROTO_PARSE_OK);
}

static void test_nested_fields(void)
{
    const char *prefix = "\"id\":\"5f0c9a52-7d1e-4b8e-9a43-1f2d3c4b5a69\","
                         "\"issued_at\":\"2026-11-05T10:00:00Z\",\"expires_at\":\"2026-11-05T10:00:30Z\",";
    const char *bodies[] = {
        "\"type\":\"scatter\",\"doses\":[{\"hopper\":\"grain\",\"grams\":1,\"gramz\":2}]",
        "\"type\":\"set_schedule\",\"window\":{\"start\":\"07:00\",\"end\":\"19:00\",\"extra\":1},\"scatters_per_day\":1",
        "\"type\":\"set_schedule\",\"window\":{\"start\":\"07:00\",\"end\":\"19:00\"},\"scatters_per_day\":1,\"limits\":{\"quiet_hours\":{\"start\":\"21:00\",\"end\":\"07:00\",\"extra\":1}}"
    };
    for (size_t i = 0; i < sizeof bodies / sizeof bodies[0]; i++) {
        char data[1024];
        snprintf(data, sizeof data, "{%s%s}", prefix, bodies[i]);
        proto_cmd_t cmd;
        CHECK_EQ(proto_parse_cmd(data, strlen(data), &cmd), PROTO_PARSE_INVALID);
    }
}

static void test_actual_busy_ack_builder(void)
{
    char *json = proto_build_ack("5f0c9a52-7d1e-4b8e-9a43-1f2d3c4b5a69", false,
                                gate_reason_str((gate_result_t)(GATE_INVALID + 1)), "2026-11-05T10:00:00Z");
    CHECK(json != NULL);
    if (!json) return;
    cJSON *ack = cJSON_Parse(json);
    CHECK(ack != NULL);
    if (ack) {
        const cJSON *status = cJSON_GetObjectItemCaseSensitive(ack, "status");
        const cJSON *reason = cJSON_GetObjectItemCaseSensitive(ack, "reason");
        CHECK(cJSON_IsString(status)); CHECK(cJSON_IsString(reason));
        if (cJSON_IsString(status)) CHECK_STR(status->valuestring, "rejected");
        if (cJSON_IsString(reason)) CHECK_STR(reason->valuestring, "busy");
        cJSON_Delete(ack);
    }
    const char *path = getenv("FEEDER_BUSY_ACK_JSON");
    if (path) { FILE *f = fopen(path, "wb"); CHECK(f != NULL); if (f) { CHECK_EQ(fwrite(json, 1, strlen(json), f), strlen(json)); fclose(f); } }
    proto_free(json);
}

int main(void)
{
    RUN_TEST(test_actual_busy_ack_builder);
    RUN_TEST(test_committed_fixtures);
    RUN_TEST(test_nested_fields);
    return mt_summary();
}
