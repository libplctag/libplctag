#
#   Copyright 2015, OmanTek
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

CC?=gcc
CXX?=g++

CFLAGS += -fno-strict-aliasing -g -I. -I../lib -fms-extensions -Wall -I/usr/local/include -O2
CXXFLAGS += $(CFLAGS)
LIBS = -L../lib -lplctag -lpthread -pthread

TARGETS = async data_dumper simple simple_cpp simple_dual string toggle_bool write_string tag_rw multithread multithread_plc5 multithread_plc5_dhp multithread_cached_read plc5 slc500

all: $(TARGETS)
	
%: %.c
	$(CC) -o $@ $< $(CFLAGS) $(LIBS)

%: %.cpp
	$(CXX) -o $@ $< $(CXXFLAGS) $(LIBS)

clean:
	rm -rf $(TARGETS) *.o *.so *~ Makefile.depends *.log
