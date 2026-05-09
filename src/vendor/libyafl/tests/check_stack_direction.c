/*
 * check_stack_direction.c
 * Checks if the stack grows downward (high address -> low address).
 *
 * Returns:
 *   0: Stack grows downward (Supported)
 *   1: Stack grows upward (Unsupported)
 */

#include <stdlib.h>

void check(char *parent) {
    char child;
    if ((void*)&child > (void*)parent) {
        exit(1); /* Stack grows upward */
    }
    exit(0); /* Stack grows downward */
}

int main() {
    char parent;
    check(&parent);
    return 0;
}