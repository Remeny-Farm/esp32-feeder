/* Focused replay-capacity regression: actual gate, no ESP-IDF or hardware. */
#include "mini_test.h"
#include "safety_gate.h"

#define TEST_BUSY ((gate_result_t)(GATE_INVALID + 1))

#define NOW (GATE_MIN_VALID_EPOCH + 12 * 3600)
#define RETAIN (GATE_MAX_FUTURE_S + FEEDER_CMD_MAX_TTL_S)

static gate_time_t clock_at(int64_t now)
{
    gate_time_t t = {.now = now, .clock_valid = true, .local_minute = 720, .local_day = 1};
#if GATE_STATE_VERSION >= 2
    t.monotonic_s = (uint64_t)now;
#endif
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
    CHECK_EQ(command(&g, &t, "overflow"), TEST_BUSY);
    CHECK_EQ(gate_check_command(&g, &t, &resume), GATE_DUPLICATE);
    CHECK(gate_paused(&g));
    CHECK_EQ(g.st.seen_count, GATE_SEEN_IDS);
    /* Saturation also stops a running feeder when a pause cannot fit. */
    gate_set_paused(&g, false);
    CHECK_EQ(command(&g, &t, "operator-pause"), TEST_BUSY);
    CHECK(gate_paused(&g));
    CHECK(gate_take_dirty(&g));
}

static void test_overflow_extends_quiet_recovery(void)
{
    gate_t g;
    setup(&g);
    gate_time_t t = clock_at(NOW);
    fill(&g, &t);
    CHECK_EQ(command(&g, &t, "overflow"), TEST_BUSY);
    t.now += RETAIN;
#if GATE_STATE_VERSION >= 2
    t.monotonic_s += RETAIN;
#endif
    CHECK_EQ(command(&g, &t, "overflow"), TEST_BUSY); /* inclusive deadline */
    t.now += RETAIN;
#if GATE_STATE_VERSION >= 2
    t.monotonic_s += RETAIN;
#endif
    CHECK_EQ(command(&g, &t, "other-overflow"), TEST_BUSY);
    t.now += RETAIN + 1;
#if GATE_STATE_VERSION >= 2
    t.monotonic_s += RETAIN + 1;
#endif
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
    CHECK_EQ(command(&restored, &t, "no-clock-overflow"), TEST_BUSY);
    CHECK(gate_paused(&restored));
    t.clock_valid = true;
    CHECK_EQ(command(&restored, &t, "resume-A"), GATE_DUPLICATE);
    t.now += RETAIN + 1;
#if GATE_STATE_VERSION >= 2
    t.monotonic_s += RETAIN + 1;
#endif
    CHECK_EQ(command(&restored, &t, "after-recovery"), GATE_OK);
}

static void test_future_rejected_payload_stays_consumed_after_restore(void)
{
    gate_t g, restored;
    setup(&g);
    gate_time_t t = clock_at(NOW);
    gate_cmd_meta_t a = {.id = "future-resume", .issued_at = NOW + 1000, .expires_at = NOW + 1300};
    CHECK_EQ(gate_check_command(&g, &t, &a), GATE_EXPIRED);
    t = clock_at(NOW + 1);
    CHECK_EQ(command(&g, &t, "pause-B"), GATE_OK);
    gate_set_paused(&g, true);
    setup(&restored);
    CHECK(gate_restore(&restored, gate_state(&g)));
    /* Start a conservative monotonic interval after restore, then wait it out. */
    CHECK_EQ(gate_check_command(&restored, &t, &a), GATE_EXPIRED);
    t = clock_at(NOW + 1000);
    CHECK_EQ(gate_check_command(&restored, &t, &a), GATE_DUPLICATE);
    CHECK(gate_paused(&restored));
}

