# Instructions for Linux

Simply run `make` in this directory. libplctag.so will be built in the 'lib' folder, and examples will be built in the 'examples' folder.

Additionally, you can specify a different compiler:
```
$ CC=clang CXX=clang++ make
```

# Instructions for MSVC using nmake

1. Open a Developer Command Prompt (usually Start Menu > Visual Studio 20xx > Visual Studio Tools > Developer Command Prompt for VS20xx).
2. Navigate (cd) to the 'vs' folder.
3. Run `nmake /f vs.mk all`. libplctag.dll will be built in this folder.
4. To build examples, navigate to the 'examples' folder and run the same command.
5. Either add the 'vs' folder to PATH or copy/move libplctag.dll into the 'examples' folder in order to run the examples.
