/*
 * wrbufcore.c
 *
 * Core of algorithm for writing output to devices which don't like
 * constant stopping and starting, such as tape drives.  This is:
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

static size_t waitfill;

int writing;

void wrbufcore_startup(void) {
  waitfill= (buffersize*3)/4;
  writing=0;
  maxselfd=0;
}

void fdsetset(int fd, fd_set *set) {
  FD_SET(fd,set);
  if (fd >= maxselfd) maxselfd= fd+1;
}

void wrbufcore_prepselect(int rdfd, int wrfd) {
  FD_ZERO(&readfds);
  if (rdfd>=0 && !seeneof && used+1<buffersize) fdsetset(rdfd,&readfds);
  
  FD_ZERO(&writefds);
  if (writing) fdsetset(wrfd,&writefds);
}

void wrbufcore_afterselect(int rdfd, int wrfd) {
  int r;
    
  if (FD_ISSET(wrfd,&writefds) &&
      !(rdfd>=0 && FD_ISSET(rdfd,&readfds)) &&
      !used) {
    wrbuf_report("stopping");
    writing= 0;
    FD_CLR(wrfd,&writefds);
  }

  if (rdfd>=0 && FD_ISSET(rdfd,&readfds)) {
    r= read(rdfd,rp,min(buffersize-1-used,buf+buffersize-rp));
    if (!r) {
      seeneof=1; writing=1;
      wrbuf_report("seeneof");
    } else if (r<0) {
      if (!(errno == EAGAIN || errno == EINTR)) { perror("read"); exit(1); }
    } else {
      used+= r;
      rp+= r;
      if (rp == buf+buffersize) rp=buf;
    }
    if (used > waitfill) {
      if (!writing) wrbuf_report("starting");
      writing=1;
    }
  }

  if (FD_ISSET(wrfd,&writefds) && used) {
    r= write(wrfd,wp,min(used,buf+buffersize-wp));
    if (r<=0) {
      if (!(errno == EAGAIN || errno == EINTR)) { perror("write"); exit(1); }
    } else {
      used-= r;
      wp+= r;
      if (wp == buf+buffersize) wp=buf;
    }
  }
}
