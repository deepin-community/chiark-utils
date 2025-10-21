# Makefile

# This file is part of chiark-utils, a collection of useful programs
# used on chiark.greenend.org.uk.
#
# This file is:
#  Copyright (C) 2001 Ian Jackson <ian@chiark.greenend.org.uk>
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

include ../settings.make

SCRIPTS=	palm-datebook-reminders random-word expire-iso8601 \
		genspic2gnuplot gnucap2genspic ngspice2genspic \
		cvs-repomove cvs-adjustroot remountresizereiserfs \
		hexterm summarise-mailbox-preserving-privacy \
		git-cache-proxy git-branchmove nntpid

MANPAGES1=	palm-datebook-reminders git-branchmove

CSCRIPTS=	named-conf
CMANPAGES8=	named-conf

PERLMODULES=	Chiark/NNTP Proc/Prefork/Interp

all:

install:
		$(INSTALL_DIRECTORY) $(bindir)
		set -e; for f in $(SCRIPTS); do \
			$(INSTALL_SCRIPT) $$f $(bindir)/$$f; done
		set -e; for f in $(CSCRIPTS); do \
			$(INSTALL_SCRIPT) $$f $(bindir)/chiark-$$f; done
		set -e; for f in $(PERLMODULES); do \
			$(INSTALL_DIRECTORY) ${perl5dir}/$${f%/*}; \
			$(INSTALL_SHARE) $$f.pm $(perl5dir)/$$f.pm; done


install-docs: $(addsuffix .1,$(MANPAGES1))
		$(INSTALL_DIRECTORY) $(man1dir) $(man8dir)
		set -e; for f in $(MANPAGES1); do \
			$(INSTALL_SHARE) $$f.1 $(man1dir)/$$f.1; done
		set -e; for f in $(CMANPAGES8); do \
			$(INSTALL_SHARE) $$f.8 $(man8dir)/chiark-$$f.8; done

git-branchmove.1: git-branchmove
	pod2man git-branchmove >git-branchmove.1

install-examples:

clean:
		rm -f *~ ./#*#
		rm -f git-branchmove.1

distclean realclean:	clean
