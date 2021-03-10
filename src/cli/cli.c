#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../lib/libplctag.h"
#include "./cli.h"

void parse_args(int argc, char *argv[], request_t *request)
{
    if (argc != 3) {
        printf("ERROR: invalid number of arguments!\n");
        return;
    }

    char *operation = argv[1];
    char *args = argv[2];

    if (!strcmp(operation, "--read")) {
        printf("cli operation: READ.\n");
        request->operation = CLI_READ_OPERATION;
    } else if (!strcmp(operation, "--watch")) {
        printf("cli operation: WATCH.\n");
        request->operation = CLI_WATCH_OPERATION;
    } else if (!strcmp(operation, "--write")) {
        printf("cli operation: WRITE.\n");
        request->operation = CLI_WRITE_OPERATION;
    } else {
        printf("cli operation: INVALID.\n");
        return;
    }

    char *args_l = (char *) malloc(strlen(args));
    strcpy(args_l, args);
    
    printf("cli %s: ", strtok(args_l, "="));
    request->ip = strtok(NULL, "=");
    printf("%s\n", request->ip);
    
    free(args_l);
    return;    
}

int main(int argc, char *argv[])
{
    request_t request = Request;
    parse_args(argc, argv, &request);

    if (request.operation == CLI_INVALID_OPERATION) {
        printf("ERROR: operation invalid!\n");
        exit(-1);
    }

    if (request.ip == NULL) {
        printf("ERROR: ip invalid!\n");
        exit(-1);
    }

    switch (request.operation) {
        case CLI_READ_OPERATION:
            printf("Read here!\n");
            break;
        case CLI_WATCH_OPERATION:
            printf("Watch here!\n");
            break;
        case CLI_WRITE_OPERATION:
            printf("Write here!\n");
            break;
        default:
            break;
    }

    return 0;
}
