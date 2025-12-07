#include "coro_net.h"
#include "log.h"
#include <netinet/in.h>
#include <netinet/tcp.h>

#define MAX_TASKS 64

static Task tasks[MAX_TASKS];
static coro_pollfd pfds[MAX_TASKS];

static bool running = false;

// --- Platform Helpers ---

static void set_non_blocking(CSOCKET fd);
static void set_no_delay(CSOCKET fd);

void coro_init(void) {
    pdlog(LOG_MODULE_CORO_NET, LOG_LEVEL_INFO, "Initializing coroutine network library.");
#ifdef _WIN32
    WSADATA wsaData;
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
        pdlog(LOG_MODULE_CORO_NET, LOG_LEVEL_ERROR, "WSAStartup failed.");
        exit(EXIT_FAILURE);
    }
#endif
    for (int i = 0; i < MAX_TASKS; i++) {
        tasks[i].fd = (CSOCKET)-1;
    }

    pdlog(LOG_MODULE_CORO_NET, LOG_LEVEL_INFO, "Coroutine network library initialized.");
}



void coro_stop(void) {
    running = false;

    pdlog(LOG_MODULE_CORO_NET, LOG_LEVEL_INFO, "Stopping coroutine network library.");
    for (int i = 0; i < MAX_TASKS; i++) {
        if (tasks[i].fd != (CSOCKET)-1) {
            CS_CLOSE(tasks[i].fd);
            tasks[i].fd = (CSOCKET)-1;
        }
    }
    pdlog(LOG_MODULE_CORO_NET, LOG_LEVEL_INFO, "Coroutine network library stopped.");
}



void coro_add(CSOCKET fd, void (*handler)(Task*), void *context) {
    set_non_blocking(fd);

    for (int i = 0; i < MAX_TASKS; i++) {
        if (tasks[i].fd == (CSOCKET)-1) {
            tasks[i].fd = fd;
            tasks[i].line = 0;
            tasks[i].handler = handler;
            tasks[i].context = context;
            tasks[i].events = POLLIN; // Default start state
            return;
        }
    }

    fprintf(stderr, "Error: Task list full\n");
    CS_CLOSE(fd);
}

void coro_remove(CSOCKET fd) {
    for (int i = 0; i < MAX_TASKS; i++) {
        if (tasks[i].fd == fd) {
            CS_CLOSE(tasks[i].fd);
            tasks[i].fd = (CSOCKET)-1;
            return;
        }
    }
}

// Sets the application-managed buffers for the task
void coro_set_buffer(Task *t, buf_t *rx, buf_t *tx) {
    t->rx_buf = *rx;
    t->tx_buf = *tx;
}


void coro_run(void) {
    pdlog(LOG_MODULE_CORO_NET, LOG_LEVEL_INFO, "Starting coroutine network loop.");
    running = true;

    while (running) {
        int nfds = 0;
        int task_map[MAX_TASKS];

        // 1. Rebuild Poll List
        for (int i = 0; i < MAX_TASKS; i++) {
            if (tasks[i].fd != (CSOCKET)-1) {
                pfds[nfds].fd = tasks[i].fd;
                pfds[nfds].events = tasks[i].events;
                pfds[nfds].revents = 0;
                task_map[nfds] = i;
                nfds++;
            }
        }

        if (nfds == 0) break;

        // 2. Wait (Uses portable coro_poll alias)
        if (coro_poll(pfds, nfds, -1) < 0) {
#ifdef _WIN32
            // Check for interruption/error
            if (WSAGetLastError() != WSAEINTR) {
                fprintf(stderr, "Poll failed: %d\n", WSAGetLastError());
                break;
            }
#else
            if (errno == EINTR) continue;
            perror("Poll failed");
            break;
#endif
        }

        // 3. Dispatch
        for (int i = 0; i < nfds; i++) {
            if (pfds[i].revents) {
                int ti = task_map[i];
                tasks[ti].handler(&tasks[ti]);
            }
        }
    }
#ifdef _WIN32
    WSACleanup();
#endif
    pdlog(LOG_MODULE_CORO_NET, LOG_LEVEL_INFO, "Coroutine network loop exited.");
}


/* Helpers */


static void set_non_blocking(CSOCKET fd) {
#ifdef _WIN32
    u_long iMode = 1;
    ioctlsocket(fd, FIONBIO, &iMode);
#else
    fcntl(fd, F_SETFL, O_NONBLOCK);
#endif
}


static void set_no_delay(CSOCKET fd) {
    int flag = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, (char *)&flag, sizeof(int));
}

