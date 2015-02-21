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
CPP=g++
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

CFLAGS += -fno-strict-aliasing -g -I. -I../lib -fms-extensions -Wall -I/usr/local/include
LIBS = -L../lib -lplctag -lpthread -pthread
SHLIBS = $(LIBS)
# LIBPLC_LIB_SO = ../lib/libplctag.so

TARGETS = async simple simple_cpp string toggle_bool write_string tag_rw multithread multithread_plc5 multithread_cached_read plc5 slc500

all: $(TARGETS)
	
async: async.c
	$(CC) -o async async.c $(CFLAGS) $(SHLIBS)

simple: simple.c
	$(CC) -o simple simple.c $(CFLAGS) $(SHLIBS)

simple_cpp: simple_cpp.cpp
	$(CPP) -o simple_cpp simple_cpp.cpp $(CFLAGS) $(SHLIBS)

plc5: plc5.c
	$(CC) -o plc5 plc5.c $(CFLAGS) $(SHLIBS)

slc500: slc500.c
	$(CC) -o slc500 slc500.c $(CFLAGS) $(SHLIBS)

string: string.c
	$(CC) -o string string.c $(CFLAGS) $(SHLIBS)

toggle_bool: toggle_bool.c
	$(CC) -o toggle_bool toggle_bool.c $(CFLAGS) $(SHLIBS)

write_string: write_string.c
	$(CC) -o write_string write_string.c $(CFLAGS) $(SHLIBS)

tag_rw: tag_rw.c
	$(CC) -o tag_rw tag_rw.c $(CFLAGS) $(SHLIBS)

multithread: multithread.c
	$(CC) -o multithread multithread.c $(CFLAGS) $(SHLIBS)

multithread_plc5: multithread_plc5.c
	$(CC) -o multithread_plc5 multithread_plc5.c $(CFLAGS) $(SHLIBS)

multithread_cached_read: multithread_cached_read.c
	$(CC) -o multithread_cached_read multithread_cached_read.c $(CFLAGS) $(SHLIBS)


clean:
	rm -rf $(TARGETS) *.o *.so *~ Makefile.depends *.log

