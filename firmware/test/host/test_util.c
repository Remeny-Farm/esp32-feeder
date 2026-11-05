/* Host tests for feeder_util: RFC 3339, HH:MM and the random day planner. */
#include <stdint.h>
#include <string.h>

#include "feeder_util.h"
#include "mini_test.h"

#define T_2026_11_05_10 INT64_C(1793872800) /* 2026-11-05T10:00:00Z */

static void test_rfc3339_parse_ok(void)
{
    int64_t e = -1;
    CHECK(rfc3339_parse("1970-01-01T00:00:00Z", &e));
    CHECK_EQ(e, 0);
    CHECK(rfc3339_parse("2026-11-05T10:00:00Z", &e));
    CHECK_EQ(e, T_2026_11_05_10);
    CHECK(rfc3339_parse("2026-11-01T00:00:00Z", &e));
    CHECK_EQ(e, 1793491200); /* GATE_MIN_VALID_EPOCH */
    /* Leap day in a leap year divisible by 400. */
    CHECK(rfc3339_parse("2000-02-29T12:00:00Z", &e));
    CHECK_EQ(e, 951825600);
    /* Same instant in other zones, with and without fraction. */
    CHECK(rfc3339_parse("2026-11-05T11:00:00+01:00", &e));
    CHECK_EQ(e, T_2026_11_05_10);
    CHECK(rfc3339_parse("2026-11-05T05:00:00-05:00", &e));
    CHECK_EQ(e, T_2026_11_05_10);
    CHECK(rfc3339_parse("2026-11-05T15:30:00+05:30", &e));
    CHECK_EQ(e, T_2026_11_05_10);
    CHECK(rfc3339_parse("2026-11-05T10:00:00.123456Z", &e));
    CHECK_EQ(e, T_2026_11_05_10);
    CHECK(rfc3339_parse("2026-11-05T10:00:00.5+00:00", &e));
    CHECK_EQ(e, T_2026_11_05_10);
    /* Before 1970. */
    CHECK(rfc3339_parse("1969-12-31T23:59:59Z", &e));
    CHECK_EQ(e, -1);
    /* Last second of the year and day roll-over. */
    CHECK(rfc3339_parse("2026-12-31T23:59:59Z", &e));
    CHECK_EQ(e, INT64_C(1793491200) + 61 * 86400 - 1);
}

static void test_rfc3339_parse_rejects(void)
{
    int64_t e = 777;
    CHECK(!rfc3339_parse(NULL, &e));
    CHECK(!rfc3339_parse("2026-11-05T10:00:00Z", NULL));
    CHECK(!rfc3339_parse("", &e));
    CHECK(!rfc3339_parse("2026-02-29T00:00:00Z", &e)); /* 2026 is not a leap year */
    CHECK(!rfc3339_parse("1900-02-29T00:00:00Z", &e)); /* divisible by 100, not 400 */
    CHECK(!rfc3339_parse("2026-13-01T00:00:00Z", &e)); /* month 13 */
    CHECK(!rfc3339_parse("2026-00-10T00:00:00Z", &e)); /* month 0 */
    CHECK(!rfc3339_parse("2026-04-31T00:00:00Z", &e)); /* April has 30 days */
    CHECK(!rfc3339_parse("2026-11-00T00:00:00Z", &e)); /* day 0 */
    CHECK(!rfc3339_parse("2026-11-05T24:00:00Z", &e));
    CHECK(!rfc3339_parse("2026-11-05T10:60:00Z", &e));
    CHECK(!rfc3339_parse("2026-11-05T10:00:60Z", &e));
    CHECK(!rfc3339_parse("2026-11-05T10:00:00", &e));        /* no zone */
    CHECK(!rfc3339_parse("2026-11-05T10:00:00+0100", &e));   /* offset needs a colon */
    CHECK(!rfc3339_parse("2026-11-05T10:00:00+24:00", &e));  /* offset hour out of range */
    CHECK(!rfc3339_parse("2026-11-05T10:00:00+01:60", &e));  /* offset minute out of range */
    CHECK(!rfc3339_parse("2026-11-05T10:00:00+1:00", &e));
    CHECK(!rfc3339_parse("2026-11-05T10:00:00.Z", &e));      /* empty fraction */
    CHECK(!rfc3339_parse("2026-11-05T10:00:00Z ", &e));      /* trailing space */
    CHECK(!rfc3339_parse("2026-11-05T10:00:00Zjunk", &e));   /* trailing text */
    CHECK(!rfc3339_parse(" 2026-11-05T10:00:00Z", &e));      /* leading space */
    CHECK(!rfc3339_parse("2026-11-05 10:00:00Z", &e));       /* space instead of T */
    CHECK(!rfc3339_parse("2026-11-05t10:00:00z", &e));       /* lower case */
    CHECK(!rfc3339_parse("2026-1-05T10:00:00Z", &e));        /* one-digit month */
    CHECK(!rfc3339_parse("2026-11-05T10:00", &e));           /* too short */
    CHECK(!rfc3339_parse("2026-11-05T10:00:0", &e));
    CHECK_EQ(e, 777); /* untouched on every failure */
}

