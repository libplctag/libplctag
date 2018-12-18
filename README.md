NOTICE
======

Version 2.0 is almost out.  It should be out by the end of 2018.   *It has breaking API changes!*

The release plan is to get 2.0 out, but not on the master branch.  Within a couple of week, I will move the existing stable release to the 1.5 branch and move 2.0 to master.



libplctag
=========

This library for Linux and Windows (not heavily tested) provides a means of accessing PLCs to read and write simple data.

Stable Version: 1.5

Future Version: 2.0

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

We will be setting up forums or a mailing list to support this library.  Look
here to find out when that happens.

- the OmanTek team.



Goals
=====

The primary goals of this library are to provide:

* a simple, consistent way to access PLCs or similar devices of various types.
* cross-platform support (currently Windows and Linux).
* protocol-agnostic access.
* an easily wrappable library.
* a portable library for big and little-endian and 32 and 64-bit processors.

We do not hit all those goals yet.  A lack of big-endian systems is preventing testing there.

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


Current Status
==============

Take a look at the [test page](https://github.com/kyle-github/libplctag/wiki/PLC-Testing-Results "PLC Testing Results") for more on specific PLC models.

The library has been in production use for several years and is in use by multiple organizations including some very large ones.

We are on version 1.5.  That includes:

* CMake build system for better cross-platform support.
* support for Rockwell/Allen-Bradley ControlLogix(tm) PLCs via CIP-EtherNet/IP (CIP/EIP or EIP)(tm?).
* native support for multiple data types:
  * read/write 8, 16, and 32-bit signed and unsigned integers.
  * read/write single booleans.
  * read/write 32-bit IEEE format (little endian) floating point.
  * raw support for user-defined structures (you need to pull out the data piece by piece)
  * read/write arrays of the above.
* preliminary support for Rockwell/Allen-Bradley MicroLogix 850 PLCs.
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
* support for 32 and 64-bit x86 Linux (Ubuntu 11.10, 12.04, 14.04, 15.04, 16.04 tested).
* support for maxOS is marginal.  It did work, but we do not have Macs.
* basic support for Windows 7 x86 (32-bit) builds with Visual Studio, not well tested (help very welcome!). Support is basic because:
  * we do not use Windows for our deployments.
  * only the tag_rw example program has been tested (though that tests most of the API)
* tested support AB ControlLogix (version 16 and version 20 firmware).
* sample code.
* a fairly stable API.  It has only had minor additions in four years.
* a fairly solid wrapper for Java.  Contributed wrappers for Python and C++.
* we have deployed this in customer environments.
* we have reports of successful use on ARM-based systems including the RaspberryPi boards.
* Other groups use this library (if you do, please let us know).

Planning for version 2.0 has started.  Please see the contact section at the bottom for a link to the
Google forum (Google Groups) for discussion.

PLC5, SLC 500, MicroLogix, CompactLogix and ControlLogix are trademarks of Rockwell/Allen Bradley.  Windows and Visual Studio are trademarks
of Microsoft.  Please let us know if we missed some so that we can get all the attributions correct!

We need and welcome help with the following:

* bug fixes and reports.
* other protocols like Modbus, SBus etc.
* other platforms like Android, iOS, macOS etc.
* 64-bit Windows.
* other versions of Windows.
* more language wrappers like Python, Ruby, VB, C++ etc.


Portability
===========

We have tried to maintain a high level of portability.  For the most part the
code conforms to C99.  However, we do the following things that may be an
issue for some compilers:

* we make assumptions about the bit layout of 32-bit IEEE floating point numbers in memory.  x86-based processors handle this fine, but nothing else has been tested.
* opaque pointers may be an issue on some compilers.
* we use packed structures and access structure elements off of alignment boundaries.
* threading.  We tried to avoid this, but at least Allen-Bradley/Rockwell's protocol is very much asynchronous.
* we still have one spot where we use inline variable declaration.

We do not have access to any big-endian machines.  We would love to have someone
with such access let us know if it is working.  We have put in byte swapping
code everywhere we think it will be a problem, but without a test platform...

We have limited access to some types of PLCs and most of our testing
has been on PLC5 and ControlLogix machines.

We are trying to keep things as simple as possible so that this can be easily
deployed in embedded systems.  We have limited the API "surface area" as much
as possible.  We also have made an effort to limit internal use of things
like malloc and free.  If you wrap the library, you will need to make sure that finalizers take care
of calling the destruction functions to deallocate internally allocated
memory.


Threading
=========

Access to the C tag data structure is not
thread-safe.  We have added lock/unlock API calls that use mutexes, but if you are using the library
wrapped in another language, you should use that language's synchronization primitives to prevent simultaneous access.

There is example code (C POSIX only) showing how to use the tag lock and unlock API functions.

If you share a tag between two threads, you are
going to get undefined behavior (almost certainly a crash) unless you serialize access.  We tried to keep the tag
data structure as lightweight as possible.

The Allen-Bradley EIP protocol is very asynchronous and the part of it that we
have implemented does use a thread internally.  We kept it to just one thread and
use non-blocking IO.


The API
=======

The library uses opaque pointers with accessor functions.  There are only a
few functions in the API.

These functions operation on all types of tags:

```c
    plc_tag plc_tag_create(const char *attrib_str);
    int plc_tag_lock(plc_tag tag);
    int plc_tag_unlock(plc_tag tag);
    int plc_tag_abort(plc_tag tag);
    int plc_tag_destroy(plc_tag tag);
    int plc_tag_read(plc_tag tag, int timeout);
    int plc_tag_status(plc_tag tag);
    int plc_tag_write(plc_tag tag, int timeout);
    int plc_tag_get_size(plc_tag tag);
```

The following functions get and set data within a tag's
local data.  Note that after you set something, you must
still call plc_tag_write(tag) to push it to the PLC.

```c
    uint32_t plc_tag_get_uint32(plc_tag tag, int offset);
    int plc_tag_set_uint32(plc_tag tag, int offset, uint32_t val);

    int32_t plc_tag_get_int32(plc_tag tag, int offset);
    int plc_tag_set_int32(plc_tag, int offset, int32_t val);

    uint16_t plc_tag_get_uint16(plc_tag tag, int offset);
    int plc_tag_set_uint16(plc_tag tag, int offset, uint16_t val);

    int16_t plc_tag_get_int16(plc_tag tag, int offset);
    int plc_tag_set_int16(plc_tag, int offset, int16_t val);

    uint8_t plc_tag_get_uint8(plc_tag tag, int offset);
    int plc_tag_set_uint8(plc_tag tag, int offset, uint8_t val);

    int8_t plc_tag_get_int8(plc_tag tag, int offset);
    int plc_tag_set_int8(plc_tag, int offset, int8_t val);

    float plc_tag_get_float32(plc_tag tag, int offset);
    int plc_tag_set_float32(plc_tag tag, int offset, float val);
```

Most of the functions in the API are for data access.

See the [API](https://github.com/kyle-github/libplctag/wiki/API "API Wiki Page") for more information.


Sample Code
===========

Oh, wait, you want code!

(this is from simple.c in the examples)

The following code reads 10 32-bit signed integers, updates them,
then writes them back out and rereads them from a tag named myDINTArray
in a Logix-class Allen-Bradley PLC located at IP 192.168.1.42.  The PLC
processor is located at slot zero in the backplane.

This example is for Linux.

```c
#include <stdio.h>
#include <unistd.h>
#include <libplctag.h>


#define TAG_PATH "protocol=ab_eip&gateway=192.168.1.42&path=1,0&cpu=LGX&elem_size=4&elem_count=10&name=myDINTArray"
#define ELEM_COUNT 10
#define ELEM_SIZE 4
#define DATA_TIMEOUT 5000

int main(int argc, char **argv)
{
    plc_tag tag = PLC_TAG_NULL;
    int rc;
    int i;

    /* create the tag */
    tag = plc_tag_create(TAG_PATH);

    /* everything OK? */
    if(!tag) {
        fprintf(stderr,"ERROR: Could not create tag!\n");

        return 0;
    }

    /* let the connect succeed we hope */
    while(plc_tag_status(tag) == PLCTAG_STATUS_PENDING) {
        sleep(1);
    }

    if(plc_tag_status(tag) != PLCTAG_STATUS_OK) {
        fprintf(stderr,"Error setting up tag internal state.\n");
        return 0;
    }

    /* get the data */
    rc = plc_tag_read(tag, DATA_TIMEOUT);

    if(rc != PLCTAG_STATUS_OK) {
        fprintf(stderr,"ERROR: Unable to read the data! Got error code %d\n",rc);

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
        fprintf(stderr,"ERROR: Unable to read the data! Got error code %d\n",rc);

        return 0;
    }


    /* get the data again*/
    rc = plc_tag_read(tag, DATA_TIMEOUT);

    if(rc != PLCTAG_STATUS_OK) {
        fprintf(stderr,"ERROR: Unable to read the data! Got error code %d\n",rc);

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

Current work is starting on version 2.0 of the library. We are using a type of semantic versioning,
so the API will have breaking changes in 2.0.  We will continue to support 1.5 after 2.0 is released
for some period of time.  At this point, we do not expect the conversion to 2.0 to be difficult as
the changes generally simplify the API and make it a little easier to use.


History
=======

We were tired of poor quality, expensive and non-portable OPC implementations.
Actually, we were tired of OPC.  It is a large protocol and provides
far more functionality than we need.  It is also not terribly portable since it is
based on Microsoft's OLE.  We
looked around to find an open-source library that would provide the 20% of
functionality of PLC access we needed to do 100% of our work.  We found some
very old and abandoned libraries for PCCC (AB's older protocol) and just one
EIP/CIP library, also apparently abandoned, that was marginally portable: TuxEIP.

TuxEIP was written by a company (probably in France based on the
email addresses and names) which seems to have disappeared off of the Internet.  The library's
original home site was long gone (still available via the Internet Archive).
It was only used as slightly patched version in the pvbrowser project.

([TuxEIP/TuxPLC](https://github.com/leicht/TuxPLC) can be found on Github now!)

We set about seeing if we could use the code.  We ran into some problems:

* the code was GPL.  That is too restrictive for us and our customers.  While we always
provide code with our projects, we needed more options than allowed by
the GPL.
* the code, while officially at 1.0, was not really finished.  It was not clear
which parts were fully functional and which were not.  Basic read/write access
of tags was fairly strong.  There was code that appeared to
be designed to dig deep into AB PLCs. We did not need most of the code.
* the code was only marginally portable.  Significant work would have to be
done to make it safe for other systems than 32-bit little-endian Linux.  The
pvbrowser project did some small patches to get it to compile with MinGW
on Windows, but it would not get very far with Visual Studio C/C++.  We tried.
* the code was organized such that it would be complicated to wrap for use in
Python or other languages.   We did a small preliminary Python wrapper for a
part of it.  It took a while and we were not very happy with it.  Raw pointers just
do not translate well.
* the code did not hide the intricacies of the EtherNet/IP (CIP) protocol, but
made application logic deal with them.
* the code had a lot of calls to malloc/free that made us nervous about both
memory use and memory leaks.
* the code was clearly abandoned hence we were on our own for patches and
support.

We tried for a while to find the original authors, but were not able to find them.
Our hope was to offer to take over the library in return for changing the license
to either the LGPL or BSD License.  The TuxEIP authors appear to have had access
to documentation about the CIP protocol as they reference sections and specific
points in the documents inside their code.  We do not have this access.

We decided to write our own library.  In looking at how TuxEIP worked, we made
the discovery that the AP protocol packets are not as dynamically sized as the
TuxEIP code makes it seem.  It turns out that there are a few basic complete
packets with one dynamically sized part at the end.  We were able to make our code a lot
simpler and almost completely remove dynamic memory allocation from the library during
normal operation.  Note that this means we definitely do **not** support the whole CIP protocol!

We copied no code from TuxEIP.  Those areas that are similar are due to the
necessities of coding the correct binary packet format.  Where we had no other
source, we tend to use the same element names as in TuxEIP where they appear to
correspond with some named construct that is part of the actual CIP specification
(at least that is what we thought was happening since we do not have that specification).
Where we had no clue, we made names up to fit what we thought was going on.

We could only find a few tidbits of free information on the Internet about how
the various layers of the EtherNet/IP (CIP) protocol(s) work.  There are several
layers to the whole thing.  Luckily, TuxEIP had already blazed that trail and
we were able to examine that code to find out how things worked.

The EtherNet/IP (as part of CIP) protocol specification is very large and very complicated
and covers several generations of Rockwell/Allen-Bradley PLCs.  Parts of it
date to systems that AB built before Ethernet was common and proprietary
networks like serial Data Highway were all that was around.

As we started our work on the library, we realized that it would be possible to
write a higher-level API that would handle all the protocol-specific parts of
basic read/write PLC communication.  We changed our library API again and started
that work.  That is what resulted in this project.

None of this could have happened without the hard work that the authors of
TuxEIP did before us.  Thanks!

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




