#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <pthread.h>
#include <stdint.h>
#include <sys/time.h>
#include "../lib/libplctag.h"

#define TAG_PATH "protocol=ab_eip&gateway=10.206.1.28&cpu=PLC5&elem_size=4&elem_count=1&name=F8:10"
#define ELEM_COUNT 1
#define ELEM_SIZE 4
#define DATA_TIMEOUT 5000



/*
 * This test program creates a lot of threads that read the same tag in
 * the plc.  They all hit the exact same underlying tag data structure.  
 * This tests, to some extent, whether the library can handle multi-threaded
 * access.
 * 
 * This test does not stop by itself.  You need to kill it externally!
 */


/* global to cheat on passing it to threads. */
plc_tag tag;



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
		case PLCTAG_STATUS_PENDING: return "PLCTAG_STATUS_PENDING"; break;
		case PLCTAG_STATUS_OK: return "PLCTAG_STATUS_OK"; break;
		case PLCTAG_ERR_NULL_PTR: return "PLCTAG_ERR_NULL_PTR"; break;
		case PLCTAG_ERR_OUT_OF_BOUNDS: return "PLCTAG_ERR_OUT_OF_BOUNDS"; break;
		case PLCTAG_ERR_NO_MEM: return "PLCTAG_ERR_NO_MEM"; break;
		case PLCTAG_ERR_LL_ADD: return "PLCTAG_ERR_LL_ADD"; break;
		case PLCTAG_ERR_BAD_PARAM: return "PLCTAG_ERR_BAD_PARAM"; break;
		case PLCTAG_ERR_CREATE: return "PLCTAG_ERR_CREATE"; break;
		case PLCTAG_ERR_NOT_EMPTY: return "PLCTAG_ERR_NOT_EMPTY"; break;
		case PLCTAG_ERR_OPEN: return "PLCTAG_ERR_OPEN"; break;
		case PLCTAG_ERR_SET: return "PLCTAG_ERR_SET"; break;
		case PLCTAG_ERR_WRITE: return "PLCTAG_ERR_WRITE"; break;
		case PLCTAG_ERR_TIMEOUT: return "PLCTAG_ERR_TIMEOUT"; break;
		case PLCTAG_ERR_TIMEOUT_ACK: return "PLCTAG_ERR_TIMEOUT_ACK"; break;
		case PLCTAG_ERR_RETRIES: return "PLCTAG_ERR_RETRIES"; break;
		case PLCTAG_ERR_READ: return "PLCTAG_ERR_READ"; break;
		case PLCTAG_ERR_BAD_DATA: return "PLCTAG_ERR_BAD_DATA"; break;
		case PLCTAG_ERR_ENCODE: return "PLCTAG_ERR_ENCODE"; break;
		case PLCTAG_ERR_DECODE: return "PLCTAG_ERR_DECODE"; break;
		case PLCTAG_ERR_UNSUPPORTED: return "PLCTAG_ERR_UNSUPPORTED"; break;
		case PLCTAG_ERR_TOO_LONG: return "PLCTAG_ERR_TOO_LONG"; break;
		case PLCTAG_ERR_CLOSE: return "PLCTAG_ERR_CLOSE"; break;
		case PLCTAG_ERR_NOT_ALLOWED: return "PLCTAG_ERR_NOT_ALLOWED"; break;
		case PLCTAG_ERR_THREAD: return "PLCTAG_ERR_THREAD"; break;
		case PLCTAG_ERR_NO_DATA: return "PLCTAG_ERR_NO_DATA"; break;
		case PLCTAG_ERR_THREAD_JOIN: return "PLCTAG_ERR_THREAD_JOIN"; break;
		case PLCTAG_ERR_THREAD_CREATE: return "PLCTAG_ERR_THREAD_CREATE"; break;
		case PLCTAG_ERR_MUTEX_DESTROY: return "PLCTAG_ERR_MUTEX_DESTROY"; break;
		case PLCTAG_ERR_MUTEX_UNLOCK: return "PLCTAG_ERR_MUTEX_UNLOCK"; break;
		case PLCTAG_ERR_MUTEX_INIT: return "PLCTAG_ERR_MUTEX_INIT"; break;
		case PLCTAG_ERR_MUTEX_LOCK: return "PLCTAG_ERR_MUTEX_LOCK"; break;
		case PLCTAG_ERR_NOT_IMPLEMENTED: return "PLCTAG_ERR_NOT_IMPLEMENTED"; break;
		case PLCTAG_ERR_BAD_DEVICE: return "PLCTAG_ERR_BAD_DEVICE"; break;
		case PLCTAG_ERR_BAD_GATEWAY: return "PLCTAG_ERR_BAD_GATEWAY"; break;
		case PLCTAG_ERR_REMOTE_ERR: return "PLCTAG_ERR_REMOTE_ERR"; break;
		case PLCTAG_ERR_NOT_FOUND: return "PLCTAG_ERR_NOT_FOUND"; break;
		case PLCTAG_ERR_ABORT: return "PLCTAG_ERR_ABORT"; break;

		default: return "Unknown error."; break;
	}
	
	return "Unknown error.";
}



