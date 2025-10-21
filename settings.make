# Makefile
# common make settings
#
# This file is part of chiark-utils, a collection of useful utilities
#
# This file is:
#  Copyright (C) 1997-1998,2000-2001 Ian Jackson <ian@chiark.greenend.org.uk>
#
# This is free software; you can redistribute it and/or modify it under the
# terms of the GNU General Public License as published by the Free Software
# Foundation; either version 3, or (at your option) any later version.
#
# This is distributed in the hope that it will be useful, but WITHOUT ANY
# WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
# FOR A PARTICULAR PURPOSE.  See the GNU General Public License for more
# details.
#
# You should have received a copy of the GNU General Public License along
# with this program; if not, consult the Free Software Foundation's
# website at www.fsf.org, or the GNU Project website at www.gnu.org.

CONFIG_CPPFLAGS=	-DRWBUFFER_SIZE_MB=$(RWBUFFER_SIZE_MB) \
			-DREALLY_CHECK_FILE='"/etc/inittab"' \
			-DREALLY_CHECK_FILE_2='"/etc/rc.local"'

CC=		gcc
CFLAGS=		$(WARNINGS) $(OPTIMISE) $(DEBUG) $(CMDLINE_CFLAGS)
CPPFLAGS=	$(CONFIG_CPPFLAGS) $(CMDLINE_CPPFLAGS)
LDFLAGS=	$(CMDLINE_LDFLAGS)

WARNINGS=	-Wall -Wwrite-strings -Wmissing-prototypes \
		-Wstrict-prototypes -Wpointer-arith -Wno-pointer-sign
OPTIMISE=	-O2
DEBUG=		-g

SYSTEM_USER= 	root
SYSTEM_GROUP=	root

prefix=/usr/local
etcdir=/etc
varlib=/var/lib

confdir=$(etcdir)/$(us)
bindir=$(prefix)/bin
sbindir=$(prefix)/sbin
sharedir=$(prefix)/share/$(us)
perl5dir=$(prefix)/share/perl5
txtdocdir=$(prefix)/share/doc/$(us)
python2dir=$(prefix)/lib/python2.7/dist-packages
python3dir=$(prefix)/lib/python3/dist-packages
exampledir=$(txtdocdir)/examples
vardir=$(varlib)/$(us)
mandir=${prefix}/man
man1dir=${mandir}/man1
man8dir=${mandir}/man8

# INSTALL_PROGRAM_STRIP_OPT=-s

INSTALL=		install -c
INSTALL_SHARE=		$(INSTALL) -m 644 -o $(SYSTEM_USER) -g $(SYSTEM_GROUP)
INSTALL_SCRIPT=		$(INSTALL) -m 755 -o $(SYSTEM_USER) -g $(SYSTEM_GROUP)
INSTALL_PROGRAM=	$(INSTALL_SCRIPT) $(INSTALL_PROGRAM_STRIP_OPT)
INSTALL_DIRECTORY=	$(INSTALL) -m 2755 -o $(SYSTEM_USER) -g $(SYSTEM_GROUP) -d
