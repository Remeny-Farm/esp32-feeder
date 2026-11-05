/* Host tests for the safety gate. Local time == UTC in every test. */
#include "mini_test.h"
#include "safety_gate.h"

#include <stdio.h>
#include <string.h>

#define EPOCH0 INT64_C(1793491200) /* 2026-11-01T00:00:00Z, the clock floor */
#define DAY(d) (EPOCH0 + (int64_t)(d) * 86400)
#define NOON(d) (DAY(d) + 12 * 3600)

/* Fixture limits (test values, not welfare values). */
#define MAX0 10
#define MAX1 10
#define MAX2 5
#define BUD0 30
#define BUD1 30
#define BUD2 20
#define TOTAL 50
#define INTERVAL 1800
#define QSTART (21 * 60)
#define QEND (7 * 60)

static gate_limits_t limits(void)
{
    gate_limits_t l;
    memset(&l, 0, sizeof l);
    l.hopper_count = 3;
    l.max_dose_g[0] = MAX0;
    l.max_dose_g[1] = MAX1;
    l.max_dose_g[2] = MAX2;
    l.daily_budget_g[0] = BUD0;
    l.daily_budget_g[1] = BUD1;
    l.daily_budget_g[2] = BUD2;
    l.daily_total_g = TOTAL;
    l.min_interval_s = INTERVAL;
    l.quiet_start_min = QSTART;
    l.quiet_end_min = QEND;
    return l;
}

static void setup(gate_t *g)
{
    gate_limits_t l = limits();
    CHECK(gate_init(g, &l));
}

static gate_time_t at(int64_t now)
{
    gate_time_t t;
    t.now = now;
    t.clock_valid = true;
    t.local_day = (now - EPOCH0) / 86400;
    t.local_minute = (uint16_t)(((now - EPOCH0) % 86400) / 60);
    return t;
}

static gate_cmd_meta_t meta(const char *id, int64_t now)
{
    gate_cmd_meta_t m;
    m.id = id;
    m.issued_at = now - 1;
    m.expires_at = now + 30;
    return m;
}

/* Remote scatter with one dose. */
static gate_result_t rs(gate_t *g, int64_t now, const char *id, uint8_t hopper, int64_t grams)
{
    gate_time_t t = at(now);
    gate_cmd_meta_t m = meta(id, now);
    gate_dose_t d;
    d.hopper = hopper;
    d.grams = grams;
    return gate_check_scatter(g, &t, GATE_SRC_REMOTE, &m, &d, 1);
}

/* Random scatter with one dose. */
static gate_result_t rnd(gate_t *g, int64_t now, uint8_t hopper, int64_t grams)
{
    gate_time_t t = at(now);
    gate_dose_t d;
    d.hopper = hopper;
    d.grams = grams;
    return gate_check_scatter(g, &t, GATE_SRC_RANDOM, NULL, &d, 1);
}

static void commit(gate_t *g, int64_t now, uint32_t a, uint32_t b, uint32_t c)
{
    gate_time_t t = at(now);
    uint32_t grams[GATE_MAX_HOPPERS];
    grams[0] = a;
    grams[1] = b;
    grams[2] = c;
    gate_commit_scatter(g, &t, grams);
}

static uint32_t used(gate_t *g, int64_t now, uint8_t hopper)
{
    gate_time_t t = at(now);
    return gate_used_today(g, &t, hopper);
}

static uint32_t used_total(gate_t *g, int64_t now)
{
    gate_time_t t = at(now);
    return gate_used_total_today(g, &t);
}

#define REASON(res, want) CHECK_STR(gate_reason_str(res), want)

/* ---------------------------------------------------------------- */

static void test_reason_strings(void)
{
    CHECK_STR(gate_reason_str(GATE_OK), NULL);
    CHECK_STR(gate_reason_str(GATE_EXPIRED), "expired");
    CHECK_STR(gate_reason_str(GATE_DUPLICATE), "duplicate");
    CHECK_STR(gate_reason_str(GATE_NO_CLOCK), "no_clock");
    CHECK_STR(gate_reason_str(GATE_PAUSED), "paused");
    CHECK_STR(gate_reason_str(GATE_QUIET_HOURS), "quiet_hours");
    CHECK_STR(gate_reason_str(GATE_MIN_INTERVAL), "min_interval");
    CHECK_STR(gate_reason_str(GATE_DOSE_LIMIT), "dose_limit");
    CHECK_STR(gate_reason_str(GATE_BUDGET_EXCEEDED), "daily_budget_exceeded");
    CHECK_STR(gate_reason_str(GATE_HOPPER_EMPTY), "hopper_empty");
    CHECK_STR(gate_reason_str(GATE_FAULT), "fault");
    CHECK_STR(gate_reason_str(GATE_INVALID), "invalid");
}

static void test_accepts_a_normal_remote_and_random_scatter(void)
{
    gate_t g;
    setup(&g);
    CHECK_EQ(rs(&g, NOON(1), "id-1", 0, 5), GATE_OK);
    CHECK_EQ(rnd(&g, NOON(1), 1, 5), GATE_OK);
}

static void test_no_clock_remote(void)
{
    gate_t g;
    gate_time_t t;
    gate_cmd_meta_t m = meta("id-nc", NOON(1));
    gate_dose_t d = {0, 5};
    setup(&g);

    t = at(NOON(1));
    t.clock_valid = false; /* never synced */
    REASON(gate_check_scatter(&g, &t, GATE_SRC_REMOTE, &m, &d, 1), "no_clock");
    REASON(gate_check_cue(&g, &t, GATE_SRC_REMOTE, &m), "no_clock");
    REASON(gate_check_command(&g, &t, &m), "no_clock");

    t = at(EPOCH0 - 1); /* RTC at 1970-ish default: before the floor */
    REASON(gate_check_scatter(&g, &t, GATE_SRC_REMOTE, &m, &d, 1), "no_clock");

    t = at(NOON(1));
    t.local_minute = 1440; /* caller could not convert: fail closed */
    REASON(gate_check_scatter(&g, &t, GATE_SRC_REMOTE, &m, &d, 1), "no_clock");
}

static void test_no_clock_random(void)
{
    gate_t g;
    gate_time_t t;
    gate_dose_t d = {0, 5};
    setup(&g);

    t = at(NOON(1));
    t.clock_valid = false;
    REASON(gate_check_scatter(&g, &t, GATE_SRC_RANDOM, NULL, &d, 1), "no_clock");
    REASON(gate_check_cue(&g, &t, GATE_SRC_RANDOM, NULL), "no_clock");

    t = at(100); /* time before the floor */
    REASON(gate_check_scatter(&g, &t, GATE_SRC_RANDOM, NULL, &d, 1), "no_clock");

    CHECK_EQ(rnd(&g, NOON(1), 0, 5), GATE_OK); /* after the first sync */
}

static void test_id_recorded_when_rejected_for_no_clock(void)
{
    gate_t g;
    gate_time_t t = at(NOON(1));
    gate_cmd_meta_t m = meta("id-late", NOON(1));
    gate_dose_t d = {0, 5};
    setup(&g);
    t.clock_valid = false;
    REASON(gate_check_scatter(&g, &t, GATE_SRC_REMOTE, &m, &d, 1), "no_clock");
    /* Clock arrives; QoS 1 redelivers the same command: never executed. */
    REASON(rs(&g, NOON(1), "id-late", 0, 5), "duplicate");
}

