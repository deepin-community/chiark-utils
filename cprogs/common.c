/*
 * common.[ch] - C helpers common to the whole of chiark-utils
 *
 * Copyright 2007 Canonical Ltd
 * Copyright 2016 Canonical Ltd
 *
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public
 * License along with this file; if not, consult the Free Software
 * Foundation's website at www.fsf.org, or the GNU Project website at
 * www.gnu.org.
 *
 */

#include "common.h"

char *m_vasprintf(const char *fmt, va_list al) {
  char *s;  int r;
  r= vasprintf(&s,fmt,al);
  if (r==-1) common_diee("vasprintf");
  return s;
}
char *m_asprintf(const char *fmt, ...) {
  char *s;  va_list al;
  va_start(al,fmt); s= m_vasprintf(fmt,al); va_end(al);
  return s;
}

void *xmalloc(size_t sz) {
  void *r= malloc(sz);
  if (!r) common_diee("malloc");
  return r;
}

void nonblock(int fd) {
  int r;
  r= fcntl(fd,F_GETFL);  if (r<0) common_diee("nonblock fcntl F_GETFL");
  r |= O_NONBLOCK;
  r= fcntl(fd,F_SETFL,r);  if (r<0) common_diee("nonblock fcntl F_GETFL");
}
