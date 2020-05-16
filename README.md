|   OS   | Version | 64-bit | 32-bit |
|   --:  |   :-:   |   :-:  |   :-:  |
|Ubuntu  |  18.04  | Supported | Supported |
|Windows |  10 (Server 19) | Supported | Supported |
|macOS   |  10.15  | Supported | Not Supported |

![libplctag CI](https://github.com/libplctag/libplctag/workflows/libplctag%20CI/badge.svg?branch=master)

libplctag
=========

This library for Linux, Windows and macOS provides a means of accessing PLCs to read and write
simple data.  The library has been in production use since 2012 and is in use by multiple organizations for many tasks
including controlling radio telescopes, large and precision manufacturing, controlling fitness equipment, food handling
and many, many more.

Current Stable Version: 2.1

Old Stable Version: 1.5


WARNING - DISCLAIMER
====================

Note: *PLCs control many kinds of equipment and loss of property, production
or even life can happen if mistakes in programming or access are
made.  Always use caution when accessing or programming PLCs!*

We make no claims or warrants about the suitability of this code for
any purpose.

Be careful!



High-Level Features
===================

* Pure C library for portability across Linux, Windows and macOS as well as 32-bit and 64-bit.
* Very stable API with almost no changes other than feature additions since 2012.
* Read and write tags of various types across AB/Rockwell PLCs such as ControlLogix, CompactLogix, Micro800 etc.
* Low memory use and very high performance and capacity.  Uses protocol-specific features to increase performance.
* Simple API with minimal use of language-specific data to enable easy wrapping in other languages.
* Extensive example programs showing use of all library features.
* Included wrappers for some languages.
* Open source licensing.
* Free!

Non-Goals
=========

This library is not to replace tools such as RSLinx or stacks like OPC.  It
only reads and writes tags.  Other tools and systems may provide significantly
more functionality.  We do not want that.  We want to keep it small and simple.

License
=========

See the file LICENSE for our legal disclaimers of responsibility, fitness or
merchantability of this library as well as your rights with regards
to use of this library.  This code is licensed under the GNU LGPL.


Detailed Features
=================

## PLC Support

* support for Rockwell/Allen-Bradley ControlLogix(tm) PLCs via CIP-EtherNet/IP (CIP/EIP or EIP)(tm?).   Firmware versions 16, 20 and 31.
  * read/write 8, 16, 32, and 64-bit signed and unsigned integers.
  * read/write single bits/booleans under some circumstances (BOOL arrays are still tricky).
  * read/write 32-bit and 64-bit IEEE format (little endian) floating point.
  * raw support for user-defined structures (you need to pull out the data piece by piece)
  * read/write arrays of the above.
  * multiple-request support per packet.
  * packet size negotiation with newer firmware (version 20+) and hardware.
  * tag listing, both controller and program tags.
* support for accessing Logix-class PLCs via PLC/5 protocol.
  * support for INT and REAL read and write.
* support for Rockwell/Allen-Bradley MicroLogix 850 PLCs.
  * Support as for ControlLogix where possible.
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
* extensive example code.  Including
  * tag listing.
  * setting up and handling callbacks.
  * logging data from multiple tags.
  * reading and writing tags from the command line.
  * getting and setting individual bits as tags.

## Platform Support

* CMake build system for better cross-platform support.
  * Native CMake support in recent versions of Microsoft Visual Studio.
* Semantic versioning used and supported with specific library APIs for compatibility and feature checking.
* C library has no dependencies apart from libc and pthreads on some platforms.
* Binary releases built for Ubuntu 18.04, macOS 10.15 and Windows 10.  All 64-bit, with 32-bit binary releases for Windows and Ubuntu.
* RaspberryPi supported. Both Linux and Windows IoT-based (some effort required to configure Visual Studio to build).


## Alternate Programming Languages

The C library is designed for easy wrapping.  Wrappers for many other languages exist:
* C++ (included)
* Java (included)
* Python (included)
* Go (included)
* Pascal (included)
* .Net/C#
  * [Corsinvest](https://github.com/Corsinvest/cv4ab-api-dotnet) supports .Net Core and is on Nuget!
  * [Mesta Automation](https://github.com/mesta1/libplctag-csharp).   Very popular with a nice introductory video.
  * [possibly experimental by timyhac, libplctag.Net](https://github.com/timyhac/libplctag.NET).   A relatively thin wrapper but loads the correct binary DLL at runtime.
* Labview (see [here](https://github.com/dirtyb15/libplctag-labview))



Help Wanted!
============

We need and welcome help with the following:

* bug fixes and reports!
* other protocols like Modbus, SBus etc.
* other platforms like Android, iOS etc.
* additional compilers.
* more language wrappers!
* more tests!



Sample Code
===========

Oh, wait, you want code!   There are many examples in the `src/examples` directory.

A good place to start is [simple.c](https://github.com/libplctag/libplctag/src/examples/simple.c).

This code reads 200 32-bit signed integers (DINT), updates them,
then writes them back out and rereads them from a tag named myDINTArray
in a Logix-class Allen-Bradley PLC.  

API
===

Most of the functions in the API are for data access.   Direct support for single bits, 8-bit, 16-bit, 32-bit and
64-bit words (integer and floating point) are provided by the library.

See the [API](https://github.com/libplctag/libplctag/wiki/API) for more information.



History
=======

See the [wiki history page](https://github.com/libplctag/libplctag/wiki/History) for more details on how libplctag was created and why we built it.


Contact
=======

There are two ways to contact us.

If you have general questions or comments about the
library or its use, please join and post on the Google group [libplctag](https://groups.google.com/forum/#!forum/libplctag).
The forum is open to all, but is by request only to keep the spammers down.  The traffic is fairly
light with usually a small number of emails per month.  It is our primary means for users to
ask questions and for discussions to happen.   Announcements about released happen on the forum.

If you find bugs or need specific features, please file them on GitHub's issue tracker for
the project.

If needed, we will initiate private communication from there.


Attributions and Trademarks
===========================

PLC5, SLC 500, MicroLogix, Micro8X0, CompactLogix and ControlLogix are trademarks of Rockwell/Allen Bradley.
Windows and Visual Studio are trademarks of Microsoft.  Apple owns the trademark on macOS.  

Please let us know if we missed some so that we can get all the attributions correct!


End Note
========

Have fun and let us know if this library is useful to you.  Please send test
cases if you run into bugs.  As PLC hardware is fairly expensive, we may not
be able to test out your test scenarios.  If possible, please send patches.
We do not ask that you transfer copyright over to us, but we do ask that you
make any submitted patches under the same LGPL license we use.  We will not
take any patches under the GPL license or licenses that are incompatible with the
LGPL.

We hope you find this library as useful as we do!

- the libplctag team

