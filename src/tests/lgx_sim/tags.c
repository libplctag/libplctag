
#include <stdlib.h>
#include <string.h>
#include "log.h"
#include "tags.h"


tag_data *tags = NULL;
size_t num_tags = 0;

void init_tags()
{
    num_tags = 2;

    tags = (tag_data *)calloc(num_tags, sizeof(tag_data));

    tags[0].name = "TestDINTArray";
    tags[0].data_type[0] = 0xc4;
    tags[0].data_type[1] = 0x00;
    tags[0].elem_count = 10;
    tags[0].elem_size = 4;
    tags[0].data = (uint8_t *)calloc(tags[0].elem_size, tags[0].elem_count);

    tags[1].name = "TestBigArray";
    tags[1].data_type[0] = 0xc4;
    tags[1].data_type[1] = 0x00;
    tags[1].elem_count = 1000;
    tags[1].elem_size = 4;
    tags[1].data = (uint8_t *)calloc(tags[1].elem_size, tags[1].elem_count);

}


tag_data *find_tag(const char *tag_name)
{
    log("find_data() finding tag %s\n", tag_name);

    for(size_t i=0; i<num_tags; i++) {
        if(strcmp(tags[i].name, tag_name) == 0) {
            return &(tags[i]);
        }
    }

    log("find_tag() unable to find tag %s\n", tag_name);

    return NULL;
}
