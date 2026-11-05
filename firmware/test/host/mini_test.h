/* Tiny assert harness for the host tests. No external dependencies.
 *
 * Each test_*.c file is its own executable:
 *
 *   static void test_x(void) { CHECK(1 == 1); CHECK_EQ(2, 1 + 1); CHECK_STR("a", "a"); }
 *   int main(void) { RUN_TEST(test_x); return mt_summary(); }
 */
#ifndef MINI_TEST_H
#define MINI_TEST_H

#include <stdio.h>
#include <string.h>

static int mt_checks = 0;
static int mt_failures = 0;
static int mt_tests = 0;
static int mt_test_failures = 0;

#define CHECK(cond)                                                                   \
    do {                                                                              \
        mt_checks++;                                                                  \
        if (!(cond)) {                                                                \
            mt_failures++;                                                            \
            mt_test_failures++;                                                       \
            printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                  \
        }                                                                             \
    } while (0)

#define CHECK_EQ(a, b)                                                                \
    do {                                                                              \
        long long mt_a = (long long)(a), mt_b = (long long)(b);                       \
        mt_checks++;                                                                  \
        if (mt_a != mt_b) {                                                           \
            mt_failures++;                                                            \
            mt_test_failures++;                                                       \
            printf("  FAIL %s:%d: %s == %s (got %lld, want %lld)\n", __FILE__,        \
                   __LINE__, #a, #b, mt_a, mt_b);                                     \
        }                                                                             \
    } while (0)

/* Compare gate results by their protocol reason string for readable failures. */
#define CHECK_STR(a, b)                                                               \
    do {                                                                              \
        const char *mt_sa = (a), *mt_sb = (b);                                        \
        mt_checks++;                                                                  \
        if (mt_sa == NULL || mt_sb == NULL ? mt_sa != mt_sb : strcmp(mt_sa, mt_sb)) { \
            mt_failures++;                                                            \
            mt_test_failures++;                                                       \
            printf("  FAIL %s:%d: %s == %s (got \"%s\", want \"%s\")\n", __FILE__,    \
                   __LINE__, #a, #b, mt_sa ? mt_sa : "(null)", mt_sb ? mt_sb : "(null)"); \
        }                                                                             \
    } while (0)

#define RUN_TEST(fn)                                                                  \
    do {                                                                              \
        mt_tests++;                                                                   \
        mt_test_failures = 0;                                                         \
        fn();                                                                         \
        printf("[%s] %s\n", mt_test_failures ? "FAIL" : " ok ", #fn);                 \
    } while (0)

static int mt_summary(void)
{
    printf("%d tests, %d checks, %d failed checks\n", mt_tests, mt_checks, mt_failures);
    return mt_failures ? 1 : 0;
}

#endif
