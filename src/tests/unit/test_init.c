#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <setjmp.h>
#include "cmocka.h"

// Your library headers
#include <libplctag/lib/init.h>
#include <libplctag/lib/libplctag.h>
#include <utils/rc.h>

// CMocka requires we redefine the wrapped functions. These match the real ones exactly.
// When compiled with -Wl,--wrap=func_name, calls to func_name() get redirected to __wrap_func_name().

int __wrap_refcount_startup(void) {
    return mock_type(int);  // Return whatever value we dictate in the test
}

int __wrap_lib_init(void) { return mock_type(int); }

int __wrap_ab_init(void) { return mock_type(int); }

int __wrap_mb_init(void) { return mock_type(int); }

int __wrap_omron_init(void) { return mock_type(int); }

/// Reset any global library state that gets polluted between tests
/// (e.g., library_state in init.c)
extern int32_t atomic_set_int32(atomic_int32_t *, int32_t);
extern void destroy_modules(void);

// A simple test ensuring that if lib_init fails, the initialization bails out with an error
static void test_initialization_fails_if_lib_init_fails(void **state) {
    (void)state;  // Unused

    // Arrange: Mock returns
    will_return(__wrap_refcount_startup, PLCTAG_STATUS_OK);  // Succeeds
    will_return(__wrap_lib_init, PLCTAG_ERR_BAD_STATUS);     // Fails!

    // Act
    int rc = initialize_modules();

    // Assert
    assert_int_equal(rc, PLCTAG_ERR_BAD_STATUS);
}

// Test everything succeeds
static void test_initialization_success(void **state) {
    (void)state;  // Unused

    // Arrange sequence of valid module startups
    will_return(__wrap_refcount_startup, PLCTAG_STATUS_OK);
    will_return(__wrap_lib_init, PLCTAG_STATUS_OK);
    will_return(__wrap_ab_init, PLCTAG_STATUS_OK);
    will_return(__wrap_mb_init, PLCTAG_STATUS_OK);
    will_return(__wrap_omron_init, PLCTAG_STATUS_OK);

    // Act
    int rc = initialize_modules();

    // Assert
    assert_int_equal(rc, PLCTAG_STATUS_OK);

    // Clean up
    destroy_modules();
}

int main(void) {
    const struct CMUnitTest tests[] = {
        cmocka_unit_test(test_initialization_fails_if_lib_init_fails),
        cmocka_unit_test(test_initialization_success),
    };
    return cmocka_run_group_tests(tests, NULL, NULL);
}