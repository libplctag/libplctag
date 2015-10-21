#!/bin/bash

# Requires python 2.7.10 or later (for the standard inclusion of argparse)
python tag_rw.py -t uint8 -p "TODO:read_test_attribute_string"

python tag_rw.py -t real32 -p "TODO:write_test_attribute_string" -w "42.0"

python tag_rw.py -t sint32 -p "TODO:write_test_attribute_string" -w "-32"

# Not valid float, should not convert
python tag_rw.py -t real32 -p "TODO:write_test_attribute_string" -w "50,02"

# Not a integer, should not convert
python tag_rw.py -t uint32 -p "TODO:write_test_attribute_string" -w "X"

