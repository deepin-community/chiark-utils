/* common stuff for cgi-fcgi-interp and prefork-interp */
/*
 * Copyright 2016-2022 Ian Jackson and contributors to chiark-utils
 * SPDX-License-Identifier: GPL-3.0-or-later
 * There is NO WARRANTY.
 */

#ifndef PREFORK_H
#define PREFORK_H

#include "common.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <stdbool.h>
#include <assert.h>
#include <limits.h>

#include <sys/types.h>
#include <sys/stat.h>
#include <sys/utsname.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/file.h>
#include <unistd.h>
#include <fcntl.h>
#include <pwd.h>
#include <time.h>
#include <signal.h>
#include <sys/wait.h>
#include <syslog.h>
	
#include <nettle/sha.h>

#include "myopt.h"

#define MINHEXHASH 33

extern const char *interp, *ident, *script, *socket_path, *lock_path;
extern bool logging;
extern struct sha256_ctx identsc;
extern const char *run_base;

extern const char our_name[];

bool find_run_base_var_run(void);
void find_socket_path(void);

int acquire_lock(void);
int flock_file(const char *lock_path);

extern const struct cmdinfo cmdinfos[];
#define PREFORK_CMDINFOS \
  { "help",   0, .call=of_help                                         }, \
  { 0, 'g',   1,                    .sassignto= &ident                 }, \
  { 0, 'G',   1, .call= off_ident_addstring                            }, \
  { 0, 'E',   1, .call= off_ident_addenv                               },

void process_opts(const char *const **argv_io);

void vmsgcore(int estatus, int errnoval, const char *fmt, va_list al);

#define DEF_MSG(func, attrs, estatus, errnoval, after)	\
  static void func(const char *fmt, ...)		\
    __attribute__((unused, format(printf,1,2))) attrs;	\
  static void func(const char *fmt, ...) {		\
    va_list al;						\
    va_start(al,fmt);					\
    vmsgcore(estatus,errnoval,fmt,al);			\
    after						\
  }

DEF_MSG(warninge, /*empty*/, 0, errno, { });
DEF_MSG(warning , /*empty*/, 0, -1,    { });

#define DEF_DIE(func, errnoval) \
  DEF_MSG(func, __attribute__((noreturn)), 127, errnoval, { abort(); })

DEF_DIE(diee, errno)
DEF_DIE(die,  -1)

#define MAX_OPTS 5

void fusagemessage(FILE *f);
void usagemessage(void);
void of_help(const struct cmdinfo *ci, const char *val);
void of_iassign(const struct cmdinfo *ci, const char *val);
void ident_addinit(void);
bool stabs_same_inode(struct stat *a, struct stat *b);
void ident_addstring(char key, const char *string);

void off_ident_addstring(const struct cmdinfo *ci, const char *name);
void off_ident_addenv(const struct cmdinfo *ci, const char *name);

void ident_add_key_byte(char key);

#define IDENT_ADD_OBJ(key, obj) do{				\
    ident_add_key_byte(key);					\
    sha256_update(&identsc, sizeof((obj)), (void*)&obj);	\
  }while(0)

#endif /*PREFORK_H*/