static void test_forward_backward_wall_step_cannot_forget_resume(void)
{
    gate_t g;
    setup(&g);
    gate_time_t t = clock_at(NOW);
    gate_cmd_meta_t a = {.id = "resume-A", .issued_at = NOW, .expires_at = NOW + 300};
    CHECK_EQ(gate_check_command(&g, &t, &a), GATE_OK);
    t = clock_at(NOW + 1);
    CHECK_EQ(command(&g, &t, "pause-B"), GATE_OK);
    gate_set_paused(&g, true);
    t.now = NOW + 1000; /* Only one actual elapsed second, no real quiet interval. */
#if GATE_STATE_VERSION >= 2
    t.monotonic_s++;
#endif
    CHECK_EQ(command(&g, &t, "unrelated"), GATE_OK);
    CHECK_EQ(g.st.seen_count, 3);
    t.now = NOW + 2;
#if GATE_STATE_VERSION >= 2
    t.monotonic_s++;
#endif
    CHECK_EQ(gate_check_command(&g, &t, &a), GATE_DUPLICATE);
    CHECK(gate_paused(&g));
}

static void test_rollback_after_real_expiry_and_reboot_fails_closed(void)
{
    gate_t g, restored;
    setup(&g);
    gate_time_t t = clock_at(NOW);
    gate_cmd_meta_t a = {.id = "resume-A", .issued_at = NOW, .expires_at = NOW + 300};
    CHECK_EQ(gate_check_command(&g, &t, &a), GATE_OK);
    t = clock_at(NOW + 1);
    CHECK_EQ(command(&g, &t, "pause-B"), GATE_OK);
    gate_set_paused(&g, true);
    t = clock_at(NOW + 1000); /* Real quiet interval and expiry both passed. */
    CHECK_EQ(command(&g, &t, "fresh"), GATE_OK);
    CHECK_EQ(g.st.seen_count, 1);
    setup(&restored);
    CHECK(gate_restore(&restored, gate_state(&g)));
    t = clock_at(NOW + 2);
    CHECK_EQ(gate_check_command(&restored, &t, &a), GATE_EXPIRED);
    CHECK_EQ(command(&restored, &t, "fresh-after-clock-correction"), GATE_OK);
    CHECK(gate_paused(&restored));
}

static void test_never_valid_expiry_does_not_poison_recovery(void)
{
    gate_t g;
    setup(&g);
    gate_time_t t = clock_at(NOW);
    gate_cmd_meta_t bad_ttl = {.id = "bad-ttl", .issued_at = NOW, .expires_at = NOW + 1000000};
    gate_cmd_meta_t bad_order = {.id = "bad-order", .issued_at = NOW + 1000001, .expires_at = NOW + 1000000};
    CHECK_EQ(gate_check_command(&g, &t, &bad_ttl), GATE_EXPIRED);
    CHECK_EQ(gate_check_command(&g, &t, &bad_order), GATE_EXPIRED);
    fill(&g, &t);
    CHECK_EQ(command(&g, &t, "overflow"), TEST_BUSY);
    t = clock_at(NOW + RETAIN + 1);
    CHECK_EQ(command(&g, &t, "fresh-after-invalid-metadata"), GATE_OK);
    CHECK_EQ(g.st.seen_count, 1);
    CHECK(gate_paused(&g));
}

static void test_one_future_id_does_not_pin_other_expired_slots(void)
{
    gate_t g, restored;
    setup(&g);
    gate_time_t t = clock_at(NOW);
    gate_cmd_meta_t future = {.id = "one-future", .issued_at = NOW + 100000000, .expires_at = NOW + 100000300};
    CHECK_EQ(gate_check_command(&g, &t, &future), GATE_EXPIRED);
    fill(&g, &t);
    CHECK_EQ(command(&g, &t, "unrecorded-capacity"), TEST_BUSY);
    t = clock_at(NOW + RETAIN + 1);
    CHECK_EQ(command(&g, &t, "new-normal-resume"), GATE_OK);
    CHECK_EQ(g.st.seen_count, 2); /* future id plus this new normal id */
    CHECK(gate_paused(&g));
    setup(&restored); CHECK(gate_restore(&restored, gate_state(&g)));
    t = clock_at(future.issued_at);
    CHECK_EQ(gate_check_command(&restored, &t, &future), GATE_DUPLICATE);
}

