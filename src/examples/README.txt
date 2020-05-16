This directory contains some examples in C showing how to use the library.  Some of the
examples are described below.

async.c:  This example shows how to set up and fire many tag reads simultaneously,
          and then wait for them to complete.  Cross platform.

data_dumper.c: A simple data logger that outputs formatted text output with one row per sample.
          POSIX only.

list_tags.c: an example using the build-in ability to list out the tags in some AB/Rockwell PLCs.
          Specifically it will list tags in ControlLogix and CompactLogix PLCs.   Both controller
          and program tags are listed.   The output gives some information about the tag and a
          string suitable for using with plc_tag_create() to create a handle to that tag in the 
          PLC.

multithread.c: A simple example of multithreading using pthreads and the libplctag locking API calls.
          POSIX only.  Provide an argument giving the number of threads to use.  As you increase the
          number of threads, the average latency will increase.  Warning: you can really hammer the PLC
          with just a few threads.  Keep it under 30 for sanity.

multithread_cached_read.c: An example of cached read settings.  This is basically the same as the
          above example, but the tag is defined to cache reads.  By looking at the packet dumps,
          you can see just how much bandwidth this saves if your data can be a little stale.
          POSIX only.

multithread_plc5.c: As for multithread.c, but reading from a PLC5.  They are slow.  Do not use
          too many threads.

multithread_plc5_dhp.c: A version of the above, but is configured to use a tag in a PLC5 that is
          accessed via a DHRIO module in a ControlLogix chassis.  If you thought the above was
          slow...  This gives an example of how to configure this kind of pass through.  It is
          used for stress testing.  POSIX only.

plc5.c:   A simple example of direct PLC 5 access.  The PLC 5 must have Ethernet and have updated
          firmware such that it can use the limited EIP/CIP protocol needed.  Cross platform.

simple.c: This is a basic tag read example.  It has a hardcoded tag name
          name and path and type.  You need to change them to match your
          system.  Cross platform

simple.cpp: As above, but in C++.

simple_dual.c: Connects to two different kinds of PLCs simultaneously.  Shows how the API is identical
          for each.  Cross platform.

slc500.c: Shows connection to a SLC500.  Cross platform.

stress_test.c: Probably not the best code.  Written to stress test the library by heavily hitting a
          LGX PLC with multiple threads while closing and openning tags constantly.  Do not code your
          apps like this!  POSIX only.

string.c: This example shows how to read an array of STRINGs.  Cross platform.

test_callback.c: The library supports callbacks for internal events on tags and a callback for logging.  The
          latter is useful if you want to capture all logging output and direct it to your language/system
          instead of stderr.   The former can be used to trigger actions within your application code 
          when a tag starts a read, finishes a read, starts a write etc.  This can be used to create 
          transparent wrappers with some languages.

toggle_bool.c: This example reads a boolean tag, inverts it, and writes back
           the new value.  Cross platform.

write_string.c: This example shows how to read and write an array of strings.  Cross platform.

tag_rw.c: This is a more complicated example.  tag_rw is a command line
          tool you can use to read and write arbitrary tags in any
          supported PLC type and network.  This example shows setting
          and getting all of the core data types supported by the library.
          Cross platform.

These examples have not been tested as much on Windows.  They will probably work
with very few changes.
