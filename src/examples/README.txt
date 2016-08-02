This directory contains some examples in C showing how to use the library.

async.c:  This example shows how to set up and fire many tag reads simultaneously,
		  and then wait for them to complete.

simple.c: This is a basic tag read example.  It has a hardcoded tag name
          name and path and type.  You need to change them to match your
          system.
          
string.c: This example shows how to read an array of STRINGs.

toggle_bool.c: This example reads a boolean tag, inverts it, and writes back
		   the new value.

write_string.c: This example shows how to read and write an array of strings.

tag_rw.c: This is a more complicated example.  tag_rw is a command line
          tool you can use to read and write arbitrary tags in any
          supported PLC type and network.  This example shows setting
          and getting all of the core data types supported by the library.

These examples have not been tested on Windows.  They will probably work
with very few changes.
