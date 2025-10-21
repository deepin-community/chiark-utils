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
 * Should be included early so that _GNU_SOURCE takes effect.
 */

#ifndef COMMON_H
#define COMMON_H

#define _GNU_SOURCE

#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>

char *m_vasprintf(const char *fmt, va_list al);
char *m_asprintf(const char *fmt, ...);

/* to be provided by program: */
void common_die(const char *what);
void common_diee(const char *what); /* prints errno */
void nonblock(int fd);

void *xmalloc(size_t sz);

#define FILLZERO(object) (memset((&object),0,sizeof(object)))

extern char **environ; // no header file for this, srsly!

#endif /*COMMON_H*/
