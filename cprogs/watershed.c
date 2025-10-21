/*
 * watershed - an auxiliary verb for optimising away
 *             unnecessary runs of idempotent commands
 *
 * watershed is Copyright 2007 Canonical Ltd
 * written by Ian Jackson <ian@davenant.greenend.org.uk>
 * and this version now maintained as part of chiark-utils
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
/*
 *  NB a different fork of this code exists in Ubuntu's udev.
 */
/*
 *
 * usage: watershed [<options>] <command> [<arg>...]
 *
 * options:
 *   -d|--state-dir <state-dir>
 *        default is /var/run/watershed for uid 0
 *                   $HOME/.watershed for others
 *   -i|--command-id <command-id>
 *
 * files used:
 *    <state-dir>/<command-id>.lock            lockfile
 *    <state-dir>/<command-id>.cohort          cohort
 *
 * default <command-id> is
 *    hex(sha256(argv[0]+'\0' + argv[1]+'\0' ... argv[argc-1]+'\0')
 *    '='
 *    mangled argv[0] (all chars [^-+_0-9A-Za-z] replaced with ?
 *                     and max 32 chars)
 *
 * exit status:
 *  127      - something went wrong, or process died with some other signal
 *  SIGPIPE  - process died with SIGPIPE
 *  x        - process called _exit(x)
 *
 * stdin/stdout/stderr:
 *
 *  If watershed exits 127 due to some unexpected problem, a message
 *  is printed to stderr explaining why (obviously).
 *
 *  If a watershed invocation ends up running the process, the process
 *  simply inherits stdin/out/err.  Otherwise stdin/stdout are not used.
 *
 *  If the process run for us by another invocation of watershed exits
 *  zero, or watershed die with the same signal as the process
 *  (currently just SIGPIPE), nothing is printed to stderr.  Otherwise
 *  (ie, failure of the actual process, in another invocation),
 *  watershed prints a description of the wait status to stderr, much
 *  as the shell might.
 *
 */
/*
 * gcc -Wall -Wwrite-strings -Wmissing-prototypes watershed.c -o watershed /usr/lib/libnettle.a
 */
