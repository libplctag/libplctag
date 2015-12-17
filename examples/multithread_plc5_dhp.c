#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <pthread.h>
#include <stdint.h>
#include <sys/time.h>
#include "../lib/libplctag.h"

#define TAG_PATH "protocol=ab_eip&gateway=10.206.1.39&path=1,2,A:27:1&cpu=plc5&elem_count=1&elem_size=2&name=N7:0&debug=1"
#define ELEM_COUNT 1
#define ELEM_SIZE 2
#define DATA_TIMEOUT 5000



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

volatile int done[300] = {0,};

/*
 * time_ms
 *
 * Get current epoch time in ms.
 */

int64_t time_ms(void)
{
    struct timeval tv;

    gettimeofday(&tv,NULL);

    return  ((int64_t)tv.tv_sec*1000)+ ((int64_t)tv.tv_usec/1000);
}



int sleep_ms(int ms)
{
    struct timeval tv;

    tv.tv_sec = ms/1000;
    tv.tv_usec = (ms % 1000)*1000;

    return select(0,NULL,NULL,NULL, &tv);
}



const char* decode_error(int rc)
{
    switch(rc) {
        case PLCTAG_STATUS_PENDING:
            return "PLCTAG_STATUS_PENDING";
            break;

        case PLCTAG_STATUS_OK:
            return "PLCTAG_STATUS_OK";
            break;

        case PLCTAG_ERR_NULL_PTR:
            return "PLCTAG_ERR_NULL_PTR";
            break;

        case PLCTAG_ERR_OUT_OF_BOUNDS:
            return "PLCTAG_ERR_OUT_OF_BOUNDS";
            break;

        case PLCTAG_ERR_NO_MEM:
            return "PLCTAG_ERR_NO_MEM";
            break;

        case PLCTAG_ERR_LL_ADD:
            return "PLCTAG_ERR_LL_ADD";
            break;

        case PLCTAG_ERR_BAD_PARAM:
            return "PLCTAG_ERR_BAD_PARAM";
            break;

        case PLCTAG_ERR_CREATE:
            return "PLCTAG_ERR_CREATE";
            break;

        case PLCTAG_ERR_NOT_EMPTY:
            return "PLCTAG_ERR_NOT_EMPTY";
            break;

        case PLCTAG_ERR_OPEN:
            return "PLCTAG_ERR_OPEN";
            break;

        case PLCTAG_ERR_SET:
            return "PLCTAG_ERR_SET";
            break;

        case PLCTAG_ERR_WRITE:
            return "PLCTAG_ERR_WRITE";
            break;

        case PLCTAG_ERR_TIMEOUT:
            return "PLCTAG_ERR_TIMEOUT";
            break;

        case PLCTAG_ERR_TIMEOUT_ACK:
            return "PLCTAG_ERR_TIMEOUT_ACK";
            break;

        case PLCTAG_ERR_RETRIES:
            return "PLCTAG_ERR_RETRIES";
            break;

        case PLCTAG_ERR_READ:
            return "PLCTAG_ERR_READ";
            break;

        case PLCTAG_ERR_BAD_DATA:
            return "PLCTAG_ERR_BAD_DATA";
            break;

        case PLCTAG_ERR_ENCODE:
            return "PLCTAG_ERR_ENCODE";
            break;

        case PLCTAG_ERR_DECODE:
            return "PLCTAG_ERR_DECODE";
            break;

        case PLCTAG_ERR_UNSUPPORTED:
            return "PLCTAG_ERR_UNSUPPORTED";
            break;

        case PLCTAG_ERR_TOO_LONG:
            return "PLCTAG_ERR_TOO_LONG";
            break;

        case PLCTAG_ERR_CLOSE:
            return "PLCTAG_ERR_CLOSE";
            break;

        case PLCTAG_ERR_NOT_ALLOWED:
            return "PLCTAG_ERR_NOT_ALLOWED";
            break;

        case PLCTAG_ERR_THREAD:
            return "PLCTAG_ERR_THREAD";
            break;

        case PLCTAG_ERR_NO_DATA:
            return "PLCTAG_ERR_NO_DATA";
            break;

        case PLCTAG_ERR_THREAD_JOIN:
            return "PLCTAG_ERR_THREAD_JOIN";
            break;

        case PLCTAG_ERR_THREAD_CREATE:
            return "PLCTAG_ERR_THREAD_CREATE";
            break;

        case PLCTAG_ERR_MUTEX_DESTROY:
            return "PLCTAG_ERR_MUTEX_DESTROY";
            break;

        case PLCTAG_ERR_MUTEX_UNLOCK:
            return "PLCTAG_ERR_MUTEX_UNLOCK";
            break;

        case PLCTAG_ERR_MUTEX_INIT:
            return "PLCTAG_ERR_MUTEX_INIT";
            break;

        case PLCTAG_ERR_MUTEX_LOCK:
            return "PLCTAG_ERR_MUTEX_LOCK";
            break;

        case PLCTAG_ERR_NOT_IMPLEMENTED:
            return "PLCTAG_ERR_NOT_IMPLEMENTED";
            break;

        case PLCTAG_ERR_BAD_DEVICE:
            return "PLCTAG_ERR_BAD_DEVICE";
            break;

        case PLCTAG_ERR_BAD_GATEWAY:
            return "PLCTAG_ERR_BAD_GATEWAY";
            break;

        case PLCTAG_ERR_REMOTE_ERR:
            return "PLCTAG_ERR_REMOTE_ERR";
            break;

        case PLCTAG_ERR_NOT_FOUND:
            return "PLCTAG_ERR_NOT_FOUND";
            break;

        case PLCTAG_ERR_ABORT:
            return "PLCTAG_ERR_ABORT";
            break;

        case PLCTAG_ERR_WINSOCK:
            return "PLCTAG_ERR_WINSOCK";
            break;

        default:
            return "Unknown error.";
            break;
    }

    return "Unknown error.";
}



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




