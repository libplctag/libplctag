#
#   Copyright 2012, Process Control Engineers
#   Author: Kyle Hayes
#
#    This library is free software; you can redistribute it and/or
#    modify it under the terms of the GNU Library General Public
#    License as published by the Free Software Foundation; either
#    version 2 of the License, or (at your option) any later version.
#
#    This library is distributed in the hope that it will be useful,
#    but WITHOUT ANY WARRANTY; without even the implied warranty of
#    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
#    Library General Public License for more details.
#
#    You should have received a copy of the GNU Library General Public
#    License along with this library; if not, write to the Free Software
#    Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301
#    USA
#


ifeq ($(LIBPLC_ROOT),)
LIBPLC_ROOT = ./
endif

CC=gcc
LD=ld
AR=ar
INSTALL=install

ASYNC = async
ASYNC_SRC = async.c
ASYNC_OBJ = $(ASYNC_SRC:%.c=%.o)

SIMPLE = simple
SIMPLE_SRC = simple.c
SIMPLE_OBJ = $(SIMPLE_SRC:%.c=%.o)

LIBPLC_TEST = tag_rw
LIBPLC_TEST_SRC = tag_rw.c
LIBPLC_TEST_OBJ = $(LIBPLC_TEST_SRC:%.c=%.o)

CFLAGS += -fno-strict-aliasing -g -I. -fms-extensions -Wall -I/usr/local/include 
LIBS = -lplctag -lpthread -pthread
SHLIBS = $(LIBS)
# LIBPLC_LIB_SO = ../lib/libplctag.so

TARGETS = $(ASYNC) $(SIMPLE) $(LIBPLC_TEST)

all: $(TARGETS)

$(ASYNC): $(ASYNC_OBJ) $(DEP_LIBS)
	$(CC) -o $@ $(ASYNC_OBJ) $(LDFLAGS) $(SHLIBS) -L. $(LIBPLC_LIB_SO)

$(ASYNC_OBJ): $(ASYNC_SRC)
	$(CC) -c -o $@ $(ASYNC_SRC) $(CFLAGS)

$(SIMPLE): $(SIMPLE_OBJ) $(DEP_LIBS)
	$(CC) -o $@ $(SIMPLE_OBJ) $(LDFLAGS) $(SHLIBS) -L. $(LIBPLC_LIB_SO)

$(SIMPLE_OBJ): $(SIMPLE_SRC)
	$(CC) -c -o $@ $(SIMPLE_SRC) $(CFLAGS)

$(LIBPLC_TEST): $(LIBPLC_TEST_OBJ) $(DEP_LIBS)
	$(CC) -o $@ $(LIBPLC_TEST_OBJ) $(LDFLAGS) $(SHLIBS) -L. $(LIBPLC_LIB_SO)

$(LIBPLC_TEST_OBJ): $(LIBPLC_TEST_SRC)
	$(CC) -c -o $@ $(LIBPLC_TEST_SRC) $(CFLAGS)

clean:
	rm -rf $(TARGETS) *.o *.so *~ Makefile.depends *.log

