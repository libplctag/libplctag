#pragma once

/*
 * Minimal stand-in for the handful of CMocka macros this project's unit
 * tests actually use: will_return()/mock_type() value queuing,
 * assert_int_equal(), and a bare-bones test-array runner.
 *
 * This is not a general mocking framework. Notably, a failed assertion
 * aborts the whole test binary instead of failing just that one test and
 * continuing (real CMocka uses setjmp/longjmp per-test to allow that).
 * Fine for the current handful of independent tests; if that ever matters,
 * it's the first thing to add back.
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MINI_MOCK_MAX_QUEUED 64

typedef struct {
    const char *function;
    intmax_t value;
} mini_mock_value_t;

static mini_mock_value_t mini_mock_queue[MINI_MOCK_MAX_QUEUED];
static int mini_mock_queue_len = 0;

static inline void mini_mock_will_return(const char *function, intmax_t value) {
    if(mini_mock_queue_len >= MINI_MOCK_MAX_QUEUED) {
        fprintf(stderr, "mini_mock: queue full, increase MINI_MOCK_MAX_QUEUED\n");
        abort();
    }
    mini_mock_queue[mini_mock_queue_len].function = function;
    mini_mock_queue[mini_mock_queue_len].value = value;
    mini_mock_queue_len++;
}

/* Values are consumed in FIFO order per function name, matching CMocka. */
static inline intmax_t mini_mock_type(const char *function) {
    for(int i = 0; i < mini_mock_queue_len; i++) {
        if(strcmp(mini_mock_queue[i].function, function) == 0) {
            intmax_t value = mini_mock_queue[i].value;
            memmove(&mini_mock_queue[i], &mini_mock_queue[i + 1],
                    (size_t)(mini_mock_queue_len - i - 1) * sizeof(mini_mock_queue[0]));
            mini_mock_queue_len--;
            return value;
        }
    }
    fprintf(stderr, "mini_mock: no queued value for %s()\n", function);
    abort();
}

#define will_return(function, value) mini_mock_will_return(#function, (intmax_t)(value))
#define mock_type(type) ((type)mini_mock_type(__func__))

#define assert_int_equal(a, b)                                                                      \
    do {                                                                                             \
        intmax_t _mini_mock_a = (intmax_t)(a);                                                       \
        intmax_t _mini_mock_b = (intmax_t)(b);                                                       \
        if(_mini_mock_a != _mini_mock_b) {                                                           \
            fprintf(stderr, "%s:%d: assert_int_equal failed: %s (%jd) != %s (%jd)\n", __FILE__,      \
                    __LINE__, #a, _mini_mock_a, #b, _mini_mock_b);                                   \
            abort();                                                                                 \
        }                                                                                            \
    } while(0)

typedef void (*mini_mock_test_fn)(void **state);

struct CMUnitTest {
    const char *name;
    mini_mock_test_fn test_func;
};
typedef struct CMUnitTest CMUnitTest;

#define cmocka_unit_test(f) \
    { #f, (f) }

static inline int mini_mock_run_group_tests(const struct CMUnitTest tests[], size_t n,
                                             int (*group_setup)(void **),
                                             int (*group_teardown)(void **)) {
    if(group_setup) {
        group_setup(NULL);
    }

    for(size_t i = 0; i < n; i++) {
        printf("[ RUN      ] %s\n", tests[i].name);
        tests[i].test_func(NULL);
        printf("[       OK ] %s\n", tests[i].name);
        /* Any value a passing test left unconsumed must not leak into the next test. */
        mini_mock_queue_len = 0;
    }

    if(group_teardown) {
        group_teardown(NULL);
    }

    printf("[==========] %zu test(s) passed.\n", n);
    return 0;
}

#define cmocka_run_group_tests(tests, setup, teardown) \
    mini_mock_run_group_tests((tests), sizeof(tests) / sizeof((tests)[0]), (setup), (teardown))
