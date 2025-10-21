# Makefile

# This file is part of chiark-utils, a collection of useful programs
# used on chiark.greenend.org.uk.
#
# This file is:
#  Copyright 2001-2002 Ian Jackson <ian@chiark.greenend.org.uk>
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

SYSTEM_USER=	root
SYSTEM_GROUP=	root

INSTALL=		install -c
INSTALL_SHARE=		$(INSTALL) -m 644 -o $(SYSTEM_USER) -g $(SYSTEM_GROUP)
INSTALL_SCRIPT=		$(INSTALL) -m 755 -o $(SYSTEM_USER) -g $(SYSTEM_GROUP)
INSTALL_DIRECTORY=	$(INSTALL) -m 2755 -o $(SYSTEM_USER) -g $(SYSTEM_GROUP) -d

prefix=/usr/local
bindir=$(prefix)/bin
mandir=${prefix}/man
man5dir=${mandir}/man5
man8dir=${mandir}/man8
txtdocdir=$(prefix)/share/doc/sync-accounts
exampledir=$(txtdocdir)/examples

SCRIPTS=	sync-accounts sync-accounts-createuser grab-account
MANPAGES5=	sync-accounts
MANPAGES8=	sync-accounts sync-accounts-createuser grab-account
EXAMPLES=	linux bsd

all:

install:
		$(INSTALL_DIRECTORY) $(bindir)
		set -e; for f in $(SCRIPTS); do \
			$(INSTALL_SCRIPT) $$f $(bindir)/$$f; done

install-docs:
		$(INSTALL_DIRECTORY) $(man5dir) $(man8dir)
		set -e; for f in $(MANPAGES5); do \
			$(INSTALL_SHARE) $$f.5 $(man5dir)/$$f.5; done
		set -e; for f in $(MANPAGES8); do \
			$(INSTALL_SHARE) $$f.8 $(man8dir)/$$f.8; done

install-examples:
		$(INSTALL_DIRECTORY) $(exampledir)
		set -e; for e in $(EXAMPLES); do \
			$(INSTALL_SHARE) sync-accounts.example-$$e \
				$(exampledir)/sync-accounts.$$e; \
			done

clean:
		rm -f *~ ./#*#

distclean realclean:	clean
