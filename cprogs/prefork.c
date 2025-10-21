/* common stuff for cgi-fcgi-interp and prefork-interp */
/*
 * Copyright 2016-2022 Ian Jackson and contributors to chiark-utils
 * SPDX-License-Identifier: GPL-3.0-or-later
 * There is NO WARRANTY.
 */

#include "prefork.h"

const char *interp, *ident, *script, *socket_path, *lock_path;
bool logging;
struct sha256_ctx identsc;
const char *run_base;

static uid_t us;
static const char *run_base_mkdir_p;

void common_diee(const char *m) { diee("%s", m); }
void common_die (const char *m) { die ("%s", m); }

void vmsgcore(int estatus, int errnoval, const char *fmt, va_list al) {
  int r;

  if (logging) {
    const char *fmt_use = fmt;
    char *fmt_free = 0;
    if (errnoval!=-1) {
      r = asprintf(&fmt_free, "%s: %%m", fmt);
      if (r) {
	fmt_free = 0;
      } else {
	fmt_use = fmt_free;
      }
    }
    vsyslog(LOG_ERR, fmt_use, al);
    free(fmt_free);
  } else {
    fprintf(stderr, "%s: ", our_name);
    vfprintf(stderr,fmt,al);
    if (errnoval!=-1) fprintf(stderr,": %s",strerror(errnoval));
    fputc('\n',stderr);
  }
  if (estatus) exit(estatus);
}

void usagemessage(void) { fusagemessage(stderr); }

void of_help(const struct cmdinfo *ci, const char *val) {
  fusagemessage(stdout);
  if (ferror(stdout)) diee("write usage message to stdout");
  exit(0);
}

void of_iassign(const struct cmdinfo *ci, const char *val) {
  long v;
  char *ep;
  errno= 0; v= strtol(val,&ep,10);
  if (!*val || *ep || errno || v<INT_MIN || v>INT_MAX)
    badusage("bad integer argument `%s' for --%s",val,ci->olong);
  *ci->iassignto = v;
}

void ident_add_key_byte(char key) {
  sha256_update(&identsc,1,&key);
}

void ident_addstring(char key, const char *string) {
  ident_add_key_byte(key);
  sha256_update(&identsc,strlen(string)+1,string);
}

void off_ident_addstring(const struct cmdinfo *ci, const char *string) {
  ident_addstring('G', string);
}

void off_ident_addenv(const struct cmdinfo *ci, const char *name) {
  ident_addstring('E', name);
  const char *val = getenv(name);
  if (val) {
    ident_addstring('v', val);
  } else {
    ident_add_key_byte(0);
  }
}

bool stabs_same_inode(struct stat *a, struct stat *b) {
  return (a->st_dev == b->st_dev &&
	  a->st_ino == b->st_ino);
}

bool find_run_base_var_run(void) {
  struct stat stab;
  char *try;
  int r;

  try = m_asprintf("%s/%lu", "/var/run/user", us);
  r = lstat(try, &stab);
  if (r<0) {
    if (errno == ENOENT ||
	errno == ENOTDIR ||
	errno == EACCES ||
	errno == EPERM)
      return 0; /* oh well */
    diee("stat /var/run/user/UID");
  }
  if (!S_ISDIR(stab.st_mode)) {
    warning("%s not a directory, falling back to ~\n", try);
    return 0;
  }
  if (stab.st_uid != us) {
    warning("%s not owned by uid %lu, falling back to ~\n", try,
	    (unsigned long)us);
    return 0;
  }
  if (stab.st_mode & 0077) {
    warning("%s writeable by group or other, falling back to ~\n", try);
    return 0;
  }
  run_base = m_asprintf("%s/%s", try, our_name);
  return 1;
}

static bool find_run_base_home(void) {
  struct passwd *pw;
  struct utsname ut;
  char *dot, *try;
  int r;

  pw = getpwuid(us);  if (!pw) diee("getpwent(uid)");

  r = uname(&ut);   if (r) diee("uname(2)");
  dot = strchr(ut.nodename, '.');
  if (dot) *dot = 0;
  if (sizeof(ut.nodename) > 32)
    ut.nodename[32] = 0;

  run_base_mkdir_p = m_asprintf("%s/.%s", pw->pw_dir, our_name);
  try = m_asprintf("%s/%s", run_base_mkdir_p, ut.nodename);
  run_base = try;
  return 1;
}

