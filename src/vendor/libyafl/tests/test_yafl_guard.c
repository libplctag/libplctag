/*
 * test_yafl_guard.c - Guard page overflow detection test
 *
 * Verifies that guard pages detect stack overflow.
 * Uses child process (fork on POSIX, CreateProcess on Windows) to detect crash.
 *
 * Copyright Kyle Hayes (2026)
 * Distributed under the Boost Software License, Version 1.0.
 */

#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../include/yafl.h"

#ifdef _WIN32
    #include <windows.h>
#else
    #include <sys/wait.h>
    #include <unistd.h>
#endif

/* Recursive function to overflow the stack */
static void overflow_stack(int depth) {
    volatile char buffer[128];

    /* Touch the buffer to prevent optimization */
    memset((void *)buffer, 0xAA, sizeof(buffer));

    fprintf(stderr, "[overflow] depth=%d, buffer=%p\n", depth, (void *)buffer);
    fflush(stderr);

    /* Recurse to build up stack usage to trigger guard page */
    if (depth < 250) {
        overflow_stack(depth + 1);
    }
}

static void *guard_test_fiber(void *data) {
    (void)data;

    fprintf(stderr, "[fiber] starting guard page test\n");
    fflush(stderr);

    /* This will hit the guard page and cause the process to crash */
    overflow_stack(0);

    fprintf(stderr, "[fiber] ERROR: should not reach here\n");
    fflush(stderr);
    return (void *)0x0;
}

/* Child process test - will crash when guard page is hit */
static int run_child_test(void) {
    fprintf(stderr, "[child] creating fiber with guard pages\n");
    fflush(stderr);

    yafl_fiber_t *fiber = yafl_fiber_create(guard_test_fiber, 24 * 1024,
                                            YAFL_STACK_FLAGS_VMEM);
    if (fiber == NULL) {
        fprintf(stderr, "[child] ERROR: fiber creation failed\n");
        fflush(stderr);
        return 1;
    }

    fprintf(stderr, "[child] resuming fiber\n");
    fflush(stderr);

    /* This will cause guard page fault and crash the process */
    void *result = yafl_fiber_resume(fiber, NULL);

    /* Should never reach here if guard page is working */
    fprintf(stderr, "[child] ERROR: fiber returned: %p (should have crashed)\n", result);
    fflush(stderr);
    yafl_fiber_destroy(fiber);
    return 1;
}

int main(int argc, char *argv[]) {
    fprintf(stderr, "=== yafl Guard Page Test ===\n");
    fflush(stderr);

    /* argc and argv only used on Windows */
#ifndef _WIN32
    (void)argc;
    (void)argv;
#endif

#ifdef _WIN32
    /* Check if we're the child process */
    if (argc > 1 && strcmp(argv[1], "__child__") == 0) {
        return run_child_test();
    }

    /* Parent: spawn child process */
    fprintf(stderr, "[main] spawning child process\n");
    fflush(stderr);

    STARTUPINFO si;
    PROCESS_INFORMATION pi;
    memset(&si, 0, sizeof(si));
    si.cb = sizeof(si);

    /* Build command line to run ourselves as child */
    char cmd_line[512];
    snprintf(cmd_line, sizeof(cmd_line), "%s __child__", argv[0]);

    if (!CreateProcessA(NULL, cmd_line, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi)) {
        fprintf(stderr, "[main] ERROR: CreateProcess failed (error: %lu)\n", GetLastError());
        fflush(stderr);
        return 1;
    }

    fprintf(stderr, "[main] waiting for child process (PID: %lu)\n", pi.dwProcessId);
    fflush(stderr);

    /* Wait for child to complete */
    WaitForSingleObject(pi.hProcess, INFINITE);

    DWORD exit_code;
    if (!GetExitCodeProcess(pi.hProcess, &exit_code)) {
        fprintf(stderr, "[main] ERROR: GetExitCodeProcess failed\n");
        fflush(stderr);
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
        return 1;
    }

    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);

    fprintf(stderr, "[main] child exited with code: 0x%lx\n", exit_code);
    fflush(stderr);

    /* On Windows, if guard page is working, child will crash with non-zero exit code
     * If child exited cleanly with code 0, it means the guard page didn't trigger */
    if (exit_code == 0) {
        fprintf(stderr, "[main] FAIL: child exited cleanly (guard page not triggered)\n");
        fflush(stderr);
        return 1;
    }

    fprintf(stderr, "[main] guard page successfully detected stack overflow\n");
    fflush(stderr);

#else
    /* POSIX: use fork */
    fprintf(stderr, "[main] spawning child process\n");
    fflush(stderr);

    pid_t pid = fork();
    if (pid == -1) {
        fprintf(stderr, "[main] ERROR: fork failed\n");
        fflush(stderr);
        return 1;
    }

    if (pid == 0) {
        /* Child process - run test and exit */
        exit(run_child_test());
    }

    /* Parent process - wait for child */
    fprintf(stderr, "[main] waiting for child process (PID: %d)\n", (int)pid);
    fflush(stderr);

    int status;
    pid_t result = waitpid(pid, &status, 0);
    if (result == -1) {
        fprintf(stderr, "[main] ERROR: waitpid failed\n");
        fflush(stderr);
        return 1;
    }

    /* Check how the child terminated */
    if (WIFSIGNALED(status)) {
        int sig = WTERMSIG(status);
        fprintf(stderr, "[main] child killed by signal %d\n", sig);
        fflush(stderr);

        /* Expect SIGSEGV (11) or SIGBUS (7) SIGILL (4) for guard page fault */
        if (sig == SIGSEGV || sig == SIGBUS || sig == SIGILL) {
            fprintf(stderr, "[main] guard page successfully detected stack overflow\n");
            fflush(stderr);
        } else {
            fprintf(stderr, "[main] FAIL: child killed by unexpected signal %d\n", sig);
            fflush(stderr);
            return 1;
        }
    } else if (WIFEXITED(status)) {
        int exit_code = WEXITSTATUS(status);
        fprintf(stderr, "[main] child exited with code %d\n", exit_code);
        fflush(stderr);

        if (exit_code == 0) {
            fprintf(stderr, "[main] FAIL: child exited cleanly (guard page not triggered)\n");
            fflush(stderr);
            return 1;
        }
    } else {
        fprintf(stderr, "[main] FAIL: unexpected child termination\n");
        fflush(stderr);
        return 1;
    }

#endif

    fprintf(stderr, "\nPASS: Guard page detected stack overflow\n");
    fflush(stderr);
    return 0;
}
