/***************************************************************************
 *   Copyright (C) 2017 by Kyle Hayes                                      *
 *   Author Kyle Hayes  kyle.hayes@gmail.com                               *
 *                                                                         *
 *   This program is free software; you can redistribute it and/or modify  *
 *   it under the terms of the GNU Library General Public License as       *
 *   published by the Free Software Foundation; either version 2 of the    *
 *   License, or (at your option) any later version.                       *
 *                                                                         *
 *   This program is distributed in the hope that it will be useful,       *
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of        *
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the         *
 *   GNU General Public License for more details.                          *
 *                                                                         *
 *   You should have received a copy of the GNU Library General Public     *
 *   License along with this program; if not, write to the                 *
 *   Free Software Foundation, Inc.,                                       *
 *   59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.             *
 ***************************************************************************/


#include <stdio.h>
#include "../lib/libplctag.h"
#include "utils.h"



void test_version(void)
{
    plc_tag tag = PLC_TAG_NULL;
    int i;
    char ver[16] = {0,};

    fprintf(stderr,"Testing version tag.\n");

    tag = plc_tag_create("make=system&family=library&name=version&debug=4", 0);

    if(!tag) {
        fprintf(stderr,"ERROR: Could not create tag!\n");
        return;
    }

    plc_tag_read(tag, 0);

    for(i=0; i < 16 && plc_tag_get_uint8(tag,i) != 0; i++) {
        ver[i] = (char)plc_tag_get_uint8(tag,i);
    }

    fprintf(stderr,"Library version %s\n", ver);

    plc_tag_destroy(tag);
}



void test_debug(void)
{
    plc_tag tag = PLC_TAG_NULL;
    int old_debug, new_debug;

    fprintf(stderr,"Testing debug tag.\n");

    tag = plc_tag_create("make=system&family=library&name=debug&debug=4", 0);

    if(!tag) {
        fprintf(stderr,"ERROR: Could not create tag!\n");
        return;
    }

    plc_tag_read(tag, 0);

    old_debug = plc_tag_get_int32(tag,0);

    fprintf(stderr,"Current debug level is %d\n",old_debug);

    new_debug = (old_debug == 3 ? 4 : 3);

    plc_tag_set_int32(tag, 0, new_debug);

    plc_tag_write(tag, 0);

    plc_tag_read(tag, 0);

    new_debug = plc_tag_get_int32(tag,0);

    fprintf(stderr,"Now debug level is %d\n",new_debug);

    new_debug = old_debug;

    plc_tag_set_int32(tag, 0, new_debug);

    plc_tag_write(tag, 0);

    plc_tag_read(tag, 0);

    new_debug = plc_tag_get_int32(tag,0);

    fprintf(stderr,"Reset debug level to %d\n",new_debug);

    plc_tag_destroy(tag);
}









int main()
{
    test_version();

    test_debug();

    return 0;
}


