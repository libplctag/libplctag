#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <stdint.h>
#include <sys/time.h>
#include "../lib/libplctag.h"
#include "utils.h"

#define TAG_PATH "protocol=ab_eip&gateway=10.206.1.39&path=1,2,A:27:1&cpu=plc5&elem_count=1&elem_size=2&name=N7:0&debug=4"
#define ELEM_COUNT 1
#define ELEM_SIZE 2
#define DATA_TIMEOUT 1000

#define MAX_THREADS (10)

/*
 * This test program creates a lot of threads that read the same tag in
 * the plc.  They all hit the exact same underlying tag data structure.
 * This tests, to some extent, whether the library can handle multi-threaded
 * access.
 *
 * This test does not stop by itself.  You need to kill it externally!
 *
 * We use this as a stress test for thread mutex locking and memory leaks.
 */

volatile int done[MAX_THREADS] = {0,};



void mark_done(volatile int * volatile done)
{
    *done = 1;
}


/* as found here:
 * http://cboard.cprogramming.com/c-programming/145187-how-pick-random-number-between-x-y.html#post1083473
 */

/* use rejection method for less biased random */
int random_min_max(int min, int max)
{
    int range, result, cutoff;

    /* bogus inputs? */
    if (min >= max) {
        return min;
    }

    range = max-min+1;
    cutoff = (RAND_MAX / range) * range;

    /* Rejection method, to be statistically unbiased. */
    do {
        result = rand();
    } while (result >= cutoff);

    return result % range + min;
}




void *thread_func(void *data)
{
    int tid = (int)(intptr_t)data;
    int rc;
    int value;
    uint64_t start;
    uint64_t end;
    plc_tag tag;
    int iterations = 2000+random_min_max(0,2);

    while(iterations>0) {
        int loops = 10+random_min_max(0,20);

        tag = plc_tag_create(TAG_PATH, DATA_TIMEOUT);

        if(tag < 0) {
            fprintf(stderr,"Thread %d Error creating tag: %s\n", tid, plc_tag_decode_error(tag));
            iterations--;
            continue;
        }

        while(loops > 0 && iterations > 0) {
            /* capture the starting time */
            start = time_ms();

            rc = plc_tag_read(tag, DATA_TIMEOUT);

            if(rc != PLCTAG_STATUS_OK) {
                fprintf(stderr,"Thread %d Error reading tag! %s\n", tid, plc_tag_decode_error(rc));
                break;
            }

            value =  plc_tag_get_int16(tag,0);

            end = time_ms();

            fprintf(stderr,"Thread %d (iteration %d) got result %d with return code %s in %dms\n",tid,iterations, value,plc_tag_decode_error(rc),(int)(end-start));

            /* no short sleeps, this is a PLC5 */
            sleep_ms(50+random_min_max(0,50));

            iterations--;
            loops--;
        }

        plc_tag_destroy(tag);
        tag = PLC_TAG_NULL;

        sleep_ms(100+random_min_max(0,100));
    }

    fprintf(stderr,"Thread %d completed work.\n",tid);

    mark_done(&done[tid]);

    return NULL;
}


int main(int argc, char **argv)
{
    pthread_t thread[MAX_THREADS];
    int num_threads;
    int total_done = 0;
    int tid;

    if(argc != 2) {
        fprintf(stderr,"ERROR: Must provide number of threads to run (between 1 and 300) argc=%d!\n",argc);
        return 0;
    }

    num_threads = (int)strtol(argv[1],NULL, 10);

    if(num_threads < 1 || num_threads > MAX_THREADS) {
        fprintf(stderr,"ERROR: %d (%s) is not a valid number. Must provide number of threads to run (between 1 and 10)!\n",num_threads, argv[1]);
        return 0;
    }

    /* create the read threads */

    fprintf(stderr,"Creating %d threads.\n",num_threads);

    for(tid = 0; tid < num_threads; tid++) {
        pthread_create(&thread[tid], NULL, &thread_func, (void *)(intptr_t)tid);
    }

    while(total_done != num_threads) {
        sleep_ms(100);
        total_done = 0;

        for(tid=0; tid<num_threads; tid++) {
            total_done+=done[tid];
        }

        fprintf(stderr,"Done: %d\n", total_done);
    }

    /* let the background thread clean up anything it can */
    sleep_ms(2000);

    return 0;
}