static void test_expired(void)
{
    gate_t g;
    gate_time_t t;
    gate_cmd_meta_t m;
    gate_dose_t d = {0, 5};
    int64_t now = NOON(1);
    setup(&g);

    t = at(now);
    m.id = "e1";
    m.issued_at = now - 60;
    m.expires_at = now - 1; /* one second past */
    REASON(gate_check_scatter(&g, &t, GATE_SRC_REMOTE, &m, &d, 1), "expired");

    m.id = "e2";
    m.expires_at = now; /* now == expires_at is still valid */
    CHECK_EQ(gate_check_scatter(&g, &t, GATE_SRC_REMOTE, &m, &d, 1), GATE_OK);

    m.id = "e3"; /* issued more than 5 minutes in the future */
    m.issued_at = now + GATE_MAX_FUTURE_S + 1;
    m.expires_at = now + 600;
    REASON(gate_check_scatter(&g, &t, GATE_SRC_REMOTE, &m, &d, 1), "expired");

    m.id = "e4"; /* exactly 5 minutes in the future is allowed */
    m.issued_at = now + GATE_MAX_FUTURE_S;
    CHECK_EQ(gate_check_command(&g, &t, &m), GATE_OK);
}

static void test_ttl_cap(void)
{
    gate_t g;
    gate_time_t t;
    gate_cmd_meta_t m;
    gate_dose_t d = {0, 5};
    int64_t now = NOON(1);
    setup(&g);
    t = at(now);

    CHECK_EQ(FEEDER_CMD_MAX_TTL_S, 300);

    m.id = "ttl-300"; /* exactly the cap: accepted */
    m.issued_at = now - 10;
    m.expires_at = m.issued_at + FEEDER_CMD_MAX_TTL_S;
    CHECK_EQ(gate_check_scatter(&g, &t, GATE_SRC_REMOTE, &m, &d, 1), GATE_OK);

    m.id = "ttl-301"; /* one second over the cap */
    m.expires_at = m.issued_at + FEEDER_CMD_MAX_TTL_S + 1;
    REASON(gate_check_scatter(&g, &t, GATE_SRC_REMOTE, &m, &d, 1), "expired");
    /* the id was recorded: a replay with a valid lifetime is a duplicate */
    m.expires_at = m.issued_at + 30;
    REASON(gate_check_scatter(&g, &t, GATE_SRC_REMOTE, &m, &d, 1), "duplicate");

    m.id = "ttl-neg"; /* expires before it was issued */
    m.issued_at = now - 10;
    m.expires_at = now - 11;
    REASON(gate_check_scatter(&g, &t, GATE_SRC_REMOTE, &m, &d, 1), "expired");
    m.expires_at = now + 20;
    REASON(gate_check_scatter(&g, &t, GATE_SRC_REMOTE, &m, &d, 1), "duplicate");

    m.id = "ttl-ovf"; /* extreme values must not overflow */
    m.issued_at = INT64_MIN;
    m.expires_at = INT64_MAX;
    REASON(gate_check_command(&g, &t, &m), "expired");

    m.id = "ttl-cue"; /* the cap applies to every remote command type */
    m.issued_at = now - 10;
    m.expires_at = m.issued_at + FEEDER_CMD_MAX_TTL_S + 1;
    REASON(gate_check_cue(&g, &t, GATE_SRC_REMOTE, &m), "expired");
    m.expires_at = m.issued_at + 30;
    REASON(gate_check_command(&g, &t, &m), "duplicate");
}

static void test_duplicate_replay_of_accepted_id(void)
{
    gate_t g;
    setup(&g);
    CHECK_EQ(rs(&g, NOON(1), "dup-a", 0, 5), GATE_OK);
    REASON(rs(&g, NOON(1) + 5, "dup-a", 0, 5), "duplicate");
    /* A different id is fine (min_interval only after a commit). */
    CHECK_EQ(rs(&g, NOON(1) + 5, "dup-b", 0, 5), GATE_OK);
}

static void test_duplicate_replay_of_rejected_id(void)
{
    gate_t g;
    setup(&g);
    /* 22:00 is in the quiet window: rejected for quiet_hours. */
    int64_t quiet = DAY(1) + 22 * 3600;
    REASON(rs(&g, quiet, "dup-q", 0, 5), "quiet_hours");
    /* Same id at noon, still inside its expiry window: now a replay. */
    gate_time_t t = at(quiet);
    gate_cmd_meta_t m = meta("dup-q", quiet);
    m.expires_at = m.issued_at + FEEDER_CMD_MAX_TTL_S; /* longest valid lifetime */
    gate_dose_t d = {0, 5};
    t = at(quiet + 100);
    REASON(gate_check_scatter(&g, &t, GATE_SRC_REMOTE, &m, &d, 1), "duplicate");
    /* Rejected for an invalid dose list: also recorded. */
    gate_dose_t bad = {0, 0};
    gate_cmd_meta_t m2 = meta("dup-i", NOON(2));
    t = at(NOON(2));
    REASON(gate_check_scatter(&g, &t, GATE_SRC_REMOTE, &m2, &bad, 1), "invalid");
    REASON(rs(&g, NOON(2), "dup-i", 0, 5), "duplicate");
    /* Rejected for dose_limit and for budget: also recorded. */
    REASON(rs(&g, NOON(2), "dup-d", 0, MAX0 + 1), "dose_limit");
    REASON(rs(&g, NOON(2), "dup-d", 0, 5), "duplicate");
}

static void test_duplicate_ring_keeps_last_32_ids(void)
{
    gate_t g;
    char id[GATE_ID_BUF];
    setup(&g);
    gate_time_t t = at(NOON(1));
    for (int i = 0; i < 40; i++) {
        gate_cmd_meta_t m;
        snprintf(id, sizeof id, "ring-%03d", i);
        m = meta(id, NOON(1));
        CHECK_EQ(gate_check_command(&g, &t, &m), GATE_OK);
    }
    /* The last 32 (and more: the ring holds GATE_SEEN_IDS) are remembered. */
    CHECK(GATE_SEEN_IDS >= 32);
    for (int i = 8; i < 40; i++) {
        gate_cmd_meta_t m;
        snprintf(id, sizeof id, "ring-%03d", i);
        m = meta(id, NOON(1));
        CHECK_EQ(gate_check_command(&g, &t, &m), GATE_DUPLICATE);
    }
    /* The ring evicts oldest first once it is full. */
    for (int i = 40; i < 40 + GATE_SEEN_IDS; i++) {
        gate_cmd_meta_t m;
        snprintf(id, sizeof id, "ring-%03d", i);
        m = meta(id, NOON(1));
        CHECK_EQ(gate_check_command(&g, &t, &m), GATE_OK);
    }
    {
        gate_cmd_meta_t m = meta("ring-000", NOON(1));
        CHECK_EQ(gate_check_command(&g, &t, &m), GATE_OK); /* forgotten: documented limit */
    }
}

static void test_paused(void)
{
    gate_t g;
    gate_time_t t = at(NOON(1));
    gate_cmd_meta_t m1 = meta("p1", NOON(1)), m2 = meta("p2", NOON(1)), m3 = meta("p3", NOON(1));
    setup(&g);
    gate_set_paused(&g, true);
    CHECK(gate_paused(&g));
    REASON(rs(&g, NOON(1), "p-s", 0, 5), "paused");
    REASON(gate_check_cue(&g, &t, GATE_SRC_REMOTE, &m1), "paused");
    REASON(rnd(&g, NOON(1), 0, 5), "paused"); /* pause also stops the random path */
    /* pause/resume/set_schedule are still accepted */
    CHECK_EQ(gate_check_command(&g, &t, &m2), GATE_OK);
    gate_set_paused(&g, false);
    CHECK(!gate_paused(&g));
    CHECK_EQ(gate_check_command(&g, &t, &m3), GATE_OK);
    CHECK_EQ(rs(&g, NOON(1), "p-s2", 0, 5), GATE_OK);
}

