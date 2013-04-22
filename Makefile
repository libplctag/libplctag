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


ifeq ($(PLCCOMM_ROOT),)
PLCCOMM_ROOT = ./
endif

	
SUBDIRS = lib examples
DEFINES = DEBUG=1

all: plccomm

plccomm:
	@for mdir in $(SUBDIRS); do \
		if test -d $$mdir; then \
			if test -f $$mdir/configure -a ! -f $$mdir/Makefile; then \
				cd $$mdir; ./configure; cd ..; \
			fi; \
			$(MAKE) -C $$mdir PREFIX=$(prefix) $(DEFINES); \
		fi; \
	done


clean:
	-@for mdir in $(SUBDIRS); do \
		$(MAKE) -C $$mdir clean; \
	done

distclean:
	-@for mdir in $(SUBDIRS); do \
		$(MAKE) -C $$mdir distclean; \
	done

	