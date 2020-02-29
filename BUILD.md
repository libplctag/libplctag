The build system uses CMake to bootstrap a local build.  On Linux.  On Windows, this makes a Visual Studio project.
CMake is also used to create a build on macOS.

Note that as of version 2.0.22, pre-built binaries are included in the GitHub releases.


# Instructions for Linux

## Install the compilers

For Ubuntu/Debian and derivatives, use

```
$ sudo apt-get install build-essential
```

That will get gcc.   Use your distro-specific instructions to install Clang if you want to use that compiler.

## Install cmake and git

For Ubuntu/Debian and derivatives, use

```
$ sudo apt-get install cmake git
```

Instructions for other Linux distributions happily accepted.


## Check out the code

Make a work directory in which you want to check out the code.

```
$ git clone https://github.com/kyle-github/libplctag.git
```

Or you can download one of the releases directly from GitHub.


## Build the Make build files


Go into the project build directory (the build directory may not already exist)/

```
$ cd libplctag
$ mkdir -p build
$ cd build
```

Run cmake (use "Release" for a release build and "Debug" for a debug build).

```
$ cmake .. -DCMAKE_BUILD_TYPE=Release
```

The ".." above is important.


## Compile the code

Run make

```
$ make
```

The binaries will be in the `build/bin_dist` directory.   This includes the libraries (static and dynamic) and the
executables for the test and example programs.

## Alternate compilers

If you want to use Clang instead, install Clang first.

Then set the default compilers to Clang

```
$ export CC=clang
$ export CXX=clang++
$ cmake ..
$ make
```

# Instructions for Windows using Microsoft Visual Studio

The build process on Windows is a little involved if it is an older version of Visual Studio.  I welcome any contributions on how to make this cleaner.  Other than installing
the software, there is one step that needs to be done on the command line, but only the first time you build the system.  After that,
you do not need to do that step.

Use the latest version of Visual Studio if at all possible!   As of Visual Studio Community 2019, GitHub and CMake are integrated into the VS build
system and it is really seamless and pleasant to use (thanks, Microsoft!).

# Visual Studio Community 2019 with Windows 10 (19.03)

Microsoft has integrated GitHub and CMake into VS 2019 very nicely.  It is highly recommended to use at least this version as the integration
has made the process much easier than in the past!

* Download at least Visual Studio 2019.  I use the Community, free, version.
* Make sure that you select the C++ command line parts when selecting the components to install.   This installs the C part too.
* After installation, start VS 2019.
* Select "Clone or Check out code" over on the top right of the splash/start up screen.
* Enter the link information for the libplctag GitHub project.
* This churns for a while (at least in a VM on a fast laptop with a NVMe drive, your wait time may vary).
* VS 2019 will notice that this is a CMake-based project and automatically start setting everything up.
* Once this is done, you should be able to build the project.
* Project binaries may take a little bit of a hunt to find.   If anyone has any tips on how to instruct VS through CMake to do something a little more obvious here, I would love to know!

## ARM Windows builds

It is possible to build for ARM Windows.   The following is how I was able to do this and tested by a user with a 32-bit Raspberry Pi:

* Get at least Visual Studio 2019 Community Edition.
* After installing the normal C++ stuff, I went into the installer and selected the ARM-specific packages.  There were some for ARM32 and ARM64.  I installed those.
* I opened VS 2019.
* I created a project from "Clone or Check out code" over on the top right of the splash/start up screen.
* I entered the GitHub info for libplctag.
* Wait while this churns for a bit.
* Then VS notices that this is a CMake based file and starts processing it.   After a period of time you should see the Solution Explorer etc. all ready to go.
* Project -> CMake Settings
* I clicked on the big green "+" to create a new build type.
* I selected x64-Debug
* In the right hand side of that, I renamed it to "ARM".
* little further down in the list of options, in "Toolset" I selected "msvc-ARM".
* ^S to save.
* Then I did a full clean and rebuilt my project.
* Your binaries should be ARM binaries now.

# Visual Studio Community 2015

Testing was done using Visual Studio Community Edition 2015 on Windows 7 SP1 (not terribly up to date).

__WARNING__ Microsoft Visual Studio versions earlier than 2013 do not support C99!  There are C99 constructs in the code.  If you have Visual Studio
2012 or earlier, the code will not compile!

__WARNING__ I found out the hard way that the VS Express 2015 download does not include the C/C++ compiler!  You must trigger the IDE to download those by trying to make a
Windows C/C++ project (of any kind).  Then VS Express will download the compilers.  If you install VS Express from a CD image, I think you have to explicitly choose to
install the compilers.  I did not do it that way, so any corrections to these directions will be appreciated.