static void test_fault(void)
{
    gate_t g;
    gate_time_t t = at(NOON(1));
    setup(&g);
    gate_fault_raise(&g, GATE_FAULT_STALL);
    CHECK_EQ(gate_faults(&g), GATE_FAULT_STALL);
    REASON(rs(&g, NOON(1), "f1", 0, 5), "fault");
    REASON(rnd(&g, NOON(1), 0, 5), "fault");
    REASON(gate_check_cue(&g, &t, GATE_SRC_RANDOM, NULL), "fault");
    gate_fault_raise(&g, GATE_FAULT_LID_OPEN);
    gate_fault_clear(&g, GATE_FAULT_LID_OPEN); /* lid closed again: stall stays latched */
    REASON(rnd(&g, NOON(1), 0, 5), "fault");
    gate_fault_clear_all(&g); /* service action */
    CHECK_EQ(rnd(&g, NOON(1), 0, 5), GATE_OK);
    gate_fault_raise(&g, GATE_FAULT_MOTOR_CONTROLLER);
    REASON(rnd(&g, NOON(1), 0, 5), "fault");
    gate_fault_clear_all(&g);
    gate_fault_raise(&g, GATE_FAULT_WATCHDOG);
    REASON(rnd(&g, NOON(1), 0, 5), "fault");
}

static void test_quiet_hours_cross_midnight(void)
{
    gate_t g;
    setup(&g);
    CHECK(!gate_in_quiet_hours(&g, 20 * 60 + 59));
    CHECK(gate_in_quiet_hours(&g, 21 * 60)); /* start is quiet */
    CHECK(gate_in_quiet_hours(&g, 23 * 60 + 59));
    CHECK(gate_in_quiet_hours(&g, 0)); /* after midnight, same window */
    CHECK(gate_in_quiet_hours(&g, 6 * 60 + 59));
    CHECK(!gate_in_quiet_hours(&g, 7 * 60)); /* end is not quiet */
    CHECK(!gate_in_quiet_hours(&g, 12 * 60));

    /* through the check path, both sides of midnight and both sources */
    REASON(rs(&g, DAY(1) + 23 * 3600 + 30 * 60, "q-late", 0, 5), "quiet_hours");
    REASON(rs(&g, DAY(2) + 30 * 60, "q-early", 0, 5), "quiet_hours");
    REASON(rnd(&g, DAY(2) + 3 * 3600, 0, 5), "quiet_hours");
    CHECK_EQ(rnd(&g, DAY(2) + 7 * 3600, 0, 5), GATE_OK);
    {
        gate_time_t t = at(DAY(2) + 23 * 3600);
        gate_cmd_meta_t m = meta("q-cue", t.now);
        REASON(gate_check_cue(&g, &t, GATE_SRC_REMOTE, &m), "quiet_hours");
    }
}

static void test_quiet_hours_same_day_window_and_disabled(void)
{
    gate_t g;
    gate_limits_t l = limits();
    l.quiet_start_min = 60;
    l.quiet_end_min = 120; /* 01:00-02:00, does not cross midnight */
    CHECK(gate_init(&g, &l));
    CHECK(!gate_in_quiet_hours(&g, 59));
    CHECK(gate_in_quiet_hours(&g, 60));
    CHECK(gate_in_quiet_hours(&g, 119));
    CHECK(!gate_in_quiet_hours(&g, 120));
    CHECK(!gate_in_quiet_hours(&g, 22 * 60));

    l.quiet_start_min = 0;
    l.quiet_end_min = 0; /* disabled */
    CHECK(gate_init(&g, &l));
    for (uint16_t m = 0; m < 1440; m++) {
        CHECK(!gate_in_quiet_hours(&g, m));
    }
}

static void test_quiet_hours_runtime_can_only_widen(void)
{
    gate_t g;
    gate_runtime_limits_t rt;
    setup(&g);

    /* runtime window inside the compiled one: nothing is removed */
    gate_runtime_limits_clear(&rt);
    rt.has_quiet = true;
    rt.quiet_start_min = 23 * 60;
    rt.quiet_end_min = 5 * 60;
    CHECK_EQ(gate_set_runtime_limits(&g, &rt, NULL), GATE_OK);
    CHECK(gate_in_quiet_hours(&g, 21 * 60 + 30)); /* compiled-only minute still quiet */
    CHECK(gate_in_quiet_hours(&g, 6 * 60));
    CHECK(!gate_in_quiet_hours(&g, 12 * 60));

    /* disjoint runtime window: the union is quiet, the compiled part stays */
    rt.quiet_start_min = 12 * 60;
    rt.quiet_end_min = 13 * 60;
    CHECK_EQ(gate_set_runtime_limits(&g, &rt, NULL), GATE_OK);
    CHECK(gate_in_quiet_hours(&g, 12 * 60 + 30));
    CHECK(!gate_in_quiet_hours(&g, 13 * 60));
    CHECK(gate_in_quiet_hours(&g, 22 * 60));
    CHECK(gate_in_quiet_hours(&g, 3 * 60));
    REASON(rs(&g, NOON(1) + 600, "qr", 0, 5), "quiet_hours"); /* 12:10 */

    /* overlapping window that crosses midnight differently */
    rt.quiet_start_min = 6 * 60;
    rt.quiet_end_min = 9 * 60;
    CHECK_EQ(gate_set_runtime_limits(&g, &rt, NULL), GATE_OK);
    CHECK(gate_in_quiet_hours(&g, 8 * 60)); /* extends past the compiled end */
    CHECK(!gate_in_quiet_hours(&g, 9 * 60));
    CHECK(gate_in_quiet_hours(&g, 22 * 60));

    /* malformed windows are rejected and change nothing */
    rt.quiet_start_min = 600;
    rt.quiet_end_min = 600; /* start == end */
    CHECK_EQ(gate_set_runtime_limits(&g, &rt, NULL), GATE_INVALID);
    rt.quiet_start_min = 1440;
    rt.quiet_end_min = 10;
    CHECK_EQ(gate_set_runtime_limits(&g, &rt, NULL), GATE_INVALID);
    CHECK(gate_in_quiet_hours(&g, 8 * 60)); /* previous runtime window still active */

    /* a new set_schedule without quiet_hours drops the runtime window */
    gate_runtime_limits_clear(&rt);
    CHECK_EQ(gate_set_runtime_limits(&g, &rt, NULL), GATE_OK);
    CHECK(!gate_in_quiet_hours(&g, 8 * 60));
    CHECK(gate_in_quiet_hours(&g, 22 * 60)); /* compiled window never goes away */
}

static void test_min_interval(void)
{
    gate_t g;
    setup(&g);
    CHECK_EQ(rs(&g, NOON(1), "mi-1", 0, 5), GATE_OK);
    commit(&g, NOON(1), 5, 0, 0);
    REASON(rs(&g, NOON(1) + INTERVAL - 1, "mi-2", 0, 5), "min_interval");
    REASON(rnd(&g, NOON(1) + INTERVAL - 1, 0, 5), "min_interval");
    CHECK_EQ(rs(&g, NOON(1) + INTERVAL, "mi-3", 0, 5), GATE_OK); /* exactly at the interval */

    /* a random scatter also starts the interval for remote commands */
    commit(&g, NOON(1) + INTERVAL, 5, 0, 0);
    REASON(rs(&g, NOON(1) + INTERVAL + 10, "mi-4", 1, 5), "min_interval");

    /* a commit with 0 g (stall at once) still counts: the motors ran */
    commit(&g, NOON(1) + 2 * INTERVAL, 0, 0, 0);
    REASON(rnd(&g, NOON(1) + 2 * INTERVAL + 1, 0, 5), "min_interval");
}