/*
 *
 * Theory:
 *
 *  We consider only invocations with a specific command id (and state
 *  directory), since other invocations are completely independent by
 *  virtue of having different state file pathnames and thus different
 *  state files.  Normally, a command id corresponds to invocations
 *  with a particular set of command line arguments and a state
 *  directory corresponds to a particular euid; environment variable
 *  settings and other inherited process properties are disregarded.
 *
 *  A `cohort' is a set of invocations which can be coalesced into one
 *  run of the command.  For each cohort there is a file, the cohort
 *  file (which may not yet exist, may exist and have a name, or may
 *  be unliked).
 *
 *  An `invocation' is an invocation of the `watershed' program.  A
 *  `process' is an invocation of the requested command.
 *
 *  There is always one current cohort, in one of the following
 *  two states:
 *
 *   * Empty
 *     No invocations are in this cohort yet.
 *     The cohort filename is ENOENT.
 *     This is the initial state for a cohort, and the legal next
 *     state is Accumulating.
 *
 *   * Accumulating
 *     The process for this run has not yet started, so that new
 *     invocations arriving would be satisfied if this cohort were to
 *     run.
 *     The cohort filename refers to this cohort's file.
 *     The legal next state for the cohort is Ready.
 *
 *  Additionally, there may be older cohorts in the following states:
 *
 *   * Ready
 *     The command for this cohort has not yet been run.
 *     The cohort file has no name and is empty.
 *     Only one cohort, the lockholder's, may be in this state.
 *     The next legal states are Running, or exceptionally Forgotten
 *     (if the lockholder crashes and is the only invocation in the
 *     cohort).
 *
 *   * Running
 *     The lockholder is running the command for this cohort.
 *     This state is identical to Ready from the point of view
 *     of all invocations except the lockholder.
 *     The legal next states are Done (the usual case), or (if the
 *     lockholder crashes) Ready or Forgotten.
 *
 *   * Done
 *     The main process for this run has finished.
 *     The cohort file has no name and contains sizeof(int)
 *     bytes, the `status' value from waitpid.
 *     The legal next state is Forgotten.
 *
 *   * Forgotten
 *     All invocations have finished and the cohort file no longer
 *     exists.  This is the final state.
 *
 *  Only the lockholder may move a cohort between states, except that
 *  any invocation may make the current Empty cohort become
 *  Accumulating, and that the kernel will automatically move a cohort
 *  from Running to Ready or from Done to Forgotten, when appropriate.
 *
 * 
 * Algorithm:
 *
 *   1. Open the cohort file (O_CREAT|O_RDWR)   so our cohort is
 *                                                 Accumulating/Ready/
 *                                                    Running/Done
 *                              
 *   2. Acquire lock (see below)                so lockholder's cohort is
 *				                   Accumulating/Ready/Done
 *   3. fstat the open cohort file
 *         If it is nonempty:                      Done
 *          Read status from it and exit.
 *         Otherwise, if nonzero link count:       Accumulating
 *          Unlink the cohort filename
 *         Otherwise:                              Ready
 *
 *   4. Fork and run the command                   Running
 *       and wait for it
 *
 *   5. Write the wait status to the cohort file   Done
 *
 *                      
 *   6. Release the lock                        so we are no longer lockholder
 *                                              but our cohort is still
 *                                                 Done
 *
 *   8. Exit                                       Done/Forgotten
 *
 *  If an invocation crashes (ie, if watershed itself fails, rather
 *  than if the command does) then that invocation's caller will be
 *  informed of the error.
 *
 *  If the lockholder crashes with the cohort in:
 *
 *     Accumulating:
 *       The cohort remains in Accumulating and another invocation can
 *       become the lockholder.  If there are never any other
 *       invocations then the lockfile and cohort file will not be
 *       cleaned up (see below).
 *
 *     Running/Ready:
 *       The cohort goes from Running back to Ready (see above) and
 *       another invocation in the same cohort will become the
 *       lockholder and run it.  If there is no other invocation in
 *       the cohort the cohort goes to Forgotten although the lockfile
 *       will not be cleaned up - see below.
 *
 *     Done:
 *       If there are no more invocations, the cohort is Forgotten but
 *       the lockfile is not cleaned up.
 *
 * Lockfile:
 *
 *  There is one lock for all cohorts.  The lockholder is the
 *  invocation which holds the fcntl lock on the file whose name is
 *  the lockfile.  The lockholder (and no-one else) may unlink the
 *  lockfile.
 *
 *  To acquire the lock:
 *
 *   1. Open the lockfile (O_CREAT|O_RDWR)
 *   2. Acquire fcntl lock (F_SETLKW)
 *   3. fstat the open lockfile and stat the lockfile filenmae
 *      If inode numbers disagree, close lockfile and start
 *      again from the beginning.
 *
 *  To release the lock, unlink the lockfile and then either close it
 *  or exit.  Crashing will also release the lock but leave the
 *  lockfile lying around (which is slightly untidy but not
 *  incorrect); if this is a problem a cleanup task could periodically
 *  acquire and release the lock for each lockfile found:
 *
 * Cleanup:
 *
 *  As described above and below, stale cohort files and lockfiles can
 *  result from invocations which crashed if the same command is never
 *  run again.  Such cohorts are always in Empty or Accumulating.
 *
 *  If it became necessary to clean up stale cohort files and
 *  lockfiles resulting from crashes, the following algorithm should
 *  be executed for each lockfile found, as a cleanup task:
 *
 *   1. Acquire the lock.
 *      This makes us the lockholder.           and the current cohort is in
 *                                                 Empty/Accumulating
 *
 *                                              so now that cohort is
 *   2. Unlink the cohort file, ignoring ENOENT.   Ready/Forgotten
 *   3. Release the lock.                          Ready/Forgotten
 *   4. Exit.                                      Ready/Forgotten
 *
 *  This consists only of legal transitions, so if current cohort
 *  wasn't stale, it will have been moved to Ready and some other
 *  invocation in this cohort will become the lockholder and as normal
 *  from step 4 of the main algorithm.  If the cohort was stale it
 *  will go to Forgotten straight away.
 *
 *  A suitable cleanup script, on a system with with-lock-ex, is: */
 //     #!/bin/sh
 //     set -e
 //     if [ $# != 1 ]; echo >&2 'usage: cleanup <statedir>'; exit 1; fi
 //     cd "$1"
 //     for f in ./*.lock; do
 //       with-lock-ex -w rm -f "${f%.lock}.cohort"
 //     done
