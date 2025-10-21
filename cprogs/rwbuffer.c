/*
 * rwbuffer.c
 * common definitions for readbuffer/writebuffer
 *
 * readbuffer and writebuffer are:
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

#ifndef RWBUFFER_SIZE_MB_DEF
#define RWBUFFER_SIZE_MB_DEF 16
#endif

#ifndef RWBUFFER_SIZE_MB_MAX
#define RWBUFFER_SIZE_MB_MAX 512
#endif

unsigned char *buf, *wp, *rp;
int used, seeneof, maxselfd;
size_t buffersize= RWBUFFER_SIZE_MB_DEF*1024*1024;
fd_set readfds;
fd_set writefds;

static int opt_mlock=0;

int min(int a, int b) { return a<=b ? a : b; }

static void usage(FILE *f) {
  if (fprintf(f,"usage: %s [--mlock] [<megabytes>]\n",progname) < 0)
    { perror("print usage"); exit(16); }
}

static void usageerr(const char *what) {
  fprintf(stderr,"%s: bad usage: %s\n",progname,what);
  usage(stderr);
  exit(12);
}

void nonblock(int fd, int yesno) {
  int r;
  r= fcntl(fd,F_GETFL,0); if (r == -1) { perror("fcntl getfl"); exit(8); }
  if (yesno) r |= O_NDELAY;
  else r &= ~O_NDELAY;
  if (fcntl(fd,F_SETFL,r) == -1) { perror("fcntl setfl"); exit(8); }
}

static void unnonblock(void) {
  nonblock(0,0); nonblock(1,0);
}

void startupcore(void) {
  buf= xmalloc(buffersize);

  if (opt_mlock) {
    if (mlock(buf,buffersize)) { perror("mlock"); exit(2); }
  }

  used=0; wp=rp=buf; seeneof=0;
  if (atexit(unnonblock)) { perror("atexit"); exit(16); }
}

void startup(const char *const *argv) {
  const char *arg;
  char *ep;
  int shift=-1;
  
  assert(argv[0]);
  
  while ((arg= *++argv)) {
    if (!strcmp(arg,"--mlock")) {
      opt_mlock= 1;
    } else if (isdigit((unsigned char)arg[0])) {
      buffersize= strtoul(arg,&ep,0);
      if (ep[0] && ep[1]) usageerr("buffer size spec. invalid");
      switch (ep[0]) {
      case 0: case 'm':  shift= 20;  break;
      case 'k':          shift= 10;  break;
      case 'b':          shift= 0;   break;
      default: usageerr("buffer size unit unknown");
      }
      if (buffersize > ((RWBUFFER_SIZE_MB_MAX << 20) >> shift))
	usageerr("buffer size too big");
      buffersize <<= shift;
    } else {
      usageerr("invalid option");
    }
  }

  startupcore();
  nonblock(0,1); nonblock(1,1);
}

void *xmalloc(size_t sz) {
  void *r= malloc(sz); if (!r) { perror("malloc"); exit(6); }; return r;
}

void callselect(void) {
  int r;
  
  for (;;) {
    r= select(maxselfd,&readfds,&writefds,0,0);
    if (r != -1) return;
    if (errno != EINTR) {
      perror("select"); exit(4);
    }
  }
}