static void test_min_interval_applies_to_cue_only_and_limits_cue_floods(void)
{
    gate_t g;
    gate_time_t t = at(NOON(1));
    gate_cmd_meta_t m1 = meta("c1", NOON(1)), m2 = meta("c2", NOON(1) + 10);
    setup(&g);
    CHECK_EQ(gate_check_cue(&g, &t, GATE_SRC_REMOTE, &m1), GATE_OK);
    gate_commit_cue(&g, &t);
    t = at(NOON(1) + 10);
    REASON(gate_check_cue(&g, &t, GATE_SRC_REMOTE, &m2), "min_interval");
    /* after a scatter, a cue-only command waits too */
    commit(&g, NOON(1) + 3 * INTERVAL, 5, 0, 0);
    t = at(NOON(1) + 3 * INTERVAL + 5);
    {
        gate_cmd_meta_t m3 = meta("c3", t.now);
        REASON(gate_check_cue(&g, &t, GATE_SRC_REMOTE, &m3), "min_interval");
    }
}

static void test_clock_moved_back_waits_a_full_interval(void)
{
    gate_t g;
    setup(&g);
    commit(&g, NOON(3), 5, 0, 0);
    /* clock is corrected 5 hours back: last scatter would be "in the future" */
    int64_t back = NOON(3) - 5 * 3600; /* 07:00 */
    REASON(rnd(&g, back, 0, 5), "min_interval");
    CHECK_EQ(rnd(&g, back + INTERVAL, 0, 5), GATE_OK);
}

static void test_dose_limit(void)
{
    gate_t g;
    gate_time_t t = at(NOON(1));
    gate_cmd_meta_t m = meta("dl", NOON(1));
    gate_dose_t d[2];
    setup(&g);
    CHECK_EQ(rs(&g, NOON(1), "dl-ok", 0, MAX0), GATE_OK);
    REASON(rs(&g, NOON(1), "dl-1", 0, MAX0 + 1), "dose_limit");
    REASON(rs(&g, NOON(1), "dl-2", 2, MAX2 + 1), "dose_limit"); /* per-hopper maximum */
    CHECK_EQ(rs(&g, NOON(1), "dl-3", 2, MAX2), GATE_OK);
    REASON(rs(&g, NOON(1), "dl-huge", 0, INT64_C(4000000000000)), "dose_limit");
    d[0].hopper = 0;
    d[0].grams = 5;
    d[1].hopper = 2;
    d[1].grams = MAX2 + 1; /* any dose over the limit rejects the whole scatter */
    REASON(gate_check_scatter(&g, &t, GATE_SRC_REMOTE, &m, d, 2), "dose_limit");
}

static void test_runtime_limits_cannot_raise_compiled_limits(void)
{
    gate_t g;
    gate_runtime_limits_t rt;
    int clamped = 0;
    setup(&g);

    gate_runtime_limits_clear(&rt);
    rt.max_dose_g[0] = 1000; /* try to raise */
    rt.max_dose_g[2] = 1000;
    rt.daily_budget_g[1] = 1000;
    rt.daily_total_g = 1000;
    rt.min_interval_s = 1; /* try to shorten */
    CHECK_EQ(gate_set_runtime_limits(&g, &rt, &clamped), GATE_OK);
    CHECK_EQ(clamped, 5);
    CHECK_EQ(gate_eff_max_dose(&g, 0), MAX0);
    CHECK_EQ(gate_eff_max_dose(&g, 2), MAX2);
    CHECK_EQ(gate_eff_daily_budget(&g, 1), BUD1);
    CHECK_EQ(gate_eff_daily_total(&g), TOTAL);
    CHECK_EQ(gate_eff_min_interval(&g), INTERVAL);
    REASON(rs(&g, NOON(1), "rl-1", 0, MAX0 + 1), "dose_limit");

    /* the interval cannot be shortened through the gate path either */
    CHECK_EQ(rs(&g, NOON(1), "rl-2", 0, 5), GATE_OK);
    commit(&g, NOON(1), 5, 0, 0);
    REASON(rs(&g, NOON(1) + 60, "rl-3", 0, 5), "min_interval");

    /* lowering works for every value */
    gate_runtime_limits_clear(&rt);
    rt.max_dose_g[0] = 4;
    rt.daily_budget_g[0] = 8;
    rt.daily_total_g = 12;
    rt.min_interval_s = 3600; /* longer interval = stricter */
    CHECK_EQ(gate_set_runtime_limits(&g, &rt, &clamped), GATE_OK);
    CHECK_EQ(clamped, 0);
    CHECK_EQ(gate_eff_max_dose(&g, 0), 4);
    CHECK_EQ(gate_eff_max_dose(&g, 1), MAX1); /* unset stays compiled */
    CHECK_EQ(gate_eff_daily_budget(&g, 0), 8);
    CHECK_EQ(gate_eff_daily_total(&g), 12);
    CHECK_EQ(gate_eff_min_interval(&g), 3600);
    REASON(rs(&g, NOON(2), "rl-4", 0, 5), "dose_limit");
    CHECK_EQ(rs(&g, NOON(2), "rl-5", 0, 4), GATE_OK);

    /* an override for a hopper that does not exist is malformed */
    {
        gate_limits_t l = limits();
        gate_t g2;
        l.hopper_count = 2;
        CHECK(gate_init(&g2, &l));
        gate_runtime_limits_clear(&rt);
        rt.max_dose_g[2] = 1;
        CHECK_EQ(gate_set_runtime_limits(&g2, &rt, NULL), GATE_INVALID);
    }
}

static void test_runtime_limit_zero_disables(void)
{
    gate_t g;
    gate_runtime_limits_t rt;
    setup(&g);
    gate_runtime_limits_clear(&rt);
    rt.daily_budget_g[1] = 0;
    CHECK_EQ(gate_set_runtime_limits(&g, &rt, NULL), GATE_OK);
    REASON(rs(&g, NOON(1), "z1", 1, 1), "daily_budget_exceeded");
    CHECK_EQ(rs(&g, NOON(1), "z2", 0, 1), GATE_OK);
}

static void test_budget_boundary(void)
{
    gate_t g;
    int64_t now = NOON(1);
    setup(&g);
    /* hopper 0: budget 30. Use 21 g in three scatters. */
    commit(&g, now, 10, 0, 0);
    now += INTERVAL;
    commit(&g, now, 10, 0, 0);
    now += INTERVAL;
    commit(&g, now, 1, 0, 0);
    now += INTERVAL;
    CHECK_EQ(used(&g, now, 0), 21);
    CHECK_EQ(rs(&g, now, "b-ok", 0, 9), GATE_OK);                      /* 21 + 9 = 30, exactly at budget */
    REASON(rs(&g, now, "b-over", 0, 10), "daily_budget_exceeded");     /* 31 */
    commit(&g, now, 9, 0, 0);
    now += INTERVAL;
    REASON(rs(&g, now, "b-one", 0, 1), "daily_budget_exceeded");       /* at budget: +1 rejected */
    CHECK_EQ(rs(&g, now, "b-other", 1, 1), GATE_OK);                   /* other hopper unaffected */
}

static void test_total_budget(void)
{
    gate_t g;
    int64_t now = NOON(1);
    gate_time_t t;
    gate_cmd_meta_t m;
    gate_dose_t d[3];
    setup(&g);
    commit(&g, now, 10, 10, 0); /* 20 */
    now += INTERVAL;
    commit(&g, now, 10, 10, 0); /* 40 */
    now += INTERVAL;
    commit(&g, now, 10, 0, 0); /* 50: total budget used up; hopper 1 and 2 have room */
    now += INTERVAL;
    CHECK_EQ(used_total(&g, now), 50);
    REASON(rs(&g, now, "t-1", 2, 1), "daily_budget_exceeded"); /* total, hopper 2 alone is fine */

    /* one multi-dose request that sums over the total: remove 10 g first */
    setup(&g);
    now = NOON(1);
    commit(&g, now, 10, 10, 5);  /* 25 */
    now += INTERVAL;
    commit(&g, now, 10, 10, 0);  /* 45 */
    now += INTERVAL;
    t = at(now);
    m = meta("t-multi", now);
    d[0].hopper = 0;
    d[0].grams = 2;
    d[1].hopper = 1;
    d[1].grams = 2;
    d[2].hopper = 2;
    d[2].grams = 2; /* 6 g more: 51 > 50, every hopper alone is fine */
    REASON(gate_check_scatter(&g, &t, GATE_SRC_REMOTE, &m, d, 3), "daily_budget_exceeded");
    m = meta("t-multi2", now);
    d[2].grams = 1; /* 5 g: exactly 50 */
    CHECK_EQ(gate_check_scatter(&g, &t, GATE_SRC_REMOTE, &m, d, 3), GATE_OK);
}