/*
 */

#include "common.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <stdarg.h>
#include <ctype.h>
#include <assert.h>

#include <sys/types.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include <fcntl.h>
#include <getopt.h>
#include <locale.h>
#include <libintl.h>

#include <nettle/sha.h>

#define die  common_die
#define diee common_diee

static const struct option os[]= {
  { "--state-dir", 1,0,'d' },
  { "--command-id",1,0,'i' },
  { "--help",      0,0,'h' },
  { 0 }
};

static const char *state_dir, *command_id, *command;
static const char *lock_path, *cohort_path;

static int cohort_fd, lock_fd;


#define _(x) gettext(x)

#define NOEINTR_TYPED(type,assign) do{			\
    while ((assign)==(type)-1 && errno==EINTR) {}	\
  }while(0)

#define NOEINTR(assign) \
    NOEINTR_TYPED(int,(assign))

#define CHECKED(value,what) do{			\
    NOEINTR(r= (value));			\
    if (r<0) diee((what));			\
  }while(0)


static void printusage(FILE *f) {
  fputs(_("usage: watershed [<options>] <command>...\n"
	  "options:\n"
	  "   -d|--state-dir <directory>\n"
	  "   -i|--command-id <id>\n"
	  "   -h|--help\n"
	  "see /usr/share/doc/chiark-utils-bin/watershed.txt\n"),
	  f);
}
static void badusage(void) {
  printusage(stderr);
  exit(127);
}
void die(const char *m) {
  fprintf(stderr,_("watershed: error: %s\n"), m);
  exit(127);
}
void diee(const char *m) {
  fprintf(stderr,_("watershed: error: %s failed: %s\n"), m, strerror(errno));
  exit(127);
}
static void dieep(const char *action, const char *path) {
  fprintf(stderr,_("watershed: error: could not %s `%s': %s\n"),
	  action, path, strerror(errno));
  exit(127);
}

static void parse_args(int argc, char *const *argv) {
  int o;
  for (;;) {
    o= getopt_long(argc, argv, "+d:i:h", os,0);
    if (o==-1) break;
    switch (o) {
    case 'd': state_dir= optarg; break;
    case 'i': command_id= optarg; break;
    case 'h': printusage(stdout); exit(0); break;
    default: badusage();
    }
  }
  command= argv[optind];
  if (!command) badusage();
  if (!state_dir) state_dir= getenv("WATERSHED_STATEDIR");
  if (!state_dir) {
    uid_t u= geteuid();  if (u==(uid_t)-1) diee("getuid");
    if (u) {
      const char *home= getenv("HOME");
      if (!home) die(_("HOME not set, no --state-dir option"
		       " supplied, not root"));
      state_dir= m_asprintf("%s/.watershed", home);
    } else {
      state_dir= "/var/run/watershed";
    }
  }
  if (!command_id) {
    char *const *ap;
    struct sha256_ctx sc;
    unsigned char dbuf[SHA256_DIGEST_SIZE], *p;
    char *construct, *q;
    int i, c;
    
    sha256_init(&sc);
    for (ap= argv+optind; *ap; ap++) sha256_update(&sc,strlen(*ap)+1,*ap);
    sha256_digest(&sc,sizeof(dbuf),dbuf);

    construct= m_asprintf("%*s#%.32s", (int)sizeof(dbuf)*2,"", command);
    for (i=sizeof(dbuf), p=dbuf, q=construct; i; i--,p++,q+=2)
      sprintf(q,"%02x",*p);
    *q++= '=';
    while ((c=*q++)) {
      if (!(c=='-' || c=='+' || c=='_' || isalnum((unsigned char)c)))
	q[-1]= '?';
    }
    command_id= construct;
  }

  lock_path= m_asprintf("%s/%s.lock", state_dir, command_id);
  cohort_path= m_asprintf("%s/%s.cohort", state_dir, command_id);
}