/*
 * Thread function.  Just read until killed.
 */

void thread_func(void *data)
{
	int tid = (int)(intptr_t)data;
	int rc;
	float value;
	uint64_t start;
	uint64_t end;
	
	while(1) {
		/* capture the starting time */
		start = time_ms();

		/* use do/while to allow easy exit without return */
		do {
			rc = plc_tag_lock(tag);
			
			if(rc != PLCTAG_STATUS_OK) {
				value = 1000;
				break; /* punt, no lock */
			} 

			rc = plc_tag_read(tag, DATA_TIMEOUT);

			if(rc != PLCTAG_STATUS_OK) {
				value = 1001;
			} else {
				value =  plc_tag_get_float32(tag,0);
		
				/* increment the value */
				value = (value > 500.0 ? 0.0 : value + 1.5);
				
				/* yes, we should be checking this return value too... */
				plc_tag_set_float32(tag, 0, (int32_t)value);
				
				/* write the value */
				rc = plc_tag_write(tag, DATA_TIMEOUT);
			}
			
			/* yes, we should look at the return value */
			plc_tag_unlock(tag);
		} while(0);
				
		end = time_ms();
		
		fprintf(stderr,"Thread %d got result %f with return code %s in %dms\n",tid,value,decode_error(rc),(int)(end-start));
		
		/* no short sleeps, this is a PLC5 */
		sleep_ms(10);
	}
}


int main(int argc, char **argv)
{
	pthread_t thread;
	int num_threads;
	
	if(argc != 2) {
		fprintf(stderr,"ERROR: Must provide number of threads to run (between 1 and 300) argc=%d!\n",argc);
		return 0;
	}
	
	num_threads = (int)strtol(argv[1],NULL, 10);

	if(num_threads < 1 || num_threads > 300) {
		fprintf(stderr,"ERROR: %d (%s) is not a valid number. Must provide number of threads to run (between 1 and 300)!\n",num_threads, argv[1]);
		return 0;
	}
	
	/* create the tag */
    tag = plc_tag_create(TAG_PATH);

    /* everything OK? */
    if(!tag) {
        fprintf(stderr,"ERROR: Could not create tag!\n");

        return 0;
    }

    /* let the connect succeed we hope */
    while(plc_tag_status(tag) == PLCTAG_STATUS_PENDING) {
    	sleep(1);
    }

    if(plc_tag_status(tag) != PLCTAG_STATUS_OK) {
    	fprintf(stderr,"Error setting up tag internal state.\n");
    	return 0;
    }


	/* create the read threads */
	
	fprintf(stderr,"Creating %d threads.\n",num_threads);
	
	while(num_threads--) {
		pthread_create(&thread, NULL, (void *) &thread_func, (void *)(intptr_t)num_threads);
	}
	
	/* wait until ^C */
	while(1) {
		sleep(1);
	}
	
	return 0;
}