static void test_rfc3339_format(void)
{
    char buf[RFC3339_UTC_BUF];
    CHECK_EQ(rfc3339_format(0, buf, sizeof buf), 20);
    CHECK_STR(buf, "1970-01-01T00:00:00Z");
    CHECK_EQ(rfc3339_format(T_2026_11_05_10, buf, sizeof buf), 20);
    CHECK_STR(buf, "2026-11-05T10:00:00Z");
    CHECK_EQ(rfc3339_format(-1, buf, sizeof buf), 20);
    CHECK_STR(buf, "1969-12-31T23:59:59Z");
    CHECK_EQ(rfc3339_format(951825600, buf, sizeof buf), 20);
    CHECK_STR(buf, "2000-02-29T12:00:00Z");
    CHECK_EQ(rfc3339_format(INT64_C(253402300799), buf, sizeof buf), 20);
    CHECK_STR(buf, "9999-12-31T23:59:59Z");
    CHECK_EQ(rfc3339_format(INT64_C(-62167219200), buf, sizeof buf), 20);
    CHECK_STR(buf, "0000-01-01T00:00:00Z");
    /* Out of range and too-small buffer write nothing useful and return 0. */
    CHECK_EQ(rfc3339_format(INT64_C(253402300800), buf, sizeof buf), 0);
    CHECK_EQ(rfc3339_format(INT64_C(-62167219201), buf, sizeof buf), 0);
    CHECK_EQ(rfc3339_format(0, buf, 20), 0);
    CHECK_EQ(rfc3339_format(0, NULL, 21), 0);
}

static void test_rfc3339_round_trip(void)
{
    const int64_t samples[] = {
        0, 1, -1, 86399, 86400, 951782400, 951868799, 1793491200, T_2026_11_05_10,
        INT64_C(4102444800), INT64_C(-86400), INT64_C(-2208988800), INT64_C(253402300799),
        INT64_C(-62167219200), INT64_C(1709164800) /* 2024-02-29T00:00:00Z */,
    };
    char buf[RFC3339_UTC_BUF];
    for (size_t i = 0; i < sizeof samples / sizeof samples[0]; i++) {
        int64_t back = 0;
        CHECK_EQ(rfc3339_format(samples[i], buf, sizeof buf), 20);
        CHECK(rfc3339_parse(buf, &back));
        CHECK_EQ(back, samples[i]);
    }
    /* Every day of a leap year and its neighbours. */
    for (int64_t t = INT64_C(1704067200); t < INT64_C(1704067200) + 800 * 86400; t += 86400 + 37) {
        int64_t back = 0;
        CHECK_EQ(rfc3339_format(t, buf, sizeof buf), 20);
        CHECK(rfc3339_parse(buf, &back));
        CHECK_EQ(back, t);
    }
}

static void test_days_from_civil(void)
{
    CHECK_EQ(days_from_civil(1970, 1, 1), 0);
    CHECK_EQ(days_from_civil(1969, 12, 31), -1);
    CHECK_EQ(days_from_civil(2026, 11, 1), 1793491200 / 86400);
    CHECK_EQ(days_from_civil(2000, 3, 1) - days_from_civil(2000, 2, 28), 2);
    CHECK_EQ(days_from_civil(1900, 3, 1) - days_from_civil(1900, 2, 28), 1);
}

