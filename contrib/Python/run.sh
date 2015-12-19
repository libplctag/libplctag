#!/bin/bash

# Requires python 2.7.10 or later (for the standard inclusion of argparse)
python tag_rw.py -t uint8 -p "protocol=ab_eip&gateway=192.168.45.92&path=1,0&cpu=LGX&elem_size=1&name=MySINT"

python tag_rw.py -t real32 -p "protocol=ab_eip&gateway=192.168.45.92&path=1,0&cpu=LGX&elem_size=4&name=MyREAL" -w "42.0"

python tag_rw.py -t sint32 -p "protocol=ab_eip&gateway=192.168.45.92&path=1,0&cpu=LGX&elem_size=4&name=MyDINT" -w "-32"

# Not valid float, should not convert
python tag_rw.py -t real32 -p "protocol=ab_eip&gateway=192.168.45.92&path=1,0&cpu=LGX&elem_size=4&name=MyREAL" -w "50,02"

# Not a integer, should not convert
python tag_rw.py -t uint32 -p "protocol=ab_eip&gateway=192.168.45.92&path=1,0&cpu=LGX&elem_size=4&name=MyDINT" -w "X"

