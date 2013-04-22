#include <stdio.h>
#include <unistd.h>
#include "../lib/libplctag.h"


#define TAG_PATH "protocol=ab_eip&gateway=10.206.1.27&path=1,0&cpu=LGX&elem_size=4&elem_count=1&name=pcomm_test_dint_array[%d]"
#define NUM_TAGS 150
#define DATA_TIMEOUT 5000

int main(int argc, char **argv)
{
    plc_tag tag[NUM_TAGS];
    int rc;
    int i;

    /* create the tags */
    for(i=0; i< NUM_TAGS; i++) {
    	char tmp_tag_path[256] = {0,};
    	snprintf(tmp_tag_path, sizeof tmp_tag_path,TAG_PATH,i);
    	tag[i]  = plc_tag_create(tmp_tag_path);

    	if(!tag[i]) {
    		fprintf(stderr,"Error: could not create tag %d\n",i);
    	}
    }

    /* let the connect complete */
    fprintf(stderr,"Sleeping to let the connect complete.\n");
    sleep(1);
    for(i=0; i < NUM_TAGS; i++) {
    	plc_tag_status(tag[i]);
    }

    /* get the data */
    for(i=0; i < NUM_TAGS; i++) {
    	rc = plc_tag_read(tag[i], 0);

		if(rc != PLCTAG_STATUS_OK && rc != PLCTAG_STATUS_PENDING) {
			fprintf(stderr,"ERROR: Unable to read the data! Got error code %d\n",rc);

			return 0;
		}
    }

    /* sleeping to let the reads complete */
    fprintf(stderr,"Sleeping to let the reads complete.\n");
    sleep(2);


    /* get any data we can */
	for(i=0; i < NUM_TAGS; i++) {
		rc = plc_tag_status(tag[i]);
		if(rc == PLCTAG_STATUS_PENDING) {
			fprintf(stderr,"Tag %d is still pending.\n",i);
		} else if(rc != PLCTAG_STATUS_OK) {
			fprintf(stderr,"Tag %d status error! %d\n",i,rc);
		} else {
			/* read complete! */
			fprintf(stderr,"Tag %d data[0]=%d\n",i,plc_tag_get_int32(tag[i],0));
		}
	}


   /*
    for(i=0; i < DATA_SIZE; i++) {
    	int32_t val = plc_tag_get_int32(tag,(i*4));

    	if(val == 0) {
    		val = i+1;
    	} else {
    		val++;
    	}

    	val = i+1;

    	fprintf(stderr,"Setting element %d to %d\n",i,val);

    	plc_tag_set_int32(tag,(i*4),val);
    }

    rc = plc_tag_write(tag, DATA_TIMEOUT);

    if(rc != PLCTAG_STATUS_OK) {
        fprintf(stderr,"ERROR: Unable to read the data! Got error code %d\n",rc);

        return 0;
    }


    rc = plc_tag_read(tag, DATA_TIMEOUT);

    if(rc != PLCTAG_STATUS_OK) {
        fprintf(stderr,"ERROR: Unable to read the data! Got error code %d\n",rc);

        return 0;
    }

    for(i=0; i < DATA_SIZE; i++) {
        fprintf(stderr,"data[%d]=%d\n",i,plc_tag_get_int32(tag,(i*4)));
    }
*/
    /* we are done */
    for(i=0; i < NUM_TAGS; i++) {
    	plc_tag_destroy(tag[i]);
    }

    return 0;
}