void find_socket_path(void) {
  struct sockaddr_un sun;
  int r;

  us = getuid();  if (us==(uid_t)-1) diee("getuid");

  find_run_base_var_run() ||
    find_run_base_home() ||
    (abort(),0);

  int maxidentlen = sizeof(sun.sun_path) - strlen(run_base) - 10 - 2;

  if (!ident) {
    if (maxidentlen < MINHEXHASH)
      die("base directory `%s'"
	  " leaves only %d characters for id hash"
	  " which is too little (<%d)",
	  run_base, maxidentlen, MINHEXHASH);

    int identlen = maxidentlen > 64 ? 64 : maxidentlen;
    char *hexident = xmalloc(identlen + 2);
    unsigned char bbuf[32];
    int i;

    ident_addstring('i', interp);
    if (script)
      ident_addstring('s', script);
    sha256_digest(&identsc,sizeof(bbuf),bbuf);

    for (i=0; i<identlen; i += 2)
      sprintf(hexident+i, "%02x", bbuf[i/2]);

    hexident[identlen] = 0;
    ident = hexident;
  }

  if (strlen(ident) > maxidentlen)
    die("base directory `%s' plus ident `%s' too long"
	" (with spare) for socket (max ident %d)\n",
	run_base, ident, maxidentlen);

  r = mkdir(run_base, 0700);
  if (r && errno==ENOENT && run_base_mkdir_p) {
    r = mkdir(run_base_mkdir_p, 0700);
    if (r) diee("mkdir %s (since %s was ENOENT)",run_base_mkdir_p,run_base);
    r = mkdir(run_base, 0700);
  }
  if (r) {
    if (!(errno == EEXIST))
      diee("mkdir %s",run_base);
  }

  socket_path = m_asprintf("%s/s%s",run_base,ident);
}  

// Returns fd
int flock_file(const char *lock_path) {
  int r;
  int lockfd = -1;
  struct stat stab_fd;
  struct stat stab_path;

  for (;;) {
    if (lockfd >= 0) { close(lockfd); lockfd = -1; }

    lockfd = open(lock_path, O_CREAT|O_RDWR, 0600);
    if (lockfd<0) diee("create lock (%s)", lock_path);

    r = flock(lockfd, LOCK_EX);
    if (r && errno == EINTR) continue;
    if (r) diee("lock lock (%s)", lock_path);

    r = fstat(lockfd, &stab_fd);
    if (r) diee("fstat locked lock");

    r = stat(lock_path, &stab_path);
    if (!r) {
      if (stabs_same_inode(&stab_path, &stab_fd)) break;
    } else {
      if (!(errno == ENOENT)) diee("re-stat locked lock (%s)", lock_path);
    }
  }

  return lockfd;
}

// Returns fd
int acquire_lock(void) {
  lock_path = m_asprintf("%s/l%s",run_base,ident);
  return flock_file(lock_path);
}

static void shbang_opts(const char *const **argv_io,
			const struct cmdinfo *cmdinfos) {
  myopt(argv_io, cmdinfos);

  interp = *(*argv_io)++;
  if (!interp) badusage("need interpreter argument");
}

void process_opts(const char *const **argv_io) {
  const char *smashedopt;

  sha256_init(&identsc);
  ident_addinit();

  if ((*argv_io)[0] &&
      (smashedopt = (*argv_io)[1]) &&
      smashedopt[0]=='-' &&
      (strchr(smashedopt,' ') || strchr(smashedopt,','))) {
    /* single argument containg all the options and <interp> */
    *argv_io += 2; /* eat argv[0] and smashedopt */
    const char *split_args[MAX_OPTS+1];
    int split_argc = 0;
    split_args[split_argc++] = (*argv_io)[0];
    for (;;) {
      if (split_argc >= MAX_OPTS) die("too many options in combined arg");
      split_args[split_argc++] = smashedopt;
      if (smashedopt[0] != '-') /* never true on first iteration */
	break;
      char *delim = strchr(smashedopt,' ');
      if (!delim) delim = strchr(smashedopt,',');
      if (!delim) badusage("combined arg lacks <interpreter>");
      *delim = 0;
      smashedopt = delim+1;
    }
    assert(split_argc <= MAX_OPTS);
    split_args[split_argc++] = 0;

    const char *const *split_argv = split_args;

    shbang_opts(&split_argv, cmdinfos);
    /* sets interp */

    if (!**argv_io)
      badusage("no script argument (expected after combined #! options)");
  } else {
    shbang_opts(argv_io, cmdinfos);
  }

  if (**argv_io)
    script = *(*argv_io)++;
}
