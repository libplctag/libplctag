#pragma once

#include <stdint.h>

typedef struct {
    const char *name;
    uint8_t data_type[2];
    uint16_t elem_count;
    uint16_t elem_size;
    uint8_t *data;
} tag_data;


extern tag_data *tags;

extern void init_tags();
extern tag_data *find_tag(const char *tag_name);


