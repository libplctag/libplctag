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


CFLAGS=/Iutils /I..\lib /MTd /W3 /O2
LIBS=..\vs\libplctag.lib ws2_32.lib
EXE_NAME=tag_rw.exe
OBJS=tag_rw.obj
C_SRCS=tag_rw.c
H_SRCS=..\lib\libplctag.h

$(EXE_NAME): $(C_SRCS) $(H_SRCS)
	cl $(CFLAGS) /Fe$(EXE_NAME) $(C_SRCS) $(LIBS)

# build application
tag_rw: $(EXE_NAME)

# delete output directories
clean:
	@del $(OBJS) $(EXE_NAME)

# create directories and build application
all: clean tag_rw

