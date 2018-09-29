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
    struct addrinfo hints, *res;
    int reuseaddr = 1; /* True */
    struct sockaddr_in listen_addr;

    /* Create the socket */
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

//    /* Get the address info */
//    memset(&hints, 0, sizeof hints);
//    hints.ai_family = AF_INET;
//    hints.ai_socktype = SOCK_STREAM;
//    if (getaddrinfo(NULL, PORT, &hints, &res) != 0) {
//        log("getaddrinfo() failed!\n");
//        return 1;
//    }
//
//    /* Bind to the address */
//    if (bind(*sock, res->ai_addr, res->ai_addrlen) == -1) {
//        log("bind() failed!\n");
//        return 0;
//    }
//
//    freeaddrinfo(res);
//
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

    if(!init_socket(&sock)) {
        log("init_socket() failed!\n");
        return 1;
    }

    init_tags();

    /* Main loop */
    while (1) {
        socklen_t size = sizeof(struct sockaddr_in);
        struct sockaddr_in their_addr;
        int newsock = accept(sock, (struct sockaddr*)&their_addr, &size);
        if (newsock == -1) {
            log("accept() failed!\n");
        } else {
            log("Got a connection from %s on port %d\n",
                inet_ntoa(their_addr.sin_addr), htons(their_addr.sin_port));
            /* Make a safe copy of newsock */
            int *safesock = malloc(sizeof(int));
            if (safesock) {
                *safesock = newsock;
                if (pthread_create(&thread, NULL, session_handler, safesock) != 0) {
                    log("Failed to create thread\n");
                    break;
                }

                pthread_detach(thread);
            } else {
                log("malloc() failed!");
                break;
            }
        }
    }

    close(sock);

    return 0;
}
