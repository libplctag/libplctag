# libplctag - a C library for PLC communication

- [libplctag - a C library for PLC communication](#libplctag---a-c-library-for-plc-communication)
  - [WARNING - DISCLAIMER](#warning---disclaimer)
  - [Get It](#get-it)
  - [Features](#features)
    - [High Level Features](#high-level-features)
    - [Detailed Features](#detailed-features)
      - [PLC Support](#plc-support)
      - [Platform Support](#platform-support)
    - [Alternate Programming Languages](#alternate-programming-languages)
  - [Code](#code)
    - [How to Get The Code](#how-to-get-the-code)
    - [Example Code](#example-code)
    - [API](#api)
  - [Help Wanted](#help-wanted)
    - [How to Contribute](#how-to-contribute)
  - [History](#history)
  - [Contact and Support](#contact-and-support)
    - [libplctag Forum](#libplctag-forum)
    - [GitHub](#github)
  - [License](#license)
  - [Attributions and Trademarks](#attributions-and-trademarks)
  - [End Note](#end-note)

|   OS   | Architecture/CPU | Version |  64-bit   | 32-bit    |
|   --:  |         :-:      |   :-:   |    :-:    |    :-:    |
|Ubuntu  |   x86, Arm       | 18.04   | Supported | Supported |
|Windows |   x86, Arm       |10 (Server 19) | Supported | Supported |
|macOS   |   x86, Arm       |  11  | Supported | Not Supported |

![libplctag CI](https://github.com/libplctag/libplctag/workflows/libplctag%20CI/badge.svg?branch=release)

**libplctag** is an open source C library for Linux, Windows and macOS using **EtherNet/IP** or **Modbus TCP** to read and write tags in PLCs.  The library has been in production since early 2012 and is used by multiple organizations for many tasks including controlling radio telescopes, large and precision manufacturing, controlling fitness equipment, food handling and many, many more.

Current Stable Version: 2.5

Old Stable Version: 2.4

## WARNING - DISCLAIMER

Note: **PLCs control many kinds of equipment and loss of property, production or even life can happen if mistakes in programming or access are made.  Always use caution when accessing or programming PLCs!**

We make no claims or warrants about the suitability of this code for any purpose.

Be careful!

## Get It

Do you know what you want already?  Download it from the [releases page](https://github.com/libplctag/libplctag/releases)!

## Features

### High Level Features

- EtherNet/IP and Modbus TCP support.
- Open source licensing under the MPL 2.0 or LGPL 2+.
- Pure C library for portability across Linux, Windows and macOS as well as 32-bit and 64-bit.
- Support for x86, ARM and MIPS, and probably others.
- Very stable API with almost no changes other than feature additions since 2012.
- Low memory use and very high performance and capacity.  Uses protocol-specific features to increase performance.
- Simple API with minimal use of language-specific data to enable easy wrapping in other languages.
- Extensive example programs showing use of all library features.
- Wrappers for higher level languages like C#/.Net, Julia etc.
- Free!

### Detailed Features

#### PLC Support

- support for Rockwell/Allen-Bradley ControlLogix(tm) PLCs via CIP-EtherNet/IP (CIP/EIP or EIP).
  - read/write 8, 16, 32, and 64-bit signed and unsigned integers.
  - read/write single bits/booleans.
  - read/write 32-bit and 64-bit IEEE format (little endian) floating point.
  - raw support for user-defined structures (you need to pull out the data piece by piece)
  - read/write arrays of the above.
  - multiple-request support per packet.
  - packet size negotiation with newer firmware (version 20+) and hardware.
  - tag listing, both controller and program tags.
- support for Rockwell/Allen-Bradley Micro 850 PLCs.
  - Support as for ControlLogix where possible.
- support for older Rockwell/Allen-Bradley such as PLC-5 PLCs (Ethernet upgraded to support Ethernet/IP), SLC 500 and MicroLogix with Ethernet via CIP.
  - read/write of 16-bit INT.
  - read/write of 32-bit floating point.
  - read/write of arrays of the above (arrays not tested on SLC 500).
- support for older Rockwell/Allen-Bradley PLCs accessed over a DH+ bridge (i.e. a LGX chassis with a DHRIO module) such as PLC/5, SLC 500 and MicroLogix.
  - read/write of 16-bit INT.
  - read/write of 32-bit floating point.
  - read/write of arrays of the above.
- extensive example code.  Including
  - tag listing.
  - setting up and handling callbacks.
  - logging data from multiple tags.
  - reading and writing tags from the command line.
  - getting and setting individual bits as tags.
- Support for Omron NX/NJ series PLCs as for Allen-Bradley Micro800.
- Support for Modbus TCP.

#### Platform Support

- CMake build system for better cross-platform support on Windows, Linux and macOS.
  - Native CMake support is present in recent versions of Microsoft Visual Studio.
- Semantic versioning used and supported with specific library APIs for compatibility and feature checking.
- C library has no dependencies apart from libc (and pthreads on some platforms).
- Binary releases built for Ubuntu 18.04, macOS 10.15 and Windows 10.  All 64-bit, with 32-bit binary releases for Windows and Ubuntu.
- RaspberryPi supported. Both Linux and Windows IoT-based (some effort required to configure Visual Studio to build).

### Alternate Programming Languages

The C library is designed for easy wrapping.  Wrappers for many other languages exist:

- C++ (included)
- Java (included)
- Python (included)
- Go (included)
- Pascal (included)
- Part of the libplctag GitHub organization:
  - .Net Core, .Net Framework, [libplctag.NET](https://github.com/libplctag/libplctag.NET).
  - Julia, [PLCTag.jl](https://github.com/libplctag/PLCTag.jl).
- Other wrappers on GitHub:
  - C#, [Corsinvest](https://github.com/Corsinvest/cv4ab-api-dotnet).
  - C#, [Mesta Automation](https://github.com/mesta1/libplctag-csharp).
  - Labview, (see [libplctag-labview](https://github.com/dirtyb15/libplctag-labview))

## Code

### How to Get The Code

The code for the core library is at [libplctag](https://github.com/libplctag/libplctag).   Stable code is on the default _release_ branch.   If you check out code from GitHub, it will default to the _release_ branch.

If you want pre-built binaries, we have them available on the [releases](https://github.com/libplctag/libplctag/releases) page.   Just pick the one you want and download the ZIP file for your system.   We have 32 and 64-bit builds for x86 Linux and Windows and 64-bit builds for x86-64 macOS.

Go to the main project at the [libplctag organization](https://github.com/libplctag) to see the other wrappers.   We are in a state of transition right now as we move more alternate language wrappers into the GitHub organization.

### Example Code

Oh, wait, you want code!   There are many examples in the [examples](https://github.com/libplctag/libplctag/tree/release/src/examples) directory.

A good place to start is [simple.c](https://github.com/libplctag/libplctag/blob/release/src/examples/simple.c).

This code reads several 32-bit signed integers (DINT), updates them,
then writes them back out and rereads them from a tag named TestBigArray
in a Logix-class Allen-Bradley PLC.

The README file in the examples directory describes some of the more interesting ones.

### API

Most of the functions in the API are for data access.   Direct support for single bits, 8-bit, 16-bit, 32-bit and
64-bit words (integer and floating point) are provided by the library.

See the [API](https://github.com/libplctag/libplctag/wiki/API) for more information.

## Help Wanted

We need and welcome help with the following:

- bug reports!   We may not have your hardware so your bugs can help us make sure the library works in cases we cannot find!
- bug fixes.
- other protocols like Modbus, SBus etc.
- other platforms like Android, iOS etc.
- changes and additions for other PLCs.
- additional compilers.
- more language wrappers!
- patches and updates for existing language wrappers!
- Testing and more testing!

### How to Contribute

We love contributions!   Many users have contributed wrappers, extra functionality and bug fixes over the years.   The library is much better for all the help that users have provided.   **We ask that your code contributions to the core library are under the same dual MPL/LGPL license.**

Testing is difficult for us as we do not have access to all the different hardware out there.   If you can, a great way to contribute is to test prereleases.  These are on the _prerelease_ branch!  We appreciate all the help we get from our users this way.

The easiest way to contribute to the core library is to raise a PR on GitHub.

Wrappers in other languages are generally split off into separate projects.  Those may have different licenses and contribution processes.  Please look at the documentation for the wrapper in question.

## History

See the [wiki history page](https://github.com/libplctag/libplctag/wiki/History) for more details on how libplctag was created and why we built it.

## Contact and Support

There are two ways to ask for help or contact us.

### libplctag Forum

If you have general questions or comments about the
library, its use, or about one of the wrapper libraries, please join the Google group
[libplctag](https://groups.google.com/forum/#!forum/libplctag)!

The forum is open to all, but is by request only to keep the spammers down.  The traffic is fairly
light with usually a small number of emails per month.  It is our primary means for users to
ask questions and for discussions to happen.   Announcements about releases happen on the forum.

### GitHub

If you find bugs or need specific features, please file them on [GitHub's issue tracker](https://github.com/libplctag/libplctag/issues) for
the main C library project.  Each individual wrapper project has its own issue tracker.

If needed, we will initiate private communication from there.

## License

See the license files (LICENSE.MPL or LICENSE.LGPL) for our legal disclaimers of responsibility, fitness or
merchantability of this library as well as your rights with regards
to use of this library.  This code is **dual licensed** under the Mozilla Public License 2.0 (MPL 2.0) or the GNU
Lesser/Library General Public License 2 or later (LGPL 2+).

This dual license applies to the core C library.  Additional wrappers for other languages may be under different licenses.   Please see those projects for more information.

## Attributions and Trademarks

PLC5, SLC 500, MicroLogix, Micro8X0, CompactLogix and ControlLogix are trademarks of Rockwell/Allen Bradley.
Windows and Visual Studio are trademarks of Microsoft.  Apple owns the trademark on macOS.

Please let us know if we missed some so that we can get all the attributions correct!

## End Note

Have fun and let us know if this library is useful to you.  Please send test
cases if you run into bugs.  As PLC hardware is fairly expensive, we may not
be able to test out your test scenarios.  If possible, please send patches.
We do not ask that you transfer copyright over to us, but we do ask that you
make any submitted patches under the same licenses we use.  We will not
take any patches under the GPL license or licenses that are incompatible with the
MPL 2.0 license.

We hope you find this library as useful as we do!

- the libplctag team
