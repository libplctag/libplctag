The build system uses CMake to bootstrap a local build.  On Linux.  On Windows, this makes a Visual Studio project.


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

Run cmake

```
$ cmake ..
```

The ".." above is important.


## Compile the code

Run make

```
$ make
```

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

The build process on Windows is a little involved.  I welcome any contributions on how to make this cleaner.  Other than installing
the software, there is one step that needs to be done on the command line, but only the first time you build the system.  After that,
you do not need to do that step.

## Install MS Visual Studio

Testing was done using Visual Studio Community Edition 2015 on Windows 7 SP1 (not terribly up to date).

__WARNING__ Microsoft Visual Studio versions earlier than 2013 do not support C99!  There are C99 constructs in the code.  If you have Visual Studio
2012 or earlier, the code will not compile!

__WARNING__ I found out the hard way that the VS Express 2015 download does not include the C/C++ compiler!  You must trigger the IDE to download those by trying to make a
Windows C/C++ project (of any kind).  Then VS Express will download the compilers.  If you install VS Express from a CD image, I think you have to explicitly choose to
install the compilers.  I did not do it that way, so any corrections to these directions will be appreciated.

Install the compilers first!  CMake gets unhappy when it cannot find the compilers.

## Install CMake for Windows

Go to the cmake web site and download the installer.  Install cmake.  __NOTE__ install CMake so that it is usable either by all users or the current user.  The default is to 
not install it directly for any user!

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

I have no idea what would be different here.  From what I read, CMake might still be able to figure this out and then the rest of the process would be almost the same
as for Linux.  Please let me know if you get it to work and what steps you took!

# Instructions for macOS

I do not have a Mac.  All contributions here would be appreciated. I suspect that CMake will set up an XCode project correctly, but I have no way of testing that.
