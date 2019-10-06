Build status:
Ubuntu: [![Build Status](https://dev.azure.com/kylehayes0607/libplctag/_apis/build/status/kyle-github.libplctag%20Ubuntu?branchName=master)](https://dev.azure.com/kylehayes0607/libplctag/_build/latest?definitionId=3&branchName=master)
Windows: [![Build Status](https://dev.azure.com/kylehayes0607/libplctag/_apis/build/status/kyle-github.libplctag%20Windows?branchName=master)](https://dev.azure.com/kylehayes0607/libplctag/_build/latest?definitionId=4&branchName=master)

libplctag
=========

This library for Linux and Windows provides a means of accessing PLCs to read and write
simple data.

Stable Version: 2.0

Old Stable Version: 1.5


WARNING - DISCLAIMER
====================

Note: *PLCs control many kinds of equipment and loss of property, production
or even life can happen if mistakes in programming or access are
made.  Always use caution when accessing or programming PLCs!*

We make no claims or warrants about the suitability of this code for
any purpose.

Be careful!

Have fun and let us know if this library is useful to you.  Please send test
cases if you run into bugs.  As PLC hardware is fairly expensive, we may not
be able to test out your test scenarios.  If possible, please send patches.
We do not ask that you transfer copyright over to us, but we do ask that you
make any submitted patches under the same LGPL license we use.  We will not
take any patches under the GPL license or licenses that are incompatible with the
LGPL.

- the libplctag team.



Goals
=====

The primary goals of this library are to provide:

* a simple, consistent way to access PLCs or similar devices of various types.
* cross-platform support (currently Windows and Linux), X86, x86-64, ARM and ARM64 (not well tested).
* protocol-agnostic access.
* an easily wrappable library.
* high performance.   We want to take advantage of PLC/protocol features that allow higher performance where possible.
* a portable library for big and little-endian, and 32 and 64-bit processors.

We do not hit all those goals yet.  A lack of big-endian systems is preventing testing
there.  It has been tested in the past on an emulated big-endian MIPS system in QEMU.

Non Goals
=========

This library is not to replace tools such as RSLinx or stacks like OPC.  It
only reads and writes tags.  Other tools and systems may provide significantly
more functionality.  We do not want that.  We want to keep it small and simple.

Licensing
=========

See the file LICENSE for our legal disclaimers of responsibility, fitness or
merchantability of this library as well as your rights with regards
to use of this library.  This code is licensed under the GNU LGPL.

Wrappers
========

Wrappers for various languages are provided, some included in the library source and others outside.

Wrappers exist for:
* C++ (included)
* Java (included)
* Python (included)
* Go (included)
* .Net (see [Mesta Automation](https://github.com/mesta1/libplctag-csharp))
* Labview (see [here](https://github.com/dirtyb15/libplctag-labview))


Current Status
==============

Take a look at the [test page](https://github.com/kyle-github/libplctag/wiki/PLC-Testing-Results "PLC Testing Results") for more on specific PLC models.

The library has been in production use since 2012 and is in use by multiple organizations including some very large ones.

We are on API version 2.0.  That includes:

* CMake build system for better cross-platform support.
* support for Rockwell/Allen-Bradley ControlLogix(tm) PLCs via CIP-EtherNet/IP (CIP/EIP or EIP)(tm?).   Firmware versions 16, 20 and 31.
  * read/write 8, 16, 32, and 64-bit signed and unsigned integers.
  * read/write single booleans under some circumstances (BOOL arrays are still tricky).
  * read/write 32-bit and 64-bit IEEE format (little endian) floating point.
  * raw support for user-defined structures (you need to pull out the data piece by piece)
  * read/write arrays of the above.
  * multiple-request support per packet.
  * packet size negotiation with newer firmward (version 20+).
* support for accessing Logix-class PLCs via PLC/5 protocol.
  * support for INT and REAL read and write.
* support for Rockwell/Allen-Bradley MicroLogix 850 PLCs.
  * Support as for ControlLogix.
* support for Rockwell/Allen-Bradley MicroLogix 1100 and 1400 series (not CIP-based)
  * use as per PLC5/SLC below.
  * Bits not directly supported yet.
  * Three address form not well supported.
* support for Rockwell/Allen-Bradley PLC5 PLCs (E-series with Ethernet), and SLC 500 with Ethernet via CIP.
  * read/write of 16-bit INT.
  * read/write of 32-bit floating point.
  * read/write of arrays of the above (arrays not tested on SLC 500).
* support for Rockwell/Allen-Bradley PLC5 PLCs accessed over a DH+ bridge (i.e. a LGX chassis with a DHRIO module).
  * read/write of 16-bit INT.
  * read/write of 32-bit floating point.
  * read/write of arrays of the above.
* support for 32 and 64-bit x86 Linux (Ubuntu 12.04-18.10 tested).
* Windows 7 x86 and Windows 10 x64 builds with Visual Studio, not well tested (help very welcome!). Support is basic because:
  * we do not use Windows for our deployments.
  * only the tag_rw example program has been tested (though that tests most of the API).
* sample code.
* a stable API.  The release of 2.0 is the first breaking change in over four years.
  * stable C API with wrappers in Python and Java.
  * user-contributed/user-supported wrappers for C# and Pascal.
* support for request bundling on supporting PLCs (ControlLogix and CompactLogix).  This is automatic within the library.
* we have deployed this in customer environments.
* we have reports of successful use on ARM-based systems including the RaspberryPi boards.
* other groups use this library (if you do, please let us know).

PLC5, SLC 500, MicroLogix, Micro8X0, CompactLogix and ControlLogix are trademarks of Rockwell/Allen Bradley.
Windows and Visual Studio are trademarks of Microsoft.  Please let us know if we missed some so
that we can get all the attributions correct!

We need and welcome help with the following:

* bug fixes and reports.
* other protocols like Modbus, SBus etc.
* other platforms like Android, iOS, macOS etc.
* other versions of Windows.
* more language wrappers!


Portability
===========

We have tried to maintain a high level of portability.  For the most part the
code conforms to C99.  However, we do the following things that may be an
issue for some compilers:

* we make assumptions about the bit layout of IEEE floating point numbers in memory.  x86-based processors handle this fine, but nothing else has been tested.
* opaque pointers may be an issue on some compilers, but this is generally not an issue with 2.0 as it no longer presents opaque pointers through the library API.
* we use packed structures and access structure elements off of alignment boundaries.
* threading.  We tried to avoid this, but at least Allen-Bradley/Rockwell's protocol is very much asynchronous.
* we still have one spot where we use inline variable declaration.   This is about the only remaining C99 requirement.

We have limited access to some types of PLCs and most of our testing
has been on PLC5 and ControlLogix machines.

We are trying to keep things as simple as possible so that this can be easily
deployed in embedded systems.  We have limited the API "surface area" as much
as possible.  We also have made an effort to limit internal use of things
like malloc and free.  If you wrap the library, you will need to make sure that
finalizers take care of calling the destruction functions to deallocate internally
allocated memory.


Threading
=========

Access to the C API is thread-safe.  All threads hit a mutex when going through any API call.
By "thread safe" we mean that you should not be able to crash the library by sharing a tag object
between threads.  That does not mean that you will get useful results!

Trying to share a tag between threads is possible, but access in only controlled within a
single API call.  If you do something like update tag data in one thread and call the read
function in another thread, the order of the operations will be determined by the actual
chronological order in which the threads executed them.

For this reason, we added additional lock/unlock API calls that allow you to synchronize
access to a tag across multiple API calls.  If you are using the library wrapped in another
programming language you should use that languages synchronization mechanisms instead.

The Allen-Bradley EIP protocol is very asynchronous and the part of it that we
have implemented does use threading internally.  The library uses one thread for all tags
and one thread per target PLC.


The API
=======

The library uses opaque integer handles with accessor functions.  There are only a
few functions in the API.

These functions operate on all types of tags. API functions that take a timeout argument
can be used in a synchronous manner or asynchronous manner.   Providing a timeout will
cause the library to do the work of waiting and checking for status.   Generally it is easier
to start out with the synchronous versions of the API and move to the asynchronous calls when
you understand it better and need the performance.

```c
    int32_t plc_tag_create(const char *attrib_str, int timeout);
    int plc_tag_lock(int32_t tag_id);
    int plc_tag_unlock(int32_t tag_d);
    int plc_tag_abort(int32_t tag_id);
    int plc_tag_destroy(int32_t tag_id);
    int plc_tag_read(int32_t tag_id, int timeout);
    int plc_tag_status(int32_t tag_id);
    int plc_tag_write(int32_t tag_id, int timeout);
    int plc_tag_get_size(int32_t tag_id);
```


The following functions get and set data within a tag's
local data.  Note that after you set something, you must
still call plc_tag_write(tag) to push it to the PLC.

```c
    /* version 2.0 */
    uint64_t plc_tag_get_uint64(int32_t tag_id, int offset);
    int plc_tag_set_uint64(int32_t tag_id, int offset, uint64_t val);

    int64_t plc_tag_get_int64(int32_t tag_id, int offset);
    int plc_tag_set_int64(int32_t tag_id, int offset, int64_t val);

    uint32_t plc_tag_get_uint32(int32_t tag_id, int offset);
    int plc_tag_set_uint32(int32_t tag_id, int offset, uint32_t val);

    int32_t plc_tag_get_int32(int32_t tag_id, int offset);
    int plc_tag_set_int32(int32_t tag_id, int offset, int32_t val);

    uint16_t plc_tag_get_uint16(int32_t tag_id, int offset);
    int plc_tag_set_uint16(int32_t tag_id, int offset, uint16_t val);

    int16_t plc_tag_get_int16(int32_t tag_id, int offset);
    int plc_tag_set_int16(int32_t tag_id, int offset, int16_t val);

    uint8_t plc_tag_get_uint8(int32_t tag_id, int offset);
    int plc_tag_set_uint8(int32_t tag_id, int offset, uint8_t val);

    int8_t plc_tag_get_int8(int32_t tag_id, int offset);
    int plc_tag_set_int8(int32_t tag_id, int offset, int8_t val);

    double plc_tag_get_float64(int32_t tag_id, int offset);
    int plc_tag_set_float64(int32_t tag_id, int offset, double val);

    float plc_tag_get_float32(int32_t tag_id, int offset);
    int plc_tag_set_float32(int32_t tag_id, int offset, float val);
```

Most of the functions in the API are for data access.

See the [API](https://github.com/kyle-github/libplctag/wiki/API) for more information.


Sample Code
===========

Oh, wait, you want code!

(this is from simple.c in the examples)

The following code reads 10 32-bit signed integers, updates them,
then writes them back out and rereads them from a tag named myDINTArray
in a Logix-class Allen-Bradley PLC located at IP 192.168.1.42.  The PLC
processor is located at slot zero in the backplane.

This example is for Linux.  It reads 200 DINTs (32-bit integers) from a tag, increments
the values of each one and writes them back.


```c
#include <stdio.h>
#include "../lib/libplctag.h"
#include "utils.h"


#define TAG_PATH "protocol=ab-eip&gateway=127.0.0.1&path=1,5&cpu=micro800&elem_size=4&elem_count=200&name=TestBigArray&debug=4"
#define ELEM_COUNT 200
#define ELEM_SIZE 4
#define DATA_TIMEOUT 5000


int main()
{
    int32_t tag = 0;
    int rc;
    int i;

    /* create the tag */
    tag = plc_tag_create(TAG_PATH, DATA_TIMEOUT);

    /* everything OK? */
    if(tag < 0) {
        fprintf(stderr,"ERROR %s: Could not create tag!\n", plc_tag_decode_error(tag));
        return 0;
    }

    if((rc = plc_tag_status(tag)) != PLCTAG_STATUS_OK) {
        fprintf(stderr,"Error setting up tag internal state. Error %s\n", plc_tag_decode_error(rc));
        plc_tag_destroy(tag);
        return 0;
    }

    /* get the data */
    rc = plc_tag_read(tag, DATA_TIMEOUT);
    if(rc != PLCTAG_STATUS_OK) {
        fprintf(stderr,"ERROR: Unable to read the data! Got error code %d: %s\n",rc, plc_tag_decode_error(rc));
        plc_tag_destroy(tag);
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
        plc_tag_destroy(tag);
        return 0;
    }

    /* get the data again*/
    rc = plc_tag_read(tag, DATA_TIMEOUT);

    if(rc != PLCTAG_STATUS_OK) {
        fprintf(stderr,"ERROR: Unable to read the data! Got error code %d: %s\n",rc, plc_tag_decode_error(rc));
        plc_tag_destroy(tag);
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
```


Future Work
===========

We have some things that we want to add to the library or change in the future.
There is no definite date for these changes.  Our goal is not to implement the
entire EIP/CIP stack.  We want to keep this as simple as we possibly can.  If you
want a library to read the code from a PLC or do anything other than read/write
a tag, this library is not for you.

That said, we have some longer term things in mind:

* increase portability.  This will be ongoing.
* make parts of the library optional.
* add more protocols.  We hope that the API will be able to support most of the commonly used PLC data access protocols.
  * Modbus is next.
  * Perhaps Siemens or other protocol after that.
* Embedded support.  This involves reducing memory requirements and finding alternate solutions for for threading.


History
=======

See the [wiki history page](https://github.com/kyle-github/libplctag/wiki/History) for more details on how libplctag was created and why we built it.


Contact
=======

There are two ways to contact us.

If you have general questions or comments about the
library or its use, please join and post on the Google group [libplctag](https://groups.google.com/forum/#!forum/libplctag).
The forum is open to all, but is by request only to keep the spammers down.  The traffic is fairly
light with usually a small number of emails per month.  It is our primary means for users to
ask questions and for discussions to happen.

If you find bugs or need specific features, please file them on GitHub's issue tracker for
the project.

If needed, we will initiate private communication from there.

Thanks for looking at this project.  We hope you find it useful!




