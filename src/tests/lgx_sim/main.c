/*
 *  Compile with -pthread
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h> /* memset() */
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <netdb.h>
#include <errno.h>
#include <pthread.h>
#include "log.h"
#include "session.h"
#include "tags.h"

#define PORT    44818 /* Port to listen on */
#define BACKLOG     10  /* Passed to listen() */

static int init_socket(int *sock);
void *client_handler(void *pnewsock);


int init_socket(int *sock)
{
    int reuseaddr = 1;
    struct sockaddr_in listen_addr;

    /* open a TCP socket using IPv4 protocol */
    *sock = socket (PF_INET, SOCK_STREAM, 0);
    if (*sock == -1) {
        log("socket() failed.\n");
        return 0;
    }

    /* Enable the socket to reuse the address */
    if (setsockopt(*sock, SOL_SOCKET, SO_REUSEADDR, &reuseaddr, sizeof(reuseaddr)) == -1) {
        log("setsockopt() failed!\n");
        return 0;
    }

    /* describe where we want to listen: on any network. */
    memset(&listen_addr, 0, sizeof(listen_addr));
    listen_addr.sin_family = AF_INET;
    listen_addr.sin_port = htons(PORT);
    listen_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(*sock, (struct sockaddr *) &listen_addr, sizeof (listen_addr)) < 0) {
        log("bind() failed!\n");
        log("errno = %d\n", errno);
        return 0;
    }

    /* Listen */
    if (listen(*sock, BACKLOG) == -1) {
        log("listen");
        return 0;
    }

    return 1;
}




int main(void)
{
    int sock;
    pthread_t thread;
    session_context *session = NULL;
    uint32_t session_handle = 1;

    if(!init_socket(&sock)) {
        log("init_socket() failed!\n");
        return 1;
    }

    init_tags();

    /* Main loop */
    while (1) {
        socklen_t size = sizeof(struct sockaddr_in);
        struct sockaddr_in client_addr;
        int newsock;

        /* get the client information. */
        memset(&client_addr, 0, sizeof client_addr);
        newsock = accept(sock, (struct sockaddr*)&client_addr, &size);


        if (newsock == -1) {
            log("accept() failed!\n");
        } else {
            log("Got a connection from %s on port %d\n", inet_ntoa(client_addr.sin_addr), htons(client_addr.sin_port));

            /*
             * set up a new session.  We allocate it here to get rid of possible
             * threading race conditions.
             */
            session = (session_context *)calloc(1, sizeof(session_context));
            if (session) {
                session->session_handle = session_handle++;
                session->connection_id = session_handle; /* note that post-increment
                                                          * will make this different
                                                          * from the session_handle
                                                          * value.
                                                          */
                session->sock = newsock;

                /* create a thread to handle the client. */
                if (pthread_create(&thread, NULL, session_handler, session) != 0) {
                    log("Failed to create thread\n");
                    break;
                }

                /* we want the thread to clean itself up. */
                pthread_detach(thread);
            } else {
                log("Unable to allocated new session!\n");
                break;
            }
        }
    }

    close(sock);

    return 0;
}
