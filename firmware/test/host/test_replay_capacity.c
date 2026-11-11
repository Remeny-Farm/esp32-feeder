/* Focused replay-capacity regression: actual gate, no ESP-IDF or hardware. */
#include "mini_test.h"
#include "safety_gate.h"

#define NOW (GATE_MIN_VALID_EPOCH + 12 * 3600)
#define RETAIN (GATE_MAX_FUTURE_S + FEEDER_CMD_MAX_TTL_S)

static gate_time_t clock_at(int64_t now)
{
    gate_time_t t = {.now = now, .clock_valid = true, .local_minute = 720, .local_day = 1};
    return t;
}

static gate_result_t command(gate_t *g, gate_time_t *t, const char *id)
{
    gate_cmd_meta_t m = {.id = id, .issued_at = t->now, .expires_at = t->now + 300};
    return gate_check_command(g, t, &m);
}

static void setup(gate_t *g)
{
    gate_limits_t limits = {.hopper_count = 1, .max_dose_g = {10}, .daily_budget_g = {50},
                            .daily_total_g = 50, .min_interval_s = 1};
    CHECK(gate_init(g, &limits));
}

static void fill(gate_t *g, gate_time_t *t)
{
    char id[GATE_ID_BUF];
    for (int i = g->st.seen_count; i < GATE_SEEN_IDS; i++) {
        snprintf(id, sizeof id, "rejected-%d", i);
        gate_cmd_meta_t m = {.id = id, .issued_at = t->now, .expires_at = t->now + 300};
        gate_dose_t invalid = {.hopper = 0, .grams = 0};
        CHECK_EQ(gate_check_scatter(g, t, GATE_SRC_REMOTE, &m, &invalid, 1), GATE_INVALID);
    }
}

static void test_pause_survives_replay_and_overflow(void)
{
    gate_t g;
    setup(&g);
    gate_time_t t = clock_at(NOW);
    gate_cmd_meta_t resume = {.id = "resume-A", .issued_at = NOW, .expires_at = NOW + 300};
    CHECK_EQ(gate_check_command(&g, &t, &resume), GATE_OK);
    gate_set_paused(&g, false);
    CHECK_EQ(command(&g, &t, "pause-B"), GATE_OK);
    gate_set_paused(&g, true);
    fill(&g, &t);
    CHECK_EQ(command(&g, &t, "overflow"), GATE_INVALID);
    CHECK_EQ(gate_check_command(&g, &t, &resume), GATE_DUPLICATE);
    CHECK(gate_paused(&g));
    CHECK_EQ(g.st.seen_count, GATE_SEEN_IDS);
    /* Saturation also stops a running feeder when a pause cannot fit. */
    gate_set_paused(&g, false);
    CHECK_EQ(command(&g, &t, "operator-pause"), GATE_INVALID);
    CHECK(gate_paused(&g));
    CHECK(gate_take_dirty(&g));
}

static void test_overflow_extends_quiet_recovery(void)
{
    gate_t g;
    setup(&g);
    gate_time_t t = clock_at(NOW);
    fill(&g, &t);
    CHECK_EQ(command(&g, &t, "overflow"), GATE_INVALID);
    t.now += RETAIN;
    CHECK_EQ(command(&g, &t, "overflow"), GATE_INVALID); /* inclusive deadline */
    t.now += RETAIN;
    CHECK_EQ(command(&g, &t, "other-overflow"), GATE_INVALID);
    t.now += RETAIN + 1;
    CHECK_EQ(command(&g, &t, "fresh-after-silence"), GATE_OK);
    CHECK_EQ(g.st.seen_count, 1);
    CHECK(gate_paused(&g)); /* recovery never resumes automatically */
}

static void test_restore_and_clockless_rejections_keep_ids(void)
{
    gate_t g, restored;
    setup(&g);
    gate_time_t t = clock_at(NOW);
    CHECK_EQ(command(&g, &t, "resume-A"), GATE_OK);
    fill(&g, &t);
    setup(&restored);
    CHECK(gate_restore(&restored, gate_state(&g)));
    t.clock_valid = false;
    CHECK_EQ(command(&restored, &t, "no-clock-overflow"), GATE_NO_CLOCK);
    CHECK(gate_paused(&restored));
    CHECK(!gate_note_id(&restored, "parse-overflow"));
    t.clock_valid = true;
    CHECK_EQ(command(&restored, &t, "resume-A"), GATE_DUPLICATE);
    t.now += RETAIN + 1;
    CHECK_EQ(command(&restored, &t, "after-recovery"), GATE_OK);
}

int main(void)
{
    RUN_TEST(test_pause_survives_replay_and_overflow);
    RUN_TEST(test_overflow_extends_quiet_recovery);
    RUN_TEST(test_restore_and_clockless_rejections_keep_ids);
    return mt_summary();
}
