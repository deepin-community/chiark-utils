# Makefile

# This file is part of chiark-utils, a collection of useful programs
# used on chiark.greenend.org.uk.
#
# This file is:
#  Copyright (C) 2017 Ian Jackson <ian.jackson@eu.citrix.com>
#  Copyright (C) 2001 Ian Jackson <ijackson@chiark.greenend.org.uk>
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

SCRIPTS=	fishdescriptor
MODULES_23=	__init__ indonor
MODULES_3=	fish

py=py/fishdescriptor
d2=$(python2dir)/fishdescriptor
d3=$(python3dir)/fishdescriptor

all:

install:
		$(INSTALL_DIRECTORY) $(bindir) $(d2) $(d3)
		set -e; for f in $(SCRIPTS); do \
			$(INSTALL_SCRIPT) $$f $(bindir)/$$f; done
		set -e; for f in $(MODULES_3); do \
			$(INSTALL_SHARE) $(py)/$$f.py $(d3)/.; done
		set -e; for f in $(MODULES_23); do \
			$(INSTALL_SHARE) $(py)/$$f.py $(d3)/.; \
			ln $(d3)/$$f.py $(d2)/.; done

install-docs:

install-examples:

clean:
		rm -f *~ ./#*#
		find -name \*.pyc -print0 | xargs -0r rm --

distclean realclean:	clean
