This directory contains some examples in C showing how to use the library.

simple.c: This is a basic tag read example.  It has a hardcoded tag name
          name and path and type.  You need to change them to match your
          system.

tag_rw.c: This is a more complicated example.  tag_rw is a command line
          tool you can use to read and write arbitrary tags in any
          supported PLC type and network.  This example shows setting
          and getting all of the data types supported by the library.

These examples have not been tested on Windows.  They will probably work
with very few changes.
