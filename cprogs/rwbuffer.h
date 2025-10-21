/*
 * rwbuffer.h
 * common declarations for readbuffer/writebuffer
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

#ifndef RWBUFFER_H
#define RWBUFFER_H

#include <sys/time.h>
#include <sys/types.h>
#include <unistd.h>
#include <fcntl.h>
#include <stdio.h>
#include <assert.h>
#include <ctype.h>
#include <stdlib.h>
#include <errno.h>
#include <time.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/un.h>
#include <netdb.h>

#include "dlist.h"


int min(int a, int b);
void callselect(void);
void startup(const char *const *argv);
void startupcore(void);
void *xmalloc(size_t sz);
void nonblock(int fd, int yesno);

extern const char *progname; /* must be defined by main .c file */

extern unsigned char *buf, *wp, *rp;
extern int used, seeneof, maxselfd;
extern size_t buffersize;
extern fd_set readfds;
extern fd_set writefds;


void wrbufcore_startup(void);
void wrbufcore_prepselect(int rdfd, int wrfd);
void wrbufcore_afterselect(int rdfd, int wrfd);
void fdsetset(int fd, fd_set *set);
void wrbuf_report(const char *m);


#endif /*RWBUFFER_H*/
