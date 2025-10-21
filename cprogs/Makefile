# Makefile
# simple make settings
#
# This file is part of chiark backup, a system for backing up GNU/Linux and
# other UN*X-compatible machines, as used on chiark.greenend.org.uk.
#
# chiark backup is:
#  Copyright (C) 1997-1998,2000-2001 Ian Jackson <ian@chiark.greenend.org.uk>
#  Copyright (C) 1999 Peter Maydell <pmaydell@chiark.greenend.org.uk>
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

us=	chiark-utils-bin

include ../settings.make

RWBUFFER_SIZE_MB=16

PROGRAMS=		readbuffer writebuffer with-lock-ex xbatmon-simple \
			summer watershed rcopy-repeatedly xduplic-copier \
			prefork-interp cgi-fcgi-interp
SUIDSBINPROGRAMS=	really
DAEMONS=		trivsoundd
MAN1PAGES=		readbuffer.1 writebuffer.1 with-lock-ex.1 \
			xduplic-copier.1 summer.1
MAN8PAGES=		trivsoundd.8 really.8
SEDDERYDOCS=		watershed.txt prefork-interp.txt cgi-fcgi-interp.txt \
			xbatmon-simple.txt rcopy-repeatedly.txt
BUILTTXTDOCS=		$(SEDDERYDOCS)
TXTDOCS=		$(BUILTTXTDOCS)

acctdump_cc_out:=$(shell \
	printf "\#include <sys/types.h>\n\#include <sys/acct.h>" \
		| $(CC) -E - 2>&1 >/dev/null \
)
ifeq (,$(acctdump_cc_out))
PROGRAMS+=acctdump
MAN8PAGES+=acctdump.8
else
$(warning Not building acctdump: $(acctdump_cc_out))
endif

TARGETS=	$(PROGRAMS) $(SUIDSBINPROGRAMS) $(DAEMONS) $(BUILTTXTDOCS)

all:		$(TARGETS)

readbuffer:			readbuffer.o			rwbuffer.o
writebuffer:			writebuffer.o	wrbufcore.o	rwbuffer.o 
trivsoundd:			trivsoundd.o	wrbufcore.o 	rwbuffer.o 
really:				really.o myopt.o
acctdump:			acctdump.o	myopt.o

acctdump.o really.o myopt.o rcopy-repeatedly.o: myopt.h
cgi-cfgi-interp.o prefork.o: myopt.h prefork.h timespeccmp.h
readbuffer.o writebuffer.o rwbuffer.o wrbufcore.o trivsoundd.o:	rwbuffer.h

xbatmon-simple: LDLIBS += -lX11 -lm

xduplic-copier: LDLIBS += -lXmu -lX11
#xduplic-copier: LDLIBS += -lXmu -lSM -lICE -lXt -lXext
#xduplic-copier: LDLIBS += -lX11 -lxcb -lXau -lXdmcp

summer:		summer.o
summer:		LDLIBS += -lnettle -lgmp

rcopy-repeatedly: rcopy-repeatedly.o myopt.o
rcopy-repeatedly: LDLIBS += -lm -lrt

watershed:	watershed.o common.o
watershed:	LDLIBS += -lnettle -lgmp

cgi-fcgi-interp:	cgi-fcgi-interp.o prefork.o myopt.o common.o
cgi-fcgi-interp:	LDLIBS += -lnettle

prefork-interp:		prefork-interp.o prefork.o myopt.o common.o
prefork-interp:		LDLIBS += -lnettle -luv

$(SEDDERYDOCS): %.txt: %.c
		sed '/^$$/,$$d' <$^ >$@.new && mv -f $@.new $@

install:		all
		$(INSTALL_DIRECTORY) $(bindir) $(sbindir)
		$(INSTALL_PROGRAM) $(PROGRAMS) $(bindir)
		$(INSTALL_PROGRAM) $(DAEMONS) $(sbindir)
		$(INSTALL) -m 4774 -o $(SYSTEM_USER) -g $(SYSTEM_GROUP) \
			$(SUIDSBINPROGRAMS) $(sbindir)

install-docs:	$(TXTDOCS)
		$(INSTALL_DIRECTORY) $(man1dir) $(man8dir) $(txtdocdir)
		$(INSTALL) -m 644 $(MAN1PAGES) ${man1dir}/.
		$(INSTALL) -m 644 $(MAN8PAGES) ${man8dir}/.
		$(INSTALL) -m 644 $(TXTDOCS) ${txtdocdir}/.

install-examples:

clean:
		rm -f *~ ./#*# *.o $(PROGRAMS)

distclean realclean:	clean
		rm -f $(TARGETS)