static void test_hhmm(void)
{
    uint16_t m = 0xFFFF;
    CHECK(hhmm_parse("00:00", &m));
    CHECK_EQ(m, 0);
    CHECK(hhmm_parse("08:05", &m));
    CHECK_EQ(m, 485);
    CHECK(hhmm_parse("23:59", &m));
    CHECK_EQ(m, 1439);
    m = 1234;
    CHECK(!hhmm_parse("24:00", &m));
    CHECK(!hhmm_parse("12:60", &m));
    CHECK(!hhmm_parse("8:05", &m));
    CHECK(!hhmm_parse("08:5", &m));
    CHECK(!hhmm_parse("08-05", &m));
    CHECK(!hhmm_parse("08:05 ", &m));
    CHECK(!hhmm_parse("08:05:00", &m));
    CHECK(!hhmm_parse("", &m));
    CHECK(!hhmm_parse(NULL, &m));
    CHECK(!hhmm_parse("ab:cd", &m));
    CHECK_EQ(m, 1234);
}

/* ---- planner ---- */

static uint32_t lcg_next(void *ctx)
{
    uint32_t *s = ctx;
    *s = *s * 1664525u + 1013904223u;
    return *s >> 8; /* low bits of an LCG are weak */
}

/* Quiet from 21:00 to 07:00, like the compiled-in default. */
static bool night_quiet(void *ctx, uint16_t minute)
{
    (void)ctx;
    return minute >= 1260 || minute < 420;
}

static uint16_t cdist(uint16_t a, uint16_t b)
{
    uint16_t d = a > b ? a - b : b - a;
    return d <= 720 ? d : 1440 - d;
}

static bool in_window(const sched_window_t *w, uint16_t m)
{
    if (w->start_min < w->end_min) {
        return m >= w->start_min && m < w->end_min;
    }
    return m >= w->start_min || m < w->end_min;
}

static void check_plan(const sched_window_t *w, const uint16_t *out, size_t cnt, uint16_t gap,
                       bool (*quiet)(void *, uint16_t))
{
    for (size_t i = 0; i < cnt; i++) {
        CHECK(in_window(w, out[i]));
        if (quiet) {
            CHECK(!quiet(NULL, out[i]));
        }
        if (i > 0) {
            CHECK(out[i - 1] < out[i]); /* ascending and distinct */
        }
        for (size_t j = i + 1; j < cnt; j++) {
            CHECK(cdist(out[i], out[j]) >= (gap ? gap : 1));
        }
    }
}

static void test_plan_count_and_window(void)
{
    const sched_window_t w = {480, 1080}; /* 08:00-18:00 */
    for (uint32_t seed = 1; seed <= 100; seed++) {
        uint32_t s = seed;
        uint16_t out[SCHED_MAX_PER_DAY];
        size_t cnt = sched_plan_day(&w, 3, 60, night_quiet, NULL, lcg_next, &s, out);
        CHECK_EQ(cnt, 3);
        check_plan(&w, out, cnt, 60, night_quiet);
    }
}

static void test_plan_crosses_midnight(void)
{
    const sched_window_t w = {1320, 360}; /* 22:00-06:00 */
    for (uint32_t seed = 1; seed <= 100; seed++) {
        uint32_t s = seed;
        uint16_t out[SCHED_MAX_PER_DAY];
        size_t cnt = sched_plan_day(&w, 4, 30, NULL, NULL, lcg_next, &s, out);
        CHECK_EQ(cnt, 4);
        check_plan(&w, out, cnt, 30, NULL);
    }
}

static void test_plan_skips_quiet(void)
{
    const sched_window_t w = {0, 1439}; /* almost the whole day */
    for (uint32_t seed = 1; seed <= 100; seed++) {
        uint32_t s = seed;
        uint16_t out[SCHED_MAX_PER_DAY];
        size_t cnt = sched_plan_day(&w, 10, 30, night_quiet, NULL, lcg_next, &s, out);
        CHECK(cnt > 0);
        check_plan(&w, out, cnt, 30, night_quiet);
    }
    /* A window that lies fully inside quiet hours plans nothing. */
    const sched_window_t q = {1300, 1400};
    uint32_t s = 5;
    uint16_t out[SCHED_MAX_PER_DAY];
    CHECK_EQ(sched_plan_day(&q, 3, 10, night_quiet, NULL, lcg_next, &s, out), 0);
}

