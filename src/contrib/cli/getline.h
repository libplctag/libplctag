#ifdef _MSC_VER
    #define _CRT_SECURE_NO_WARNINGS 1
    #define restrict __restrict
 
    #include <stdio.h>
    #include <stdlib.h>
    #include <string.h>
    #include <stdint.h>
    #include <errno.h>

    size_t getline(char **restrict line, size_t *restrict len, FILE *restrict fp) {
        // Check if either line, len or fp are NULL pointers
        if(line == NULL || len == NULL || fp == NULL) {
            errno = EINVAL;
            return -1;
        }
        
        // Use a chunk array of 128 bytes as parameter for fgets
        char chunk[128];

        // Allocate a block of memory for *line if it is NULL or smaller than the chunk array
        if(*line == NULL || *len < sizeof(chunk)) {
            *len = sizeof(chunk);
            if((*line = malloc(*len)) == NULL) {
                errno = ENOMEM;
                return -1;
            }
        }

        // "Empty" the string
        (*line)[0] = '\0';

        while(fgets(chunk, sizeof(chunk), fp) != NULL) {
            // Resize the line buffer if necessary
            size_t len_used = strlen(*line);
            size_t chunk_used = strlen(chunk);

            if(*len - len_used < chunk_used) {
                // Check for overflow
                if(*len > SIZE_MAX / 2) {
                    errno = EOVERFLOW;
                    return -1;
                } else {
                    *len *= 2;
                }
                
                if((*line = realloc(*line, *len)) == NULL) {
                    errno = ENOMEM;
                    return -1;
                }
            }

            // Copy the chunk to the end of the line buffer
            memcpy(*line + len_used, chunk, chunk_used);
            len_used += chunk_used;
            (*line)[len_used] = '\0';

            // Check if *line contains '\n', if yes, return the *line length
            if((*line)[len_used - 1] == '\n') {
                return len_used;
            }
        }

        return -1;
    }
 
#endif