static void test_partial_dispense_counts_actual_grams(void)
{
    gate_t g;
    int64_t now = NOON(1);
    setup(&g);
    CHECK_EQ(rs(&g, now, "pd-1", 0, 10), GATE_OK);
    commit(&g, now, 3, 0, 0); /* stalled after 3 g of the 10 g requested */
    CHECK_EQ(used(&g, now, 0), 3);
    CHECK_EQ(used_total(&g, now), 3);
    /* budget 30: 27 g are left, not 20 */
    now += INTERVAL;
    commit(&g, now, 10, 0, 0); /* 13 */
    now += INTERVAL;
    commit(&g, now, 10, 0, 0); /* 23 */
    now += INTERVAL;
    CHECK_EQ(rs(&g, now, "pd-2", 0, 7), GATE_OK);                 /* 30, exactly */
    REASON(rs(&g, now, "pd-3", 0, 8), "daily_budget_exceeded");   /* 31 */
}

static void test_daily_reset_at_local_midnight(void)
{
    gate_t g;
    int64_t now = NOON(1);
    setup(&g);
    commit(&g, now, 10, 10, 5);
    CHECK_EQ(used(&g, DAY(1) + 20 * 3600, 0), 10); /* same day: still counted */
    CHECK_EQ(used_total(&g, DAY(1) + 20 * 3600), 25);
    CHECK_EQ(used(&g, DAY(1) + 86399, 0), 10);     /* last second of the day */
    CHECK_EQ(used(&g, DAY(2), 0), 0);              /* local midnight: reset */
    CHECK_EQ(used_total(&g, DAY(2)), 0);
    CHECK_EQ(used(&g, DAY(2), 2), 0);

    /* the check path also resets: day 3 starts with a full budget */
    setup(&g);
    commit(&g, NOON(1), 10, 0, 0);
    commit(&g, NOON(1) + INTERVAL, 10, 0, 0);
    commit(&g, NOON(1) + 2 * INTERVAL, 10, 0, 0); /* hopper 0 budget 30 used up */
    REASON(rs(&g, NOON(1) + 3 * INTERVAL, "dr-0", 0, 1), "daily_budget_exceeded");
    CHECK_EQ(rs(&g, NOON(2), "dr-1", 0, 10), GATE_OK);
    commit(&g, NOON(2), 10, 0, 0);
    CHECK_EQ(used(&g, NOON(2), 0), 10); /* the counter restarted from 0 */
}

static void test_daily_reset_uses_local_day_not_utc(void)
{
    gate_t g;
    gate_time_t t1, t2;
    uint32_t grams[GATE_MAX_HOPPERS] = {10, 0, 0};
    setup(&g);
    /* local midnight does not fall on a UTC midnight: the caller passes the local day */
    t1 = at(NOON(1));
    t1.local_day = 500;
    gate_commit_scatter(&g, &t1, grams);
    t2 = at(NOON(1) + 3600);
    t2.local_day = 500;
    CHECK_EQ(gate_used_today(&g, &t2, 0), 10);
    t2.local_day = 501; /* the local day changed while the UTC day did not */
    CHECK_EQ(gate_used_today(&g, &t2, 0), 0);
}

static void test_clock_back_keeps_counters(void)
{
    gate_t g;
    setup(&g);
    commit(&g, NOON(5), 10, 0, 0);
    CHECK_EQ(used(&g, NOON(5), 0), 10);
    CHECK_EQ(used(&g, NOON(4), 0), 10); /* local day went backwards: keep counters, fail closed */
    CHECK_EQ(used(&g, NOON(4) + 3600, 0), 10);
}

static void test_clock_step_back_then_forward_does_not_reset_twice(void)
{
    gate_t g;
    int64_t d100 = NOON(100), d99 = NOON(99), d101 = NOON(101);
    setup(&g);
    commit(&g, d100, 10, 10, 5);
    commit(&g, d100 + INTERVAL, 10, 10, 5);
    commit(&g, d100 + 2 * INTERVAL, 10, 10, 5); /* hopper 0 and 1 at 30 g: budget used up */
    REASON(rs(&g, d100 + 3 * INTERVAL, "cb-0", 0, 1), "daily_budget_exceeded");

    /* clock steps back one day: rejected (the clamped last scatter waits an interval,
     * the counters are kept either way) */
    CHECK(rs(&g, d99 + 3 * INTERVAL, "cb-1", 0, 1) != GATE_OK);
    REASON(rs(&g, d99 + 4 * INTERVAL, "cb-1b", 0, 1), "daily_budget_exceeded");
    CHECK_EQ(used(&g, d99, 0), 30);
    CHECK_EQ(gate_state(&g)->day, 100); /* st.day stays at the highest day seen */

    /* clock returns to day 100: must NOT reset */
    REASON(rs(&g, d100 + 6 * INTERVAL, "cb-2", 0, 1), "daily_budget_exceeded");
    CHECK_EQ(used(&g, d100, 0), 30);

    /* day 101: reset */
    CHECK_EQ(rs(&g, d101, "cb-3", 0, 10), GATE_OK);
    CHECK_EQ(used(&g, d101, 0), 0);
    CHECK_EQ(gate_state(&g)->day, 101);
}

static void test_reserve_survives_reset_before_settle(void)
{
    gate_t g, g2;
    gate_time_t t = at(NOON(1));
    gate_dose_t d[2] = {{0, 10}, {1, 7}};
    gate_state_t blob;
    setup(&g);
    CHECK_EQ(gate_check_scatter(&g, &t, GATE_SRC_RANDOM, NULL, d, 2), GATE_OK);
    CHECK(gate_reserve_scatter(&g, &t, d, 2));
    CHECK(gate_take_dirty(&g)); /* the app saves before the motors run */
    CHECK_EQ(used(&g, NOON(1), 0), 10);
    CHECK_EQ(used(&g, NOON(1), 1), 7);
    CHECK_EQ(used_total(&g, NOON(1)), 17);
    CHECK_EQ(gate_state(&g)->last_scatter_at, NOON(1));

    /* reset before the settle: restore from the persisted blob */
    memcpy(&blob, gate_state(&g), sizeof blob);
    setup(&g2);
    CHECK(gate_restore(&g2, &blob));
    CHECK_EQ(used(&g2, NOON(1), 0), 10);
    CHECK_EQ(used(&g2, NOON(1), 1), 7);
    CHECK_EQ(used_total(&g2, NOON(1)), 17);
    REASON(rnd(&g2, NOON(1) + INTERVAL - 1, 0, 1), "min_interval");
    CHECK_EQ(rnd(&g2, NOON(1) + INTERVAL, 0, 1), GATE_OK);
    /* a settle after the restore has no reservation to give back */
    {
        uint32_t zero[GATE_MAX_HOPPERS] = {0, 0, 0};
        gate_settle_scatter(&g2, &t, zero);
        CHECK_EQ(used(&g2, NOON(1), 0), 10);
        CHECK_EQ(used_total(&g2, NOON(1)), 17);
    }
    /* the budget counts the requested grams: hopper 0 has 20 g left */
    REASON(rs(&g2, NOON(1) + INTERVAL, "rv-1", 0, MAX0 + 1), "dose_limit");
    commit(&g2, NOON(1) + INTERVAL, 10, 0, 0);
    commit(&g2, NOON(1) + 2 * INTERVAL, 10, 0, 0);
    REASON(rs(&g2, NOON(1) + 3 * INTERVAL, "rv-2", 0, 1), "daily_budget_exceeded");
}