Install the compilers first!  CMake gets unhappy when it cannot find the compilers.

## Install CMake for Windows

Go to the cmake web site and download the installer.  Install cmake.  __NOTE__ install CMake so that it is usable either by all users or the current user.  The default is to
not install it directly for any user! (Thanks to Nate Miller for pointing this out!)

## Install Git

There are many Git clients for Windows.  Select one that suits your workflow.

## Check out the code

Instructions for this depend on what Git client you use.  I have Windows in a VM sharing a directory in the host Linux system, so I did not do this step on Windows.
Any contributions to the instructions here greatly appreciated!

## Open a Developer command prompt

(I am not sure you need to do this, it looks like you do not if you have CMake properly installed.) Open a Developer Command Prompt (usually Start Menu > Visual Studio 20xx > Visual Studio Tools > Developer Command Prompt for VS20xx).

This is required because there are a lot of special paths that need to be set up for Visual Studio and CMake to find the compilers, include files etc.

## Run CMake

```
C:> cd Projects
C:\Projects> cd libplctag\build
C:\Projects\libplctag\build> cmake ..
```

In the above example, I am assuming that you checked out the code into the folder C:\Projects\libplctag.  Change that to match where you checked it out.

__Note__ you should only have to do this step once.  The project file that CMake creates makes the CMakeFiles.txt file a dependency in the project and Visual
Studio will rerun CMake if it changes.   So after this step is done, you should be able to close the special Developer command prompt and not have to use it
again unless you check out the code into a new folder.

There is a GUI for CMake, but it seems to be only able to create a new project, not load an existing one.  If anyone knows how to make this step work directly in
the CMake GUI, please let me know!  It would be nice to skip the command line steps for Windows.

## Build the project

Now you have a Visual Studio project file (.sln) in the build directory.  Open that in Visual Studio.  The project should be called libplctag_project.  Build
the whole project.

To run a test do this (substitute the arguments to tag_rw for your PLC)

```
C:\Projects\libplctag\build> Debug\tag_rw -t sint32 -p "protocol=ab_eip&gateway=10.206.1.39&path=1,0&cpu=lgx&elem_size=4&elem_count=10&name=TestDINTArray"
```

If you get an error about ucrtbased.dll missing, look here:

[MSDN ucrtbased.dll missing](https://social.msdn.microsoft.com/Forums/sqlserver/en-US/7f22624f-d8c9-435b-a546-f1fc470bfb5b/vs2015-c-project-needs-ucrtbaseddll-how-to-install?forum=vssetup)

The last response in that thread shows a DLL to copy.  At least on my Win7 system, this worked fine.

I only have Windows 7.  If you find that these instructions are wrong for Windows 10, please let me know what does work!


# Instructions for Windows with MINGW or similar

(Instructions from user alpep, thanks!)

I would like to share with you how I build the Libplctag for OS: Win10 32bit by using Mingw.

## Step by step procedure:

1. Download and install CMake:

  https://cmake.org/download/
  
  cmake-3.16.0-rc2-win32-x86.msi

  Add the CMake path (C:\Program Files\CMake\bin) to the system PATH, can be done while installing it.

2. Download and install MinGW64:

  https://sourceforge.net/projects/mingw-w64/files/Toolchains%20targetting%20Win32/Personal%20Builds/mingw-builds/installer/mingw-w64-install.exe/download

  mingw-w64-install.exe

  Install using the default settings

3. Download and unzip the library "libplctag-master", for example C:\libplctag-master.
In folder C:\libplctag-master, create a new folder "Build";

4. To build the library:
In Windows Start menu, click on MinGW-W64\Run Terminal;
This will add the MinGW directory to the system PATH and open the Windows command terminal.
Enter command lines:

```
C: <Enter>
cd libplctag-master\Build <Enter>
cmake -G "MinGW Makefiles" .. <Enter>
mingw32-make <Enter>
```

Binairies are in folder: `C:\libplctag-master\Build\bin_dist`

Executable needs following files `libgcc_s_dw2-1.dll` and `libwinpthread-1.dll` which are found in `C:\Program Files\mingw-w64\i686-8.1.0-posix-dwarf-rt_v6-rev0\mingw32\bin`.

Hope this could help someone else.


# Instructions for macOS

I do not have a Mac.  All contributions here would be appreciated. I suspect that CMake will set up an XCode project correctly, but I have no way of testing that.
