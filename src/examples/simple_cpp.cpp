#include <stdio.h>
#include "../lib/libplctag.h"
#include "utils.h"


#define TAG_PATH "protocol=ab_eip&gateway=192.168.1.42&path=1,0&cpu=LGX&elem_size=4&elem_count=10&name=myDINTArray"
#define ELEM_COUNT 10
#define ELEM_SIZE 4
#define DATA_TIMEOUT 5000

/*
 * There really isn't much to this example other than it being compiled
 * by g++ instead of gcc.  This is just to test that the API functions are
 * correctly exported for C++ code.
 */


int main(int argc, char **argv)
{
    plc_tag tag = PLC_TAG_NULL;
    int rc;
    int i;

    /* create the tag */
    tag = plc_tag_create(TAG_PATH);

    /* everything OK? */
    if(!tag) {
        fprintf(stderr,"ERROR: Could not create tag!\n");

        return 0;
    }

    /* let the connect succeed we hope */
    while(plc_tag_status(tag) == PLCTAG_STATUS_PENDING) {
        sleep_ms(100);
    }

    if(plc_tag_status(tag) != PLCTAG_STATUS_OK) {
        fprintf(stderr,"Error setting up tag internal state. Error %s\n", plc_tag_decode_error(plc_tag_status(tag)));
        return 0;
    }

    /* get the data */
    rc = plc_tag_read(tag, DATA_TIMEOUT);

    if(rc != PLCTAG_STATUS_OK) {
        fprintf(stderr,"ERROR: Unable to read the data! Got error code %d: %s\n",rc, plc_tag_decode_error(rc));
        return 0;
    }

    /* print out the data */
    for(i=0; i < ELEM_COUNT; i++) {
        fprintf(stderr,"data[%d]=%d\n",i,plc_tag_get_int32(tag,(i*ELEM_SIZE)));
    }

    /* now test a write */
    for(i=0; i < ELEM_COUNT; i++) {
        int32_t val = plc_tag_get_int32(tag,(i*ELEM_SIZE));

        val = val+1;

        fprintf(stderr,"Setting element %d to %d\n",i,val);

        plc_tag_set_int32(tag,(i*ELEM_SIZE),val);
    }

    rc = plc_tag_write(tag, DATA_TIMEOUT);

    if(rc != PLCTAG_STATUS_OK) {
        fprintf(stderr,"ERROR: Unable to read the data! Got error code %d: %s\n",rc, plc_tag_decode_error(rc));
        return 0;
    }


    /* get the data again*/
    rc = plc_tag_read(tag, DATA_TIMEOUT);

    if(rc != PLCTAG_STATUS_OK) {
        fprintf(stderr,"ERROR: Unable to read the data! Got error code %d: %s\n",rc, plc_tag_decode_error(rc));
        return 0;
    }

    /* print out the data */
    for(i=0; i < ELEM_COUNT; i++) {
        fprintf(stderr,"data[%d]=%d\n",i,plc_tag_get_int32(tag,(i*ELEM_SIZE)));
    }

    /* we are done */
    plc_tag_destroy(tag);

    return 0;
}


