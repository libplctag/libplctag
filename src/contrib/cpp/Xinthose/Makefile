# ATD	3-3-16
#################### Configuration ####################
32_BIT_CPU = 0

# .c: gcc; .cpp/.cc: g++
CC = ccache g++

#################### Compiler Flags ####################
# -g: allows the use of gdb for finding seg faults: gdb ./main.out ; print [VALUE] ; list ; quit
# -Wall turns on most, but not all, compiler warnings
# -v verbose displays more compiler debugging information
# -Wno-write-strings: gets rid of this warning: deprecated conversion from string constant to ‘char*’ [-Wwrite-strings]
# -O#: optimizer, makes code faster by disabling debugging features; 3 is fastest
# -j #: make # files at the same time (1 for each core)
# -MMD = creates dependency files (.d) from user header files
# -MT = target of dependency rule
# -MF = file to write dependencies to
CFLAGS = -Wall -std=c++11 -Wno-write-strings -O3
CFLAGS += -DBOOST_LOG_DYN_LINK
ifeq ($(32_BIT_CPU),1)
	CFLAGS += -m32
else 
	CFLAGS += -m64
endif

# if a header file changes, it will recompile
#DEPS = $(wildcard src/*.d)

#################### define library paths, start with -L ####################
LFLAGS = -L/usr/local/lib -L/usr/lib
ifeq ($(32_BIT_CPU),1)
	LFLAGS += -L/usr/lib/i386-linux-gnu -L/lib/i386-linux-gnu
else
	LFLAGS += -L/usr/lib/x86_64-linux-gnu -L/lib/x86_64-linux-gnu
endif

#################### define libraries (.so, .a) to link into executable ####################
# start with -l, don't put lib in the name or an extension
LIBS = -lplctag -pthread

#################### define the C source files ####################
SRCS = $(wildcard src/*.cpp)

# define the C object files 
OBJS = $(addprefix obj/, $(notdir $(SRCS:.cpp=.o)))

# define the executable file 
EXE = main

# make depend, make clean
.PHONY: depend clean

#################### make ####################
all: $(EXE)

$(EXE): $(OBJS)
	$(CC) $(CFLAGS) -o $(EXE) $(OBJS) $(LFLAGS) $(LIBS)

# this is a suffix replacement rule for building .o's from .c's
# it uses automatic variables $<: the name of the prerequisite of
# the rule(a .c file) and $@: the name of the target of the rule (a .o file) 
# (see the gnu make manual section about automatic variables)
obj/%.o: src/%.cpp
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@
	
clean:
	$(RM) obj/*.o obj/*.d *~ $(EXE) src/*.o restwebservice/*.pyc include/*.hpp.gch
	
# this last blank line is needed for make to run
