# Makefile
# simple make settings
#
# This file is part of chiark backup, a system for backing up GNU/Linux and
# other UN*X-compatible machines, as used on chiark.greenend.org.uk.
#
# chiark backup is:
#  Copyright (C) 1997-1998,2000-2001,2007
#                     Ian Jackson <ian@chiark.greenend.org.uk>
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

us=	chiark-backup

include ../settings.make

BINSCRIPTS=	checkallused loaded driver takedown whatsthis labeltape \
		snaprsync
SHARESCRIPTS=	bringup full increm snap-drop
SHAREFILES=	backuplib.pl snap-common
SNAPKINDS=	lvm remount remountrocp nosnap

EXAMPLES=	relativity chiark

all:

install:		all
		$(INSTALL_DIRECTORY) $(confdir) $(confdir)/snap $(bindir) \
			$(sharedir) $(vardir) $(man1dir)
		set -e; for s in $(BINSCRIPTS); do \
			$(INSTALL_SCRIPT) $$s $(bindir)/backup-$$s; done
		$(INSTALL_SHARE) $(SHAREFILES) $(sharedir)
		$(INSTALL_SCRIPT) $(SHARESCRIPTS) $(sharedir)
		set -e; for s in $(SNAPKINDS); do \
			d=$(confdir)/snap/$$s; \
			test ! -f $$d || d=$$d.dist; \
			$(INSTALL_SCRIPT) $$s $$d; done

install-docs:
		$(INSTALL_DIRECTORY) $(txtdocdir)
		$(INSTALL_SHARE) iwjbackup.txt $(txtdocdir)/README

install-examples:
		set -e; for e in $(EXAMPLES); do \
			cd examples/$$e; \
			$(INSTALL_DIRECTORY) $(exampledir)/$$e; \
			$(INSTALL_SHARE) [!A-Z]*[!~] $(exampledir)/$$e; \
			if test -f SYMLINKS.tar; then \
				exec <SYMLINKS.tar; \
				(set -e; cd $(exampledir)/$$e && tar -xf -); \
			fi; \
			cd ../..; \
		done

clean:
		rm -f *~ ./#*# *.o

distclean realclean:	clean