static void acquire_lock(void) {
  struct stat current_stab, our_stab;
  struct flock fl;
  int r;

  for (;;) {
    NOEINTR( lock_fd= open(lock_path, O_CREAT|O_RDWR, 0600) );
    if (lock_fd<0) diee("open lock");

    memset(&fl,0,sizeof(fl));
    fl.l_type= F_WRLCK;
    fl.l_whence= SEEK_SET;
    CHECKED( fcntl(lock_fd, F_SETLKW, &fl), "acquire lock" );
    
    CHECKED( fstat(lock_fd, &our_stab), "fstat our lock");

    NOEINTR( r= stat(lock_path, &current_stab) );
    if (!r &&
	our_stab.st_ino == current_stab.st_ino &&
	our_stab.st_dev == current_stab.st_dev) break;
    if (r && errno!=ENOENT) diee("fstat current lock");
    
    close(lock_fd);
  }
}
static void release_lock(void) {
  int r;
  CHECKED( unlink(lock_path), "unlink lock");
}

static void report(int status) {
  int v;
  if (WIFEXITED(status)) {
    v= WEXITSTATUS(status);
    if (v) fprintf(stderr,_("watershed: `%s' failed with error exit status %d"
			    " (in another invocation)\n"), command, v);
    exit(status);
  }
  if (WIFSIGNALED(status)) {
    v= WTERMSIG(status); assert(v);
    if (v == SIGPIPE) raise(v);
    fprintf(stderr,
	    WCOREDUMP(status)
	    ? _("watershed: `%s' died due to fatal signal %s (core dumped)\n")
	    : _("watershed: `%s' died due to fatal signal %s\n"),
	    command, strsignal(v));
  } else {
    fprintf(stderr, _("watershed: `%s' failed with"
		      " crazy wait status 0x%x\n"), command, status);
  }
  exit(127);
}

int main(int argc, char *const *argv) {
  int status, r, dir_created=0, l;
  unsigned char *p;
  struct stat cohort_stab;
  pid_t c, c2;
  
  setlocale(LC_MESSAGES,""); /* not LC_ALL, see use of isalnum below */
  parse_args(argc,argv);

  for (;;) {
    NOEINTR( cohort_fd= open(cohort_path, O_CREAT|O_RDWR, 0644) );
    if (cohort_fd>=0) break;
    if (errno!=ENOENT) dieep(_("open/create cohort state file"), cohort_path);
    if (dir_created++) die("open cohort state file still ENOENT after mkdir");
    NOEINTR( r= mkdir(state_dir,0700) );
    if (r && errno!=EEXIST) dieep(_("create state directory"), state_dir);
  }

  acquire_lock();

  CHECKED( fstat(cohort_fd, &cohort_stab), "fstat our cohort");
  if (cohort_stab.st_size) {
    if (cohort_stab.st_size < sizeof(status))
      die(_("cohort status file too short (disk full?)"));
    else if (cohort_stab.st_size != sizeof(status))
      die("cohort status file too long");
    NOEINTR( r= read(cohort_fd,&status,sizeof(status)) );
    if (r==-1) diee("read cohort");
    if (r!=sizeof(status)) die("cohort file read wrong length");
    release_lock(); report(status);
  }

  if (cohort_stab.st_nlink)
    CHECKED( unlink(cohort_path), "unlink our cohort");

  NOEINTR_TYPED(pid_t, c= fork() );  if (c==(pid_t)-1) diee("fork");
  if (!c) {
    close(cohort_fd); close(lock_fd);
    execvp(command, argv+optind);
    fprintf(stderr,_("watershed: failed to execute `%s': %s\n"),
	    command, strerror(errno));
    exit(127);
  }

  NOEINTR( c2= waitpid(c, &status, 0) );
  if (c2==(pid_t)-1) diee("waitpid");
  if (c2!=c) die("waitpid gave wrong pid");

  for (l=sizeof(status), p=(void*)&status; l>0; l-=r, p+=r)
    CHECKED( write(cohort_fd,p,l), _("write result status"));

  release_lock();
  if (!WIFEXITED(status)) report(status);
  exit(WEXITSTATUS(status));
}