static void test_reserve_then_settle_lowers_to_actual(void)
{
    gate_t g;
    gate_time_t t = at(NOON(1));
    gate_dose_t d[2] = {{0, 10}, {1, 8}};
    uint32_t actual[GATE_MAX_HOPPERS] = {4, 0, 0};
    setup(&g);
    CHECK(gate_reserve_scatter(&g, &t, d, 2));
    (void)gate_take_dirty(&g);
    gate_settle_scatter(&g, &t, actual); /* hopper 0: 4 of 10 g; hopper 1: stalled at once */
    CHECK(gate_take_dirty(&g));
    CHECK_EQ(used(&g, NOON(1), 0), 4);
    CHECK_EQ(used(&g, NOON(1), 1), 0);
    CHECK_EQ(used_total(&g, NOON(1)), 4);
    CHECK_EQ(gate_state(&g)->last_scatter_at, NOON(1)); /* the motors ran: interval applies */
    REASON(rnd(&g, NOON(1) + 1, 0, 1), "min_interval");
}

static void test_settle_cannot_underflow_or_raise(void)
{
    gate_t g;
    gate_time_t t = at(NOON(1));
    gate_dose_t d = {0, 10};
    uint32_t big[GATE_MAX_HOPPERS] = {50, 50, 50};
    uint32_t zero[GATE_MAX_HOPPERS] = {0, 0, 0};
    setup(&g);

    /* actual above the request does not raise the counters */
    CHECK(gate_reserve_scatter(&g, &t, &d, 1));
    gate_settle_scatter(&g, &t, big);
    CHECK_EQ(used(&g, NOON(1), 0), 10);
    CHECK_EQ(used_total(&g, NOON(1)), 10);

    /* a second settle without a reservation changes nothing */
    gate_settle_scatter(&g, &t, zero);
    CHECK_EQ(used(&g, NOON(1), 0), 10);

    /* earlier usage stays: the refund is only the unused part of the reservation */
    CHECK(gate_reserve_scatter(&g, &t, &d, 1)); /* 20 g */
    gate_settle_scatter(&g, &t, zero);
    CHECK_EQ(used(&g, NOON(1), 0), 10);
    CHECK_EQ(used_total(&g, NOON(1)), 10);

    /* counters cannot go below 0, even if the state was lowered meanwhile */
    CHECK(gate_reserve_scatter(&g, &t, &d, 1));
    gate_state_t blob;
    memcpy(&blob, gate_state(&g), sizeof blob);
    blob.used_g[0] = 3;
    blob.used_total_g = 2;
    CHECK(gate_restore(&g, &blob));
    gate_settle_scatter(&g, &t, zero); /* no reservation after a restore */
    CHECK_EQ(used(&g, NOON(1), 0), 3);
    g.reserved = true; /* force a pending reservation over smaller counters */
    g.reserved_day = g.st.day;
    g.reserved_g[0] = 10;
    gate_settle_scatter(&g, &t, zero);
    CHECK_EQ(used(&g, NOON(1), 0), 0);
    CHECK_EQ(used_total(&g, NOON(1)), 0);

    /* an invalid reservation changes nothing */
    gate_time_t bad = t;
    bad.clock_valid = false;
    CHECK(!gate_reserve_scatter(&g, &bad, &d, 1));
    CHECK(!gate_reserve_scatter(&g, &t, NULL, 0));
    CHECK_EQ(used(&g, NOON(1), 0), 0);
}

static void test_settle_after_midnight_does_not_refund_new_day(void)
{
    gate_t g;
    gate_time_t t = at(DAY(1) + 86400 - 5);
    gate_dose_t d = {0, 10};
    uint32_t zero[GATE_MAX_HOPPERS] = {0, 0, 0};
    setup(&g);
    CHECK(gate_reserve_scatter(&g, &t, &d, 1));
    t = at(DAY(2) + 5);
    commit(&g, DAY(2) + 5, 6, 0, 0); /* day 2 usage */
    gate_settle_scatter(&g, &t, zero);
    CHECK_EQ(used(&g, DAY(2) + 5, 0), 6); /* the old day's reservation is not taken from day 2 */
}

static void test_hopper_empty(void)
{
    gate_t g;
    gate_time_t t = at(NOON(1));
    gate_cmd_meta_t m = meta("he-multi", NOON(1));
    gate_dose_t d[2];
    setup(&g);
    gate_set_level(&g, 1, GATE_LEVEL_EMPTY);
    CHECK_EQ(gate_level(&g, 1), GATE_LEVEL_EMPTY);
    REASON(rs(&g, NOON(1), "he-1", 1, 5), "hopper_empty");
    REASON(rnd(&g, NOON(1), 1, 5), "hopper_empty");
    CHECK_EQ(rs(&g, NOON(1), "he-2", 0, 5), GATE_OK);
    d[0].hopper = 0;
    d[0].grams = 1;
    d[1].hopper = 1;
    d[1].grams = 1;
    REASON(gate_check_scatter(&g, &t, GATE_SRC_REMOTE, &m, d, 2), "hopper_empty"); /* any empty hopper */
    /* low and unknown still scatter */
    gate_set_level(&g, 1, GATE_LEVEL_LOW);
    CHECK_EQ(rs(&g, NOON(1), "he-3", 1, 5), GATE_OK);
    gate_set_level(&g, 1, GATE_LEVEL_UNKNOWN);
    CHECK_EQ(rs(&g, NOON(1), "he-4", 1, 5), GATE_OK);
    /* refilled */
    gate_set_level(&g, 1, GATE_LEVEL_EMPTY);
    gate_set_level(&g, 1, GATE_LEVEL_OK);
    CHECK_EQ(rs(&g, NOON(1), "he-5", 1, 5), GATE_OK);
    /* a level of a hopper that does not exist is ignored */
    CHECK_EQ(gate_level(&g, 9), GATE_LEVEL_UNKNOWN);
    gate_set_level(&g, 9, GATE_LEVEL_EMPTY);
}

