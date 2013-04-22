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



LIBPLC_LIB_SO=libplctag.so
LIBPLC_LIB_SRC=libplctag_tag.c linux/platform.c util/attr.c ab/eip.c ab/util.c
LIBPLC_LIB_HEADER=libplctag.h libplctag_tag.h linux/platform.h util/attr.h ab/ab.h
LIBPLC_LIB_OBJ=$(LIBPLC_LIB_SRC:%.c=%.o)


DESTDIR=/usr/local/
bindir=bin
libdir=lib
incdir=include


CNOLIBFLAGS += -fno-strict-aliasing -g -I. -fms-extensions -Wall -I./linux
CFLAGS += -std=c99 -fPIC $(CNOLIBFLAGS) -DDEBUG=0 -D__USE_POSIX=1 -D_POSIX_C_SOURCE=200809L
DLIBS +=
LIBS += $(DLIBS)
SHLIBS = $(LIBS)

TARGETS = $(LIBPLC_LIB_SO)

all: $(TARGETS)

$(LIBPLC_LIB_SO): $(LIBPLC_LIB_OBJ) $(AB_LIB_OBJ) $(DEP_LIBS)
	$(CC) -o $@ -shared $(LIBPLC_LIB_OBJ) $(AB_LIB_OBJ) $(LDFLAGS) $(SHLIBS)

libplctag_tag.o: libplctag_tag.c libplctag_tag.h libplctag.h linux/platform.h util/attr.h ab/ab.h
	$(CC) -c -o $@ libplctag_tag.c $(CFLAGS)

#libplctag_util.o: libplctag_util.c
#	$(CC) -c -o $@ libplctag_util.c $(CFLAGS)

linux/platform.o: linux/platform.c linux/platform.h libplctag.h
	$(CC) -c -o $@ linux/platform.c $(CFLAGS)

ab/eip.o: ab/eip.c ab/ab.h ab/eip_data.h ab/util.h libplctag.h linux/platform.h util/attr.h
	$(CC) -c -o $@ ab/eip.c $(CFLAGS)

#ab/df1.o: ab/df1.c ab/ab.h ab/df1_data.h ab/util.h linux/platform.h
#	$(CC) -c -o $@ ab/df1.c $(CFLAGS)

ab/util.o: ab/util.c ab/util.h ab/util.h libplctag.h linux/platform.h util/attr.h
	$(CC) -c -o $@ ab/util.c $(CFLAGS)
	
util/attr.o: util/attr.c util/attr.h linux/platform.h
	$(CC) -c -o $@ util/attr.c $(CFLAGS)


clean:
	rm -rf $(TARGETS) $(LIBPLC_LIB_OBJ) $(LIBPLC_TEST_OBJ) $(AB_LIB_OBJ) $(AB_LIB) linux/*.o ab/*.o *.o *.so *~ Makefile.depends

install: all
	$(INSTALL) -m 644 libplctag.so $(DESTDIR)$(libdir)/
	$(INSTALL) -m 644 libplctag.h $(DESTDIR)$(incdir)/

newdepend: killdepend
	@echo "*******************************************"
	@echo "** Building dependencies..."
	@for mdir in $(SUBDIRS); \
		do $(MAKE) -C $$mdir depend; \
	done

killdepend:
	@echo "*******************************************"
	@echo "** Removing Old dependencies..."
	@find . -name "Makefile.depends" -print | $(XARGS) rm %