static void test_repeated_id_keeps_max_expiry(void)
{
    gate_t g;
    setup(&g);
    gate_time_t t = clock_at(NOW);
    gate_cmd_meta_t later = {.id = "max-id", .issued_at = NOW + 1000, .expires_at = NOW + 1300};
    CHECK_EQ(gate_check_command(&g, &t, &later), GATE_EXPIRED);
    gate_cmd_meta_t earlier = {.id = "max-id", .issued_at = NOW, .expires_at = NOW + 300};
    CHECK_EQ(gate_check_command(&g, &t, &earlier), GATE_DUPLICATE);
    t = clock_at(NOW + RETAIN + 1);
    CHECK_EQ(command(&g, &t, "fresh"), GATE_OK);
    CHECK_EQ(g.st.seen_count, 2);
    t = clock_at(NOW + 1000);
    CHECK_EQ(gate_check_command(&g, &t, &later), GATE_DUPLICATE);
}

static void test_wall_outlier_cannot_raise_remote_or_random_floor(void)
{
    gate_t g, restored;
    setup(&g);
    gate_time_t t = clock_at(NOW);
    CHECK_EQ(command(&g, &t, "pause"), GATE_OK);
    gate_set_paused(&g, true);
    t.now = NOW + INT64_C(2300000000); /* one caller wall sample; monotonic did not advance */
    gate_cmd_meta_t old = {.id = "clock-probe", .issued_at = NOW, .expires_at = NOW + 300};
    CHECK_EQ(gate_check_command(&g, &t, &old), GATE_EXPIRED);
    setup(&restored); CHECK(gate_restore(&restored, gate_state(&g)));
    t = clock_at(NOW + 2);
    CHECK_EQ(command(&restored, &t, "normal-after-correction"), GATE_OK);
    CHECK_EQ(gate_check_cue(&restored, &t, GATE_SRC_RANDOM, NULL), GATE_PAUSED);
    CHECK(gate_paused(&restored));
}

static void test_capacity_rejection_does_not_consume_future_id(void)
{
    gate_t g;
    setup(&g);
    gate_time_t t = clock_at(NOW);
    fill(&g, &t);
    gate_cmd_meta_t unconsumed = {.id = "future-retry", .issued_at = NOW + 1000, .expires_at = NOW + 1300};
    CHECK_EQ(gate_check_command(&g, &t, &unconsumed), TEST_BUSY);
    t = clock_at(NOW + RETAIN + 1);
    CHECK_EQ(command(&g, &t, "new-normal"), GATE_OK);
    t = clock_at(NOW + 1000);
    CHECK_EQ(gate_check_command(&g, &t, &unconsumed), GATE_OK); /* busy truthfully never consumed it */
    CHECK_EQ(gate_check_command(&g, &t, &unconsumed), GATE_DUPLICATE);
    CHECK(gate_paused(&g)); /* admission check does not auto-resume */
}

int main(void)
{
    RUN_TEST(test_one_future_id_does_not_pin_other_expired_slots);
    RUN_TEST(test_repeated_id_keeps_max_expiry);
    RUN_TEST(test_wall_outlier_cannot_raise_remote_or_random_floor);
    RUN_TEST(test_capacity_rejection_does_not_consume_future_id);
    RUN_TEST(test_never_valid_expiry_does_not_poison_recovery);
    RUN_TEST(test_future_rejected_payload_stays_consumed_after_restore);
    RUN_TEST(test_forward_backward_wall_step_cannot_forget_resume);
    RUN_TEST(test_rollback_after_real_expiry_and_reboot_fails_closed);
    RUN_TEST(test_pause_survives_replay_and_overflow);
    RUN_TEST(test_overflow_extends_quiet_recovery);
    RUN_TEST(test_restore_and_clockless_rejections_keep_ids);
    return mt_summary();
}
