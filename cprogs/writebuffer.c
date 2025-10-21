/*
 * writebuffer.c
 *
 * A program for writing output to devices which don't like constant
 * stopping and starting, such as tape drives.  writebuffer is:
 *  Copyright (C) 1997-1998,2000-2001 Ian Jackson <ian@chiark.greenend.org.uk>
 *
 * writebuffer is part of chiark backup, a system for backing up GNU/Linux
 * and other UN*X-compatible machines, as used on chiark.greenend.org.uk.
 * chiark backup is:
 *  Copyright (C) 1997-1998,2000-2001 Ian Jackson <ian@chiark.greenend.org.uk>
 *  Copyright (C) 1999 Peter Maydell <pmaydell@chiark.greenend.org.uk>
 *
 * This is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 3,
 * or (at your option) any later version.
 *
 * This is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public
 * License along with this file; if not, consult the Free Software
 * Foundation's website at www.fsf.org, or the GNU Project website at
 * www.gnu.org.
 *
 */

#include "rwbuffer.h"

const char *progname= "writebuffer";

void wrbuf_report(const char *m) { }

int main(int argc, const char *const *argv) {
  startup(argv);
  wrbufcore_startup();
  while (!seeneof || used) {
    wrbufcore_prepselect(0,1);
    callselect();
    wrbufcore_afterselect(0,1);
  }
  exit(0);
}