void thread_func(void *data)
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

        tag = plc_tag_create(TAG_PATH);

        /* let the connect succeed we hope */
        while(plc_tag_status(tag) == PLCTAG_STATUS_PENDING) {
            sleep_ms(100);
        }

        if(plc_tag_status(tag) != PLCTAG_STATUS_OK) {
            fprintf(stderr,"Thread %d Error creating tag: %s\n", tid, decode_error(plc_tag_status(tag)));
            plc_tag_destroy(tag);
            iterations--;
            continue;
        }

        while(loops > 0 && iterations > 0) {
            /* capture the starting time */
            start = time_ms();

            rc = plc_tag_read(tag, DATA_TIMEOUT);

            if(rc != PLCTAG_STATUS_OK) {
                fprintf(stderr,"Thread %d Error reading tag! %s\n", tid, decode_error(rc));
                break;
            }

            value =  plc_tag_get_int16(tag,0);

            end = time_ms();

            fprintf(stderr,"Thread %d (iteration %d) got result %d with return code %s in %dms\n",tid,iterations, value,decode_error(rc),(int)(end-start));

            /* no short sleeps, this is a PLC5 */
            sleep_ms(50+random_min_max(0,50));

            iterations--;
            loops--;
        }

        plc_tag_destroy(tag);
        tag = NULL;

        sleep_ms(100+random_min_max(0,100));
    }

    if(tag) {
        fprintf(stderr,"Thread %d done but tag still not destroyed.\n",tid);
        plc_tag_destroy(tag);
    }

    fprintf(stderr,"Thread %d completed work.\n",tid);

    mark_done(&done[tid]);

    return;
}


int main(int argc, char **argv)
{
    pthread_t thread;
    int num_threads;
    int total_done = 0;
    int tid;

    /*if(argc != 2) {
        fprintf(stderr,"ERROR: Must provide number of threads to run (between 1 and 300) argc=%d!\n",argc);
        return 0;
    }

    num_threads = (int)strtol(argv[1],NULL, 10);
     */

    num_threads = 5;

    if(num_threads < 1 || num_threads > 300) {
        fprintf(stderr,"ERROR: %d (%s) is not a valid number. Must provide number of threads to run (between 1 and 300)!\n",num_threads, argv[1]);
        return 0;
    }

    /* create the read threads */

    fprintf(stderr,"Creating %d threads.\n",num_threads);

    for(tid = 0; tid < num_threads; tid++) {
        pthread_create(&thread, NULL, (void *) &thread_func, (void *)(intptr_t)tid);
    }

    while(total_done != num_threads) {
        sleep(1);
        total_done = 0;

        for(tid=0; tid<num_threads; tid++) {
            total_done+=done[tid];
        }

        fprintf(stderr,"Done: %d\n", total_done);
    }

    /* let the background thread clean up anything it can */
    sleep(2);

    return 0;
}