static void test_plan_min_gap(void)
{
    const sched_window_t w = {480, 1080};
    for (uint32_t seed = 1; seed <= 100; seed++) {
        uint32_t s = seed;
        uint16_t out[SCHED_MAX_PER_DAY];
        size_t cnt = sched_plan_day(&w, 8, 70, NULL, NULL, lcg_next, &s, out);
        CHECK(cnt >= 1 && cnt <= 8);
        check_plan(&w, out, cnt, 70, NULL);
    }
}

static void test_plan_too_many_places_fewer(void)
{
    const sched_window_t w = {600, 610}; /* ten minutes */
    for (uint32_t seed = 1; seed <= 100; seed++) {
        uint32_t s = seed;
        uint16_t out[SCHED_MAX_PER_DAY];
        size_t cnt = sched_plan_day(&w, 5, 5, NULL, NULL, lcg_next, &s, out);
        CHECK(cnt >= 1 && cnt <= 2); /* 600 and 605 at best; never 3 */
        check_plan(&w, out, cnt, 5, NULL);
    }
    /* gap 0 means "distinct": a 5-minute window holds at most 5. */
    const sched_window_t v = {600, 605};
    uint32_t s = 9;
    uint16_t out[SCHED_MAX_PER_DAY];
    size_t cnt = sched_plan_day(&v, 10, 0, NULL, NULL, lcg_next, &s, out);
    CHECK(cnt >= 1 && cnt <= 5);
    check_plan(&v, out, cnt, 1, NULL);
    /* n above SCHED_MAX_PER_DAY is clamped. */
    const sched_window_t full = {0, 1439};
    cnt = sched_plan_day(&full, 255, 1, NULL, NULL, lcg_next, &s, out);
    CHECK(cnt <= SCHED_MAX_PER_DAY);
}

static void test_plan_edge_cases(void)
{
    uint32_t s = 3;
    uint16_t out[SCHED_MAX_PER_DAY];
    const sched_window_t w = {480, 1080};
    const sched_window_t same = {480, 480};
    const sched_window_t bad = {1440, 100};
    CHECK_EQ(sched_plan_day(&w, 0, 30, NULL, NULL, lcg_next, &s, out), 0);   /* n == 0 */
    CHECK_EQ(sched_plan_day(&same, 3, 30, NULL, NULL, lcg_next, &s, out), 0); /* start == end */
    CHECK_EQ(sched_plan_day(&bad, 3, 30, NULL, NULL, lcg_next, &s, out), 0);  /* out of range */
    CHECK_EQ(sched_plan_day(NULL, 3, 30, NULL, NULL, lcg_next, &s, out), 0);
    CHECK_EQ(sched_plan_day(&w, 3, 30, NULL, NULL, NULL, &s, out), 0);
    CHECK_EQ(sched_plan_day(&w, 3, 30, NULL, NULL, lcg_next, &s, NULL), 0);
}

static void test_next_after(void)
{
    const uint16_t t[] = {480, 600, 900};
    uint16_t next = 0;
    CHECK(sched_next_after(t, 3, 0, &next));
    CHECK_EQ(next, 480);
    CHECK(sched_next_after(t, 3, 480, &next)); /* strictly after */
    CHECK_EQ(next, 600);
    CHECK(sched_next_after(t, 3, 899, &next));
    CHECK_EQ(next, 900);
    CHECK(!sched_next_after(t, 3, 900, &next));
    CHECK(!sched_next_after(t, 0, 0, &next));
    CHECK(!sched_next_after(NULL, 3, 0, &next));
}

int main(void)
{
    RUN_TEST(test_rfc3339_parse_ok);
    RUN_TEST(test_rfc3339_parse_rejects);
    RUN_TEST(test_rfc3339_format);
    RUN_TEST(test_rfc3339_round_trip);
    RUN_TEST(test_days_from_civil);
    RUN_TEST(test_hhmm);
    RUN_TEST(test_plan_count_and_window);
    RUN_TEST(test_plan_crosses_midnight);
    RUN_TEST(test_plan_skips_quiet);
    RUN_TEST(test_plan_min_gap);
    RUN_TEST(test_plan_too_many_places_fewer);
    RUN_TEST(test_plan_edge_cases);
    RUN_TEST(test_next_after);
    return mt_summary();
}
