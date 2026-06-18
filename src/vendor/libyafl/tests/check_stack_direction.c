/*
 * check_stack_direction.c
 * Checks if the stack grows downward (high address -> low address).
 *
 * Returns:
 *   0: Stack grows downward (Supported)
 *   1: Stack grows upward (Unsupported)
 *
 * The probe compares a frame address in a callee against one in the caller.
 * That is only meaningful if the two frames are distinct, so check() must NOT
 * be inlined: at -O1 and above the compiler otherwise folds it into main(),
 * collapsing both locals into one frame and producing a bogus "upward" result.
 * Force a separate frame with noinline and take the addresses through volatile
 * pointers so the optimizer cannot reason them away.
 */

#include <stdlib.h>

#if defined(_MSC_VER)
#    define NOINLINE __declspec(noinline)
#elif defined(__GNUC__) || defined(__clang__)
#    define NOINLINE __attribute__((noinline))
#else
#    define NOINLINE
#endif

NOINLINE static int stack_grows_up(char *parent) {
    char child;
    char *volatile child_addr = &child;
    return (void *)child_addr > (void *)parent;
}

int main(void) {
    char parent;
    char *volatile parent_addr = &parent;
    return stack_grows_up(parent_addr) ? 1 : 0;
}
