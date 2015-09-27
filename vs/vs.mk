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


CFLAGS=/MDd /c /DWIN32=1 /DLIBPLCTAGDLL_EXPORTS=1
LFLAGS=/DLL /DEBUG 
LIBRARY_NAME= libplctag.dll

LIB_DIR=..\lib
LIB_SRC= $(LIB_DIR)\libplctag_tag.c

AB_DIR=..\lib\ab
AB_SRC= $(AB_DIR)\ab_common.c \
	$(AB_DIR)\cip.c \
	$(AB_DIR)\connection.c \
	$(AB_DIR)\eip.c \
	$(AB_DIR)\eip_cip.c \
	$(AB_DIR)\eip_dhp_pccc.c \
	$(AB_DIR)\eip_pccc.c \
	$(AB_DIR)\pccc.c \
	$(AB_DIR)\request.c \
	$(AB_DIR)\session.c

UTIL_DIR=..\lib\util
UTIL_SRC=$(UTIL_DIR)\attr.c

PLATFORM_DIR=..\lib\windows
PLATFORM_SRC=$(PLATFORM_DIR)\platform.c

INC_DIRS=/I$(LIB_DIR) /I$(AB_DIR) /I$(UTIL_DIR) /I$(PLATFORM_DIR)

OBJ_DIR=obj
OBJS=	$(LIB_DIR)\libplctag_tag.obj \
	$(AB_DIR)\ab_common.obj \
	$(AB_DIR)\cip.obj \
	$(AB_DIR)\connection.obj \
	$(AB_DIR)\eip.obj \
	$(AB_DIR)\eip_cip.obj \
	$(AB_DIR)\eip_dhp_pccc.obj \
	$(AB_DIR)\eip_pccc.obj \
	$(AB_DIR)\pccc.obj \
	$(AB_DIR)\request.obj \
	$(AB_DIR)\session.obj \
	$(UTIL_DIR)\attr.obj \
	$(PLATFORM_DIR)\platform.obj

$(LIB_DIR)\libplctag_tag.obj: $(LIB_DIR)\libplctag_tag.c $(LIB_DIR)\libplctag_tag.h $(LIB_DIR)\libplctag.h $(PLATFORM_DIR)\platform.h $(UTIL_DIR)\attr.h
	cl $(INC_DIRS) $(CFLAGS) /Fo$(LIB_DIR)\ /Tc $(LIB_DIR)\libplctag_tag.c

$(AB_DIR)\ab_common.obj: $(AB_DIR)\ab_common.c $(AB_DIR)\ab_common.h $(LIB_DIR)\libplctag_tag.h $(LIB_DIR)\libplctag.h $(PLATFORM_DIR)\platform.h $(UTIL_DIR)\attr.h $(AB_DIR)\ab.h $(AB_DIR)\pccc.h $(AB_DIR)\cip.h $(AB_DIR)\eip.h $(AB_DIR)\eip_cip.h $(AB_DIR)\eip_pccc.h $(AB_DIR)\eip_dhp_pccc.h $(AB_DIR)\session.h $(AB_DIR)\connection.h $(AB_DIR)\tag.h $(AB_DIR)\request.h
	cl $(INC_DIRS) $(CFLAGS) /Fo$(AB_DIR)\ /Tc $(AB_DIR)\ab_common.c

$(AB_DIR)\cip.obj: $(AB_DIR)\cip.c $(AB_DIR)\ab_common.h $(LIB_DIR)\libplctag_tag.h $(LIB_DIR)\libplctag.h $(PLATFORM_DIR)\platform.h $(UTIL_DIR)\attr.h $(AB_DIR)\ab_common.h $(AB_DIR)\cip.h $(AB_DIR)\tag.h  $(AB_DIR)\eip.h
	cl $(INC_DIRS) $(CFLAGS) /Fo$(AB_DIR)\ /Tc $(AB_DIR)\cip.c

$(AB_DIR)\connection.obj: $(AB_DIR)\connection.c $(AB_DIR)\ab_common.h $(LIB_DIR)\libplctag_tag.h $(LIB_DIR)\libplctag.h $(PLATFORM_DIR)\platform.h $(UTIL_DIR)\attr.h $(AB_DIR)\connection.h $(AB_DIR)\session.h $(AB_DIR)\tag.h $(AB_DIR)\eip.h $(AB_DIR)\ab.h
	cl $(INC_DIRS) $(CFLAGS) /Fo$(AB_DIR)\ /Tc $(AB_DIR)\connection.c

