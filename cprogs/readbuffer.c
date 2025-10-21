/*
 * readbuffer.c
 *
 * A program for reading input from devices which don't like constant
 * stopping and starting, such as tape drives.  readbuffer is:
 *  Copyright (C) 1997-1998,2000-2001 Ian Jackson <ian@chiark.greenend.org.uk>
 *
 * readbuffer is part of chiark backup, a system for backing up GNU/Linux and
 * other UN*X-compatible machines, as used on chiark.greenend.org.uk.
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

const char *progname= "readbuffer";

static size_t waitempty;

int main(int argc, const char *const *argv) {
  int r,reading;

  startup(argv);
  waitempty= (buffersize*1)/4;
  reading=1;
  maxselfd=2;
  
  while (!seeneof || used) {
    
    FD_ZERO(&readfds);
    if (reading) {
      if (used<buffersize-1) {
	FD_SET(0,&readfds);
      } else {
	reading=0;
      }
    }
    FD_ZERO(&writefds); if (used) FD_SET(1,&writefds);

    callselect();

    if (FD_ISSET(0,&readfds)) {
      r= read(0,rp,min(buffersize-1-used,buf+buffersize-rp));
      if (!r) {
        seeneof=1; reading=0;
      } else if (r<0) {
        if (!(errno == EAGAIN || errno == EINTR)) { perror("read"); exit(1); }
      } else {
        used+= r;
        rp+= r;
        if (rp == buf+buffersize) rp=buf;
      }
    }

    if (FD_ISSET(1,&writefds)) {
      assert(used);
      r= write(1,wp,min(used,buf+buffersize-wp));
      if (r<=0) {
        if (!(errno == EAGAIN || errno == EINTR)) { perror("write"); exit(1); }
      } else {
        used-= r;
        wp+= r;
        if (wp == buf+buffersize) wp=buf;
      }
      if (used < waitempty && !seeneof) {
	reading=1;
      }
    }
  }
  exit(0);
}
