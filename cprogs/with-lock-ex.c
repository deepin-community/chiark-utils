/*
 * File locker
 *
 * Usage:
 *  with-lock-ex -<mode> [-t <secs>] <lockfile> <command> <args>...
 *  with-lock-ex -l      <lockfile>
 *
 * modes are
 *  w    wait for the lock
 *  f    fail if the lock cannot be acquired
 *  q    silently do nothing if the lock cannot be acquired
 *  l    show who is waiting (print "none" or "read <pid>"
 *         or "write <pid>"; lockfile opened for reading;
 *         no command may be specified)
 *
 * If -t is specified, then with-lock-ex will wait for up to <secs>
 * seconds to acquire the lock, and then fail or silently do nothing
 * (depending on whether -f or -q is specified). You cannot specify
 * a timeout for modes l or w.
 *
 * with-lock-ex will open and lock the lockfile for writing and
 * then feed the remainder of its arguments to exec(2); when
 * that process terminates the fd will be closed and the file
 * unlocked automatically by the kernel.
 *
 * If invoked as with-lock, behaves like with-lock-ex -f (for backward
 * compatibility with an earlier version).
 *
 * This file written by me, Ian Jackson, in 1993, 1994, 1995, 1996,
 * 1998, 1999, 2016.
 *
 * Copyright 1993-2016 Ian Jackson in some jurisdictions
 * Copyright 2017      Ian Jackson in all jurisdictions
 * Copyright 2017      Genome Research Ltd
 *
 * (MIT licence:)
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * SOFTWARE IN THE PUBLIC INTEREST, INC. BE LIABLE FOR ANY CLAIM, DAMAGES OR
 * OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
 * ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 */

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <sys/stat.h>
#include <limits.h>
#include <signal.h>
#include <sys/time.h>

static const char *cmd;

static void fail(const char *why) __attribute__((noreturn));

static void fail(const char *why) {
  fprintf(stderr,"with-lock-ex %s: %s: %s\n",cmd,why,strerror(errno));
  exit(255);
}

static void badusage(void) __attribute__((noreturn));

static void badusage(void) {
    fputs("usage: with-lock-ex -w|-q|-f [-t <secs>] <lockfile> <command> <args>...\n"
	  "       with-lock-ex -l       <lockfile>\n"
	  "       with-lock-ex          <lockfile> <command> <args>...\n",
	  stderr);
    exit(255);
}

static int mode;

/* This signal handler uses unsafe functions, so MUST NOT be callable
 * during an unsafe function, as that is Undefined Behaviour
 */
static void alrm_handler(int signum) {
  if (mode=='q') {
    exit(0);
  } else {
    fprintf(stderr,
	    "with-lock-ex %s: timer expired while trying to acquire lock\n",
	    cmd);
    exit(255);
  }
}

int main(int argc, char **argv) {
  int fd, um, c, r;
  struct stat stab, fstab;
  long cloexec, secs=0;
  struct flock fl;
  char *endptr;
  sigset_t sigs, oldsigs;
  struct sigaction siga;
  struct itimerval itv;

  mode= 'x';
  while ((c= getopt(argc,argv,"+wfqlt:")) != -1) {
    switch(c) {
    case 'l':
    case 'w':
    case 'f':
    case 'q':
      if (mode != 'x') badusage();
      mode= c;
      break;
    case 't':
      errno = 0;
      secs = strtol(optarg, &endptr, 0);
      if (*endptr || endptr==optarg || errno==ERANGE)
	fail("parsing timeout value");
      if (secs < 0) {
	fprintf(stderr,"timeout value must be >=0\n");
	exit(255);
      }
      break;
    default:
      badusage();
    }
  }

  if (secs && (mode=='l' || mode=='w')) {
    fputs("-t only allowed with -q or -f.\n", stderr);
    exit(255);
  }

  argv += optind-1; argc -= optind-1;
  if (argc < 2) badusage();

  if (secs) {
    if (sigemptyset(&sigs)) fail("Initialising signal set");
    if (sigaddset(&sigs,SIGALRM)) fail("Adding SIGALRM to signal set");
    if (sigprocmask(SIG_BLOCK,&sigs,&oldsigs)) fail("Blocking SIGALRM");
    memset(&siga,0,sizeof(siga));
    siga.sa_handler=alrm_handler;
    if (sigaction(SIGALRM,&siga,NULL)) fail("Installing SIGALRM handler");
    memset(&itv,0,sizeof(itv));
    itv.it_value.tv_sec=secs;
    if (setitimer(ITIMER_REAL,&itv,NULL)) fail("Setting timer");
  }

  cmd= argv[2];
  um= umask(0777); if (um==-1) fail("find umask");
  if (umask(um)==-1) fail("reset umask");

  for (;;) {

    int openmode = mode=='l' ? O_RDONLY : O_RDWR|O_CREAT;

    fd= open(argv[1],openmode,0666&~(um|((um&0222)<<1)));
    if (fd<0) fail(argv[1]);
  
    for (;;) {
      fl.l_type= F_WRLCK;
      fl.l_whence= SEEK_SET;
      fl.l_start= 0;
      fl.l_len= mode=='l' ? 0 : 1;
      if (secs) sigprocmask(SIG_UNBLOCK,&sigs,NULL);
      r = fcntl(fd,
		mode=='l' ? F_GETLK :
		mode=='w' || secs > 0 ? F_SETLKW :
		F_SETLK,
		&fl);
      if (secs) sigprocmask(SIG_BLOCK,&sigs,NULL);
      if (!r) {
	break;
      }
      if (mode=='q' &&
	  (errno == EAGAIN || errno == EWOULDBLOCK || errno == EBUSY))
	exit(0);
      if (errno != EINTR) fail("could not acquire lock");
    }
    if (mode=='l') {
      if (fl.l_pid) {
	printf("%s %lu\n",
	       fl.l_type == F_WRLCK ? "write" :
	       fl.l_type == F_RDLCK ? "read" : "unknown",
	       (unsigned long)fl.l_pid);
      } else {
	printf("none\n");
      }
      if (ferror(stdout)) fail("print to stdout\n");
      exit(0);
    }

    if (fstat(fd, &fstab)) fail("could not fstat lock fd");
    if (stat(argv[1], &stab)) {
      if (errno != ENOENT) fail("could not stat lockfile");
    } else {
      if (stab.st_dev == fstab.st_dev &&
	  stab.st_ino == fstab.st_ino) break;
    }
    close(fd);
  }

  if (secs) {
    itv.it_value.tv_sec=0;
    if (setitimer(ITIMER_REAL,&itv,NULL)) fail("Clearing timer");
    sigprocmask(SIG_SETMASK,&oldsigs,NULL);
    siga.sa_handler=SIG_DFL;
    sigaction(SIGALRM,&siga,NULL);
  }

  cloexec= fcntl(fd, F_GETFD); if (cloexec==-1) fail("fcntl F_GETFD");
  cloexec &= ~1;
  if (fcntl(fd, F_SETFD, cloexec)==-1) fail("fcntl F_SETFD");

  execvp(cmd,argv+2);
  fail("unable to execute command");
}
