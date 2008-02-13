# Copyright (C) 2007 Free Software Foundation, Inc.
# 
# This program is free software; you can redistribute it and/or modify it
# under the terms of the GNU General Public License as published by the
# Free Software Foundation; version 2.
# 
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License for more details.
# 
# You should have received a copy of the GNU General Public License
# along with this program; if not, write to the Free Software Foundation,
# Inc., 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA.
#
# Author: Wolfgang Beck <bewo at users.berlios.de> 2007

VER := 1.1
CFLAGS := -Wall -O3 -MMD 
LDFLAGS := -lraw1394 -liec61883 -ldv

datarot=${prefix}/share
bindir=/usr/local${exec_prefix}/bin
mandir=/usr/local${datarootdir}/man

SRC := \
normfile.c \
palettes.c \
scale.c \
util.c \
dv4l.c

INCS := \
palettes.h \
normfile.h \
scale.h \
util.h

ARCHIVE := \
CHANGES \
LICENSE \
Makefile.in \
config.h.in \
version.h.in \
configure.ac \
configure \
mkdv4lstart \
interdv4l.c \
dv4l.1 \
dv4lstart.1 \
$(SRC) \
$(INCS)

OBJ := $(SRC:.c=.o)

all: dv4l libdv4l.so dv4lstart

dv4l: $(OBJ)
	$(CC) $(LDFLAGS) -o $@ $(OBJ)

libdv4l.so: interdv4l.c normfile.c scale.c palettes.c util.c $(INCS)
	$(CC) -Wall -O3 -shared -fpic -Wl,-soname,$@ \
	-D DV4LLIBNAME=/usr/local${exec_prefix}/lib/libdv4l.so \
	-ldl -lraw1394 -liec61883 -ldv $< \
	normfile.c scale.c palettes.c util.c -o $@
	
dv4lstart:
	./mkdv4lstart /usr/local${exec_prefix}/lib $(VER)

tar:
	mkdir dv4l-$(VER)
	cp $(ARCHIVE) dv4l-$(VER)
	tar cvzf dv4l-$(VER).tar.gz dv4l-$(VER)
	rm -rf dv4l-$(VER)

clean:
	rm -f *.o core dv4l dv4lstart libdv4l.so

install: dv4l dv4lstart libdv4l.so
	install -D -m 0750 -g video dv4l $(bindir)
	install -D -m 0644 dv4l.1 $(mandir)/man1
	install -D -m 0644 dv4lstart.1 $(mandir)/man1
	install -D -m 0755 libdv4l.so /usr/local${exec_prefix}/lib
	install -D -m 0755 dv4lstart /usr/local${exec_prefix}/bin

uninstall:
	rm -f $(bindir)/dv4l
	rm -f $(mandir)/man1/dv4l.1
	rm -f $(mandir)/man1/dv4lstart.1
	rm -f /usr/local${exec_prefix}/bin/dv4lstart
	rm -f /usr/local${exec_prefix}/lib/libdv4l.so

-include *.d