$(AB_DIR)\eip.obj: $(AB_DIR)\eip.c $(AB_DIR)\ab_common.h $(LIB_DIR)\libplctag_tag.h $(LIB_DIR)\libplctag.h $(AB_DIR)\session.h $(AB_DIR)\tag.h $(AB_DIR)\eip.h $(AB_DIR)\request.h
	cl $(INC_DIRS) $(CFLAGS) /Fo$(AB_DIR)\ /Tc $(AB_DIR)\eip.c

$(AB_DIR)\eip_cip.obj: $(AB_DIR)\eip_cip.c $(AB_DIR)\ab_common.h $(LIB_DIR)\libplctag_tag.h $(LIB_DIR)\libplctag.h $(AB_DIR)\session.h $(AB_DIR)\tag.h $(AB_DIR)\eip.h $(AB_DIR)\eip_cip.h $(AB_DIR)\request.h
	cl $(INC_DIRS) $(CFLAGS) /Fo$(AB_DIR)\ /Tc $(AB_DIR)\eip_cip.c

$(AB_DIR)\eip_dhp_pccc.obj: $(AB_DIR)\eip_dhp_pccc.c $(AB_DIR)\ab_common.h $(LIB_DIR)\libplctag_tag.h $(LIB_DIR)\libplctag.h $(AB_DIR)\session.h $(AB_DIR)\tag.h $(AB_DIR)\eip.h $(AB_DIR)\eip_dhp_pccc.h $(AB_DIR)\request.h
	cl $(INC_DIRS) $(CFLAGS) /Fo$(AB_DIR)\ /Tc $(AB_DIR)\eip_dhp_pccc.c

$(AB_DIR)\eip_pccc.obj: $(AB_DIR)\eip_pccc.c $(AB_DIR)\ab_common.h $(LIB_DIR)\libplctag_tag.h $(LIB_DIR)\libplctag.h $(AB_DIR)\session.h $(AB_DIR)\tag.h $(AB_DIR)\eip.h $(AB_DIR)\eip_pccc.h $(AB_DIR)\request.h
	cl $(INC_DIRS) $(CFLAGS) /Fo$(AB_DIR)\ /Tc $(AB_DIR)\eip_pccc.c

$(AB_DIR)\pccc.obj: $(AB_DIR)\pccc.c $(AB_DIR)\ab_common.h $(LIB_DIR)\libplctag_tag.h $(LIB_DIR)\libplctag.h $(AB_DIR)\session.h $(AB_DIR)\tag.h $(AB_DIR)\eip.h $(AB_DIR)\pccc.h $(AB_DIR)\request.h
	cl $(INC_DIRS) $(CFLAGS) /Fo$(AB_DIR)\ /Tc $(AB_DIR)\pccc.c

$(AB_DIR)\request.obj: $(AB_DIR)\request.c $(AB_DIR)\ab_common.h $(LIB_DIR)\libplctag_tag.h $(LIB_DIR)\libplctag.h $(AB_DIR)\session.h $(AB_DIR)\tag.h $(AB_DIR)\eip.h $(AB_DIR)\request.h
	cl $(INC_DIRS) $(CFLAGS) /Fo$(AB_DIR)\ /Tc $(AB_DIR)\request.c

$(AB_DIR)\session.obj: $(AB_DIR)\session.c $(AB_DIR)\ab_common.h $(LIB_DIR)\libplctag_tag.h $(LIB_DIR)\libplctag.h $(AB_DIR)\session.h $(AB_DIR)\tag.h $(AB_DIR)\eip.h
	cl $(INC_DIRS) $(CFLAGS) /Fo$(AB_DIR)\ /Tc $(AB_DIR)\session.c

$(UTIL_DIR)\attr.obj: $(UTIL_DIR)\attr.c $(UTIL_DIR)\attr.h $(PLATFORM_DIR)\platform.h 
	cl $(INC_DIRS) $(CFLAGS) /Fo$(UTIL_DIR)\ /Tc $(UTIL_DIR)\attr.c

$(PLATFORM_DIR)\platform.obj: $(PLATFORM_DIR)\platform.c 
	cl $(INC_DIRS) $(CFLAGS) /Fo$(PLATFORM_DIR)\ /Tc $(PLATFORM_DIR)\platform.c

$(LIBRARY_NAME): $(OBJS)
	link /out:$(LIBRARY_NAME) $(LFLAGS) $(OBJS) ws2_32.lib


# build application
libplctag: $(LIBRARY_NAME)


# delete output directories
clean:
	@del $(OBJS) $(LIBRARY_NAME)

# create directories and build application
all: clean libplctag

