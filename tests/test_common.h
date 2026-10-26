/*
 * RadioBridge tests — shared harness.
 *
 * Tiny assertion + sleep helpers. Every test binary prints failures
 * with file/line and exits non-zero on any failure, so CTest reports
 * it. Async tests poll with bounded waits — never sleep-and-pray
 * without a condition check.
 */
#ifndef RADIOBRIDGE_TEST_COMMON_H
#define RADIOBRIDGE_TEST_COMMON_H

#define _POSIX_C_SOURCE 200809L

#include "core/rb_thread.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "radiobridge/rb.h"
#include "radiobridge/rb_mock.h"

static int g_rb_test_failures = 0;

#define CHECK(cond) \
    do { \
        if (!(cond)) { \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            g_rb_test_failures++; \
        } \
    } while (0)

#define CHECK_ST(expr, expected) \
    do { \
        rb_status_t _s = (expr); \
        if (_s != (expected)) { \
            printf("FAIL %s:%d: %s -> %s, expected %s\n", __FILE__, \
                   __LINE__, #expr, rb_status_string(_s), \
                   rb_status_string(expected)); \
            g_rb_test_failures++; \
        } \
    } while (0)

#define TEST_END() \
    do { \
        if (g_rb_test_failures == 0) { \
            printf("ALL PASS\n"); \
        } else { \
            printf("%d FAILURES\n", g_rb_test_failures); \
        } \
        return g_rb_test_failures ? 1 : 0; \
    } while (0)

static inline void test_sleep_ms(unsigned int ms) {
    rb_sleep_ms(ms);
}

/* Wait up to timeout_ms for *flag to become non-zero. Returns 1 on
 * success, 0 on timeout. The flag is read without a lock: tests set it
 * from callbacks, which is fine for these small waits (flag is an int
 * written once per transition). */
static inline int test_wait_until(volatile int *flag, int timeout_ms) {
    int waited = 0;
    while (!*flag && waited < timeout_ms) {
        test_sleep_ms(10);
        waited += 10;
    }
    return *flag != 0;
}

/* Wait until *count reaches at least target (same polling style). */
static inline int test_wait_count(volatile int *count, int target,
                                  int timeout_ms) {
    int waited = 0;
    while (*count < target && waited < timeout_ms) {
        test_sleep_ms(10);
        waited += 10;
    }
    return *count >= target;
}

#endif /* RADIOBRIDGE_TEST_COMMON_H */
