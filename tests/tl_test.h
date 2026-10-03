/*
 * Minimal self-contained test harness (no dependencies, C11).
 *
 *   static void test_something(void) { CHECK(x); CHECK_EQ(a, b); }
 *   int main(void) { RUN_TEST(test_something); return TEST_REPORT(); }
 *
 * A failed CHECK records the failure and continues, so one run shows every
 * broken expectation. Include this header from exactly one .c per binary.
 */
#ifndef TL_TEST_H
#define TL_TEST_H

#include <stdio.h>

static int tl_test_failures;
static int tl_test_checks;
static int tl_test_cases;
static int tl_test_failed_cases;

#define CHECK(cond)                                                                                \
    do {                                                                                           \
        tl_test_checks++;                                                                          \
        if (!(cond)) {                                                                             \
            tl_test_failures++;                                                                    \
            fprintf(stderr, "%s:%d: CHECK(%s) failed\n", __FILE__, __LINE__, #cond);               \
        }                                                                                          \
    } while (0)

#define CHECK_EQ(actual, expected)                                                                 \
    do {                                                                                           \
        const unsigned long long tl_a_ = (unsigned long long)(actual);                             \
        const unsigned long long tl_e_ = (unsigned long long)(expected);                           \
        tl_test_checks++;                                                                          \
        if (tl_a_ != tl_e_) {                                                                      \
            tl_test_failures++;                                                                    \
            fprintf(stderr,                                                                        \
                    "%s:%d: CHECK_EQ(%s, %s) failed: got %llu (0x%llx), want %llu (0x%llx)\n",     \
                    __FILE__, __LINE__, #actual, #expected, tl_a_, tl_a_, tl_e_, tl_e_);           \
        }                                                                                          \
    } while (0)

#define RUN_TEST(fn)                                                                               \
    do {                                                                                           \
        const int tl_before_ = tl_test_failures;                                                   \
        fn();                                                                                      \
        tl_test_cases++;                                                                           \
        if (tl_test_failures != tl_before_) {                                                      \
            tl_test_failed_cases++;                                                                \
        }                                                                                          \
        printf("[%s] %s\n", tl_test_failures == tl_before_ ? " OK " : "FAIL", #fn);                \
    } while (0)

#define TEST_REPORT()                                                                              \
    (printf("%d test cases, %d checks, %d failed checks, %d failed cases\n", tl_test_cases,        \
            tl_test_checks, tl_test_failures, tl_test_failed_cases),                               \
     tl_test_failures == 0 ? 0 : 1)

#endif /* TL_TEST_H */