static void test_invalid(void)
{
    gate_t g;
    gate_time_t t = at(NOON(1));
    gate_cmd_meta_t m = meta("inv", NOON(1));
    gate_dose_t d[4];
    char longid[GATE_ID_MAX_LEN + 5];
    setup(&g);

    d[0].hopper = 0;
    d[0].grams = 5;
    d[1].hopper = 1;
    d[1].grams = 5;
    d[2].hopper = 2;
    d[2].grams = 5;
    d[3].hopper = 0;
    d[3].grams = 5;

    REASON(gate_check_scatter(&g, &t, GATE_SRC_REMOTE, NULL, d, 1), "invalid"); /* no meta */
    m.id = NULL;
    REASON(gate_check_scatter(&g, &t, GATE_SRC_REMOTE, &m, d, 1), "invalid");
    m.id = "";
    REASON(gate_check_scatter(&g, &t, GATE_SRC_REMOTE, &m, d, 1), "invalid");
    m.id = "has space";
    REASON(gate_check_command(&g, &t, &m), "invalid");
    memset(longid, 'a', sizeof longid - 1);
    longid[GATE_ID_MAX_LEN + 1] = '\0'; /* 37 characters: too long, never truncated */
    m.id = longid;
    REASON(gate_check_command(&g, &t, &m), "invalid");
    longid[GATE_ID_MAX_LEN] = '\0'; /* 36 characters: fine */
    CHECK_EQ(gate_check_command(&g, &t, &m), GATE_OK);
    CHECK(!gate_note_id(&g, ""));
    CHECK(gate_note_id(&g, "noted-id"));
    REASON(rs(&g, NOON(1), "noted-id", 0, 5), "duplicate");

    m = meta("inv-d", NOON(1));
    REASON(gate_check_scatter(&g, &t, GATE_SRC_REMOTE, &m, NULL, 0), "invalid");
    m = meta("inv-e", NOON(1));
    REASON(gate_check_scatter(&g, &t, GATE_SRC_REMOTE, &m, d, 0), "invalid");
    m = meta("inv-z", NOON(1));
    d[0].grams = 0;
    REASON(gate_check_scatter(&g, &t, GATE_SRC_REMOTE, &m, d, 1), "invalid");
    m = meta("inv-n", NOON(1));
    d[0].grams = -5;
    REASON(gate_check_scatter(&g, &t, GATE_SRC_REMOTE, &m, d, 1), "invalid");
    d[0].grams = 5;
    m = meta("inv-h", NOON(1));
    d[0].hopper = 3; /* no such hopper */
    REASON(gate_check_scatter(&g, &t, GATE_SRC_REMOTE, &m, d, 1), "invalid");
    d[0].hopper = 0;
    m = meta("inv-u", NOON(1));
    d[1].hopper = 0; /* duplicate hopper */
    REASON(gate_check_scatter(&g, &t, GATE_SRC_REMOTE, &m, d, 2), "invalid");
    d[1].hopper = 1;
    m = meta("inv-4", NOON(1));
    REASON(gate_check_scatter(&g, &t, GATE_SRC_REMOTE, &m, d, 4), "invalid"); /* 4 doses */
    m = meta("inv-ok", NOON(1));
    CHECK_EQ(gate_check_scatter(&g, &t, GATE_SRC_REMOTE, &m, d, 3), GATE_OK);

    /* random path: dose list is validated too */
    d[0].grams = 0;
    REASON(gate_check_scatter(&g, &t, GATE_SRC_RANDOM, NULL, d, 1), "invalid");
}

static void test_runtime_invalid_leaves_gate_unchanged(void)
{
    gate_t g;
    gate_runtime_limits_t rt;
    setup(&g);
    gate_runtime_limits_clear(&rt);
    rt.max_dose_g[0] = 3;
    rt.has_quiet = true;
    rt.quiet_start_min = 100;
    rt.quiet_end_min = 100;
    CHECK_EQ(gate_set_runtime_limits(&g, &rt, NULL), GATE_INVALID);
    CHECK_EQ(gate_eff_max_dose(&g, 0), MAX0); /* atomic: the dose limit was not applied */
}

static void test_check_order(void)
{
    gate_t g;
    gate_time_t t;
    gate_cmd_meta_t m;
    gate_dose_t d = {0, 5};
    int64_t quiet = DAY(1) + 22 * 3600;

    /* no_clock beats expired, duplicate, and everything else */
    setup(&g);
    t = at(quiet);
    t.clock_valid = false;
    m.id = "o-1";
    m.issued_at = 0;
    m.expires_at = 1;
    gate_set_paused(&g, true);
    gate_fault_raise(&g, GATE_FAULT_STALL);
    REASON(gate_check_scatter(&g, &t, GATE_SRC_REMOTE, &m, &d, 1), "no_clock");

    /* expired beats duplicate, invalid dose list, paused */
    t = at(quiet);
    d.grams = 0;
    m.id = "o-2";
    m.issued_at = quiet - 100;
    m.expires_at = quiet - 50;
    REASON(gate_check_scatter(&g, &t, GATE_SRC_REMOTE, &m, &d, 1), "expired");
    REASON(gate_check_scatter(&g, &t, GATE_SRC_REMOTE, &m, &d, 1), "expired"); /* still expired, not duplicate */

    /* duplicate beats invalid dose list and paused */
    m = meta("o-3", quiet);
    CHECK_EQ(gate_check_command(&g, &t, &m), GATE_OK);
    REASON(gate_check_scatter(&g, &t, GATE_SRC_REMOTE, &m, &d, 1), "duplicate");

    /* invalid dose list beats paused */
    m = meta("o-4", quiet);
    REASON(gate_check_scatter(&g, &t, GATE_SRC_REMOTE, &m, &d, 1), "invalid");
    d.grams = 5;

    /* paused > fault > quiet_hours (quiet hours is on at 22:00) */
    m = meta("o-5", quiet);
    REASON(gate_check_scatter(&g, &t, GATE_SRC_REMOTE, &m, &d, 1), "paused");
    gate_set_paused(&g, false);
    m = meta("o-6", quiet);
    REASON(gate_check_scatter(&g, &t, GATE_SRC_REMOTE, &m, &d, 1), "fault");
    gate_fault_clear_all(&g);
    m = meta("o-7", quiet);
    REASON(gate_check_scatter(&g, &t, GATE_SRC_REMOTE, &m, &d, 1), "quiet_hours");

    /* quiet_hours > min_interval > dose_limit > budget > hopper_empty */
    {
        gate_t g2;
        int64_t noon = NOON(2);
        gate_dose_t big[1] = {{0, 11}};
        setup(&g2);
        commit(&g2, noon - 100, 30, 0, 0); /* hopper 0 budget used up, interval running */
        gate_set_level(&g2, 0, GATE_LEVEL_EMPTY);
        t = at(noon);
        m = meta("o-8", noon);
        REASON(gate_check_scatter(&g2, &t, GATE_SRC_REMOTE, &m, big, 1), "min_interval");
        t = at(noon + INTERVAL);
        m = meta("o-9", t.now);
        REASON(gate_check_scatter(&g2, &t, GATE_SRC_REMOTE, &m, big, 1), "dose_limit");
        big[0].grams = 10;
        m = meta("o-10", t.now);
        REASON(gate_check_scatter(&g2, &t, GATE_SRC_REMOTE, &m, big, 1), "daily_budget_exceeded");
        gate_set_level(&g2, 1, GATE_LEVEL_EMPTY);
        big[0].hopper = 1; /* budget fine, hopper empty */
        m = meta("o-11", t.now);
        REASON(gate_check_scatter(&g2, &t, GATE_SRC_REMOTE, &m, big, 1), "hopper_empty");
    }
}

static void test_random_path_has_no_remote_steps(void)
{
    gate_t g;
    setup(&g);
    /* no id needed, nothing recorded in the replay ring */
    CHECK_EQ(rnd(&g, NOON(1), 0, 5), GATE_OK);
    CHECK_EQ(gate_state(&g)->seen_count, 0);
    CHECK_EQ(rnd(&g, NOON(1), 0, 5), GATE_OK); /* no min_interval without a commit */
}

static void test_gate_init_rejects_bad_limits(void)
{
    gate_t g;
    gate_limits_t l = limits();
    gate_dose_t d = {0, 1};
    gate_time_t t = at(NOON(1));
    l.hopper_count = 0;
    CHECK(!gate_init(&g, &l));
    REASON(gate_check_scatter(&g, &t, GATE_SRC_RANDOM, NULL, &d, 1), "fault"); /* unusable = no scatter */
    l.hopper_count = 4;
    CHECK(!gate_init(&g, &l));
    l = limits();
    l.quiet_start_min = 1440;
    CHECK(!gate_init(&g, &l));
    CHECK(!gate_init(&g, NULL));
    REASON(gate_check_cue(&g, &t, GATE_SRC_RANDOM, NULL), "fault");
    /* an all-zero gate (never initialised) is unusable too */
    memset(&g, 0, sizeof g);
    REASON(gate_check_scatter(&g, &t, GATE_SRC_RANDOM, NULL, &d, 1), "fault");
}

