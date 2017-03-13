#include <stdio.h>
#include "../lib/libplctag.h"
#include "utils.h"


#define TAG_PATH "protocol=ab_eip&gateway=10.206.1.28&cpu=PLC5&elem_size=4&elem_count=5&name=F8:10&debug=1"
#define ELEM_COUNT 5
#define ELEM_SIZE 4
#define DATA_TIMEOUT 1000


int main()
{
    plc_tag tag = PLC_TAG_NULL;
    int rc;
    int i;

    /* create the tag */
    tag = plc_tag_create(TAG_PATH, DATA_TIMEOUT);

    /* everything OK? */
    if(tag < 0) {
        fprintf(stderr,"ERROR: Could not create tag! error %s\n", plc_tag_decode_error(tag));
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
        fprintf(stderr,"data[%d]=%f\n",i,plc_tag_get_float32(tag,(i*ELEM_SIZE)));
    }

    /* now test a write */
    for(i=0; i < ELEM_COUNT; i++) {
        float val = plc_tag_get_float32(tag,(i*ELEM_SIZE));

        val = val+1.5f;

        fprintf(stderr,"Setting element %d to %f\n",i,val);

        plc_tag_set_float32(tag,(i*ELEM_SIZE),val);
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
        fprintf(stderr,"data[%d]=%f\n",i,plc_tag_get_float32(tag,(i*ELEM_SIZE)));
    }

    /* we are done */
    plc_tag_destroy(tag);

    return 0;
}