static void test_fewer_hoppers(void)
{
    gate_t g;
    gate_limits_t l = limits();
    l.hopper_count = 2;
    CHECK(gate_init(&g, &l));
    CHECK_EQ(rnd(&g, NOON(1), 1, 5), GATE_OK);
    REASON(rnd(&g, NOON(1), 2, 5), "invalid");
}

static void test_persistence_roundtrip(void)
{
    gate_t g, g2;
    gate_runtime_limits_t rt;
    gate_state_t blob;
    int64_t now = NOON(1);
    setup(&g);
    CHECK(gate_take_dirty(&g) == false);
    CHECK_EQ(rs(&g, now, "ps-1", 0, 10), GATE_OK);
    commit(&g, now, 10, 0, 0);
    gate_set_paused(&g, true);
    gate_fault_raise(&g, GATE_FAULT_STALL);
    gate_runtime_limits_clear(&rt);
    rt.max_dose_g[1] = 4;
    rt.has_quiet = true;
    rt.quiet_start_min = 14 * 60;
    rt.quiet_end_min = 15 * 60;
    CHECK_EQ(gate_set_runtime_limits(&g, &rt, NULL), GATE_OK);
    CHECK(gate_take_dirty(&g));
    CHECK(!gate_take_dirty(&g)); /* cleared by the read */

    memcpy(&blob, gate_state(&g), sizeof blob); /* what the app writes to NVS */

    setup(&g2); /* reboot */
    CHECK(gate_restore(&g2, &blob));
    CHECK(!gate_take_dirty(&g2));
    CHECK(gate_paused(&g2));
    CHECK_EQ(gate_faults(&g2), GATE_FAULT_STALL);
    CHECK_EQ(used(&g2, now, 0), 10);
    CHECK_EQ(gate_eff_max_dose(&g2, 1), 4);
    CHECK(gate_in_quiet_hours(&g2, 14 * 60 + 5));
    /* the replay ring survives the reboot */
    gate_set_paused(&g2, false);
    gate_fault_clear_all(&g2);
    REASON(rs(&g2, now + 60, "ps-1", 0, 5), "duplicate");
    /* and so does the interval: a reboot does not reset min_interval */
    REASON(rs(&g2, now + 60, "ps-2", 0, 5), "min_interval");

    /* bad blobs are refused and leave the gate untouched */
    {
        gate_state_t bad;
        memcpy(&bad, &blob, sizeof bad);
        bad.magic ^= 1;
        CHECK(!gate_restore(&g2, &bad));
        memcpy(&bad, &blob, sizeof bad);
        bad.version = 99;
        CHECK(!gate_restore(&g2, &bad));
        memcpy(&bad, &blob, sizeof bad);
        bad.seen_head = GATE_SEEN_IDS;
        CHECK(!gate_restore(&g2, &bad));
        memcpy(&bad, &blob, sizeof bad);
        bad.seen_count = GATE_SEEN_IDS + 1;
        CHECK(!gate_restore(&g2, &bad));
        memcpy(&bad, &blob, sizeof bad);
        bad.fault_mask = 0x80;
        CHECK(!gate_restore(&g2, &bad));
        memcpy(&bad, &blob, sizeof bad);
        bad.used_g[2] = 1;
        bad.runtime.has_quiet = true;
        bad.runtime.quiet_start_min = 5;
        bad.runtime.quiet_end_min = 5;
        CHECK(!gate_restore(&g2, &bad));
        CHECK(!gate_restore(&g2, NULL));
    }
    CHECK_EQ(gate_eff_max_dose(&g2, 1), 4);
}

static void test_restore_reclamps_runtime_limits_to_new_compiled_limits(void)
{
    gate_t g, g2;
    gate_runtime_limits_t rt;
    gate_limits_t l = limits();
    setup(&g);
    gate_runtime_limits_clear(&rt);
    rt.max_dose_g[0] = 8;
    CHECK_EQ(gate_set_runtime_limits(&g, &rt, NULL), GATE_OK);
    l.max_dose_g[0] = 6; /* new firmware lowers the compiled maximum */
    CHECK(gate_init(&g2, &l));
    CHECK(gate_restore(&g2, gate_state(&g)));
    CHECK_EQ(gate_eff_max_dose(&g2, 0), 6);
}

static void test_cue_checks_use_same_prefix_as_scatter(void)
{
    gate_t g;
    gate_time_t t = at(NOON(1));
    gate_cmd_meta_t m = meta("cq", NOON(1));
    setup(&g);
    CHECK_EQ(gate_check_cue(&g, &t, GATE_SRC_REMOTE, &m), GATE_OK);
    REASON(gate_check_cue(&g, &t, GATE_SRC_REMOTE, &m), "duplicate");
    m = meta("cq2", NOON(1));
    m.expires_at = NOON(1) - 1;
    REASON(gate_check_cue(&g, &t, GATE_SRC_REMOTE, &m), "expired");
    gate_fault_raise(&g, GATE_FAULT_WATCHDOG);
    m = meta("cq3", NOON(1));
    REASON(gate_check_cue(&g, &t, GATE_SRC_REMOTE, &m), "fault");
}

int main(void)
{
    RUN_TEST(test_reason_strings);
    RUN_TEST(test_accepts_a_normal_remote_and_random_scatter);
    RUN_TEST(test_no_clock_remote);
    RUN_TEST(test_no_clock_random);
    RUN_TEST(test_id_recorded_when_rejected_for_no_clock);
    RUN_TEST(test_expired);
    RUN_TEST(test_ttl_cap);
    RUN_TEST(test_duplicate_replay_of_accepted_id);
    RUN_TEST(test_duplicate_replay_of_rejected_id);
    RUN_TEST(test_duplicate_ring_keeps_last_32_ids);
    RUN_TEST(test_paused);
    RUN_TEST(test_fault);
    RUN_TEST(test_quiet_hours_cross_midnight);
    RUN_TEST(test_quiet_hours_same_day_window_and_disabled);
    RUN_TEST(test_quiet_hours_runtime_can_only_widen);
    RUN_TEST(test_min_interval);
    RUN_TEST(test_min_interval_applies_to_cue_only_and_limits_cue_floods);
    RUN_TEST(test_clock_moved_back_waits_a_full_interval);
    RUN_TEST(test_dose_limit);
    RUN_TEST(test_runtime_limits_cannot_raise_compiled_limits);
    RUN_TEST(test_runtime_limit_zero_disables);
    RUN_TEST(test_budget_boundary);
    RUN_TEST(test_total_budget);
    RUN_TEST(test_partial_dispense_counts_actual_grams);
    RUN_TEST(test_daily_reset_at_local_midnight);
    RUN_TEST(test_daily_reset_uses_local_day_not_utc);
    RUN_TEST(test_clock_back_keeps_counters);
    RUN_TEST(test_clock_step_back_then_forward_does_not_reset_twice);
    RUN_TEST(test_reserve_survives_reset_before_settle);
    RUN_TEST(test_reserve_then_settle_lowers_to_actual);
    RUN_TEST(test_settle_cannot_underflow_or_raise);
    RUN_TEST(test_settle_after_midnight_does_not_refund_new_day);
    RUN_TEST(test_hopper_empty);
    RUN_TEST(test_invalid);
    RUN_TEST(test_runtime_invalid_leaves_gate_unchanged);
    RUN_TEST(test_check_order);
    RUN_TEST(test_random_path_has_no_remote_steps);
    RUN_TEST(test_gate_init_rejects_bad_limits);
    RUN_TEST(test_fewer_hoppers);
    RUN_TEST(test_persistence_roundtrip);
    RUN_TEST(test_restore_reclamps_runtime_limits_to_new_compiled_limits);
    RUN_TEST(test_cue_checks_use_same_prefix_as_scatter);
    return mt_summary();
}
