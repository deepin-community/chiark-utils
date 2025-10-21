/*
 * "Interpreter" that you can put in #! like this
 *   #!/usr/bin/cgi-fcgi-interp [<options>] <interpreter>
 *
 * Usages:
 *   cgi-fcgi-interp  [<option> ..] <interpreter>  <script> [<ignored> ...]
 *   cgi-fcgi-interp  [<option>,..],<interpreter>  <script> [<ignored> ...]
 *   cgi-fcgi-interp '[<option> ..] <interpreter>' <script> [<ignored> ...]
 */
/*
 * cgi-fcgi-interp.[ch] - Convenience wrapper for cgi-fcgi
 *
 * Copyright 2016 Ian Jackson
 * Copyright 1982,1986,1993 The Regents of the University of California
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
 * See below for a BSD 3-clause notice regarding timespeccmp.
 */
/*
 * The result is a program which looks, when executed via the #!
 * line, like a CGI program.  But the script inside will be executed
 * via <interpreter> in an fcgi context.
 *
 * Options:
 *
 *  <interpreter>
 *          The real interpreter to use.  Eg "perl".  Need not
 *          be an absolute path; will be fed to execvp.
 *
 *  -G<ident-info>
 *          Add <ident-info> to the unique identifying information for
 *          this fcgi program.  May be repeated; order is significant.
 *
 *  -E<ident-info-env-var>
 *          Look <ident-info-env-var> up in the environment and add
 *          <ident-info-env-var>=<value> as if specified with -G.  If
 *          the variable is unset in the environment, it is as if
 *          -G<ident-info-env-var> was specified.
 *
 *  -g<ident>
 *          Use <ident> rather than hex(sha256(<interp>\0<script>\0))
 *          as the basename of the leafname of the fcgi rendezvous
 *          socket.  If <ident> contains only hex digit characters it
 *          ought to be no more than 32 characters.  <ident> should
 *          not contain spaces or commas (see below).
 *
 *  -M<numservers>
 *         Start <numservers> instances of the program.  This
 *         determines the maximum concurrency.  (Note that unlike
 *         speedy, the specified number of servers is started
 *         right away.)  The default is 4.
 *
 *  -c<interval>
 *         Stale server check interval, in seconds.  The worker
 *         process group will get a SIGTERM when it is no longer
 *         needed to process new requests.  Ideally it would continue
 *         to serve any existing requests.  The SIGTERM will arrive no
 *         earlier than <interval> after the last request arrived at
 *         the containing webserver.  Default is 300.
 *
 *  -D
 *         Debug mode.  Do not actually run program.  Instead, print
 *         out what we would do.
 *
 * <options> and <interpreter> can be put into a single argument
 * to cgi-fcgi-interp, separated by spaces or commas.  <interpreter>
 * must come last.
 *
 * cgi-fcgi-interp automatically expires old sockets, including
 * ones where the named script is out of date.
 */
/*
 * Uses one of two directories
 *   /var/run/user/<UID>/cgi-fcgi-interp/
 *   ~/.cgi-fcgi-interp/<node>/
 * and inside there uses these paths
 *   s<ident>
 *   l<ident>    used to lock around garbage collection
 *
 * If -M<ident> is not specified then an initial substring of the
 * lowercase hex of the sha256 of <interp>\0<script>\0 is
 * used.  The substring is chosen so that the whole path is 10 bytes
 * shorter than sizeof(sun_path).  But always at least 33 characters.
 *
 * <node> is truncated at the first `.' and after the first 32
 * characters.
 *
 * Algorithm:
 *  - see if /var/run/user exists
 *       if so, lstat /var/run/user/<UID> and check that
 *         we own it and it's X700; if not, fail
 *         if it's ok then <base> is /var/run/user/<UID>
 *       otherwise, look for and maybe create ~/.cgi-fcgi-interp
 *         (where ~ is HOME or from getpwuid)
 *         and then <base> is ~/.cgi-fcgi-interp/<node>
 *  - calculate pathname (checking <ident> length is OK)
 *  - check for and maybe create <base>
 *  - stat and lstat the <script>
 *  - stat the socket and check its timestamp
 *       if it is too old, unlink it
 *  - dup stderr, mark no cloexec
 *  - set CHIARKUTILS_CGIFCGIINTERP_STAGE2=<stderr-copy-fd>
 *  - run     cgi-fcgi -connect SOCKET <script>
 *
 * When CHIARKUTILS_CGIFCGIINTERP_STAGE2 is set, --stage2 does this:
 *  - dup2 <was-stderr> to fd 2
 *  - open /dev/null and expect fd 1 (and if not, close it)
 *  - become a new process group
 *  - lstat <socket> to find its inum, mtime
 *  - fork/exec <interp> <script>
 *  - periodically lstat <interp> and <script> and
 *      if mtime is newer than our start time
 *      kill process group (at second iteration)
 */

#include "prefork.h"
#include "timespeccmp.h"

#define STAGE2_VAR "CHIARKUTILS_CGIFCGIINTERP_STAGE2"

static const char *stage2;

const char our_name[] = "cgi-fcgi-interp";

static int numservers=4, debugmode;
static int check_interval=300;

const struct cmdinfo cmdinfos[]= {
  PREFORK_CMDINFOS
  { 0, 'M',   1, .call=of_iassign,  .iassignto= &numservers            },
  { 0, 'D',   0,                    .iassignto= &debugmode,    .arg= 1 },
  { 0, 'c',   1, .call=of_iassign,  .iassignto= &check_interval        },
  { 0 }
};

void fusagemessage(FILE *f) {
  fprintf(f, "usage: #!/usr/bin/cgi-fcgi-interp [<options>]\n");
}

void ident_addinit(void) {
}

static int stderr_copy;

static void make_stderr_copy(void) {
  stderr_copy = dup(2);
  if (stderr_copy < 0) diee("dup stderr (for copy for stage2)");
}

static void prep_stage2(void) {
  int r;
  
  const char *stage2_val = m_asprintf("%d", stderr_copy);
  r = setenv(STAGE2_VAR, stage2_val, 1);
  if (r) diee("set %s (to announce to stage2)", STAGE2_VAR);
}

#ifdef st_mtime

static bool stab_isnewer(const struct stat *a, const struct stat *b) {
  if (debugmode)
    fprintf(stderr,"stab_isnewer mtim %lu.%06lu %lu.06%lu\n",
	    (unsigned long)a->st_mtim.tv_sec,
	    (unsigned long)a->st_mtim.tv_nsec,
	    (unsigned long)b->st_mtim.tv_sec,
	    (unsigned long)b->st_mtim.tv_nsec);
  return timespeccmp(&a->st_mtim, &b->st_mtim, >);
}

static void stab_mtimenow(struct stat *out) {
  int r = clock_gettime(CLOCK_REALTIME, &out->st_mtim);
  if (r) diee("(stage2) clock_gettime");
  if (debugmode)
    fprintf(stderr,"stab_mtimenow mtim %lu.%06lu\n",
	    (unsigned long)out->st_mtim.tv_sec,
	    (unsigned long)out->st_mtim.tv_nsec);
}

#else /* !defined(st_mtime) */

static bool stab_isnewer(const struct stat *a, const struct stat *b) {
  if (debugmode)
    fprintf(stderr,"stab_isnewer mtime %lu %lu\n",
	    (unsigned long)a->st_mtime,
	    (unsigned long)b->st_mtime);
  return a->st_mtime > b->st_mtime;
}

static void stab_mtimenow(struct stat *out) {
  out->st_mtime = time(NULL);
  if (out->st_mtime == (time_t)-1) diee("(stage2) time()");
  if (debugmode)
    fprintf(stderr,"stab_mtimenow mtime %lu\n",
	    (unsigned long)out->st_mtime);
}

#endif /* !defined(st_mtime) */

static bool check_garbage_vs(const struct stat *started) {
  struct stat script_stab;
  int r;

  r = lstat(script, &script_stab);
  if (r) diee("lstat script (%s)",script);

  if (stab_isnewer(&script_stab, started))
    return 1;

  if (S_ISLNK(script_stab.st_mode)) {
    r = stat(script, &script_stab);
    if (r) diee("stat script (%s0",script);

    if (stab_isnewer(&script_stab, started))
      return 1;
  }

  return 0;
}

static bool check_garbage(void) {
  struct stat sock_stab;
  int r;

  r = lstat(socket_path, &sock_stab);
  if (r) {
    if ((errno == ENOENT))
      return 0; /* well, no garbage then */
    diee("stat socket (%s)",socket_path);
  }

  return check_garbage_vs(&sock_stab);
}

static void tidy_garbage(void) {
  /* We lock l<ident> and re-check.  The effect of this is that each
   * stale socket is removed only once.  So unless multiple updates to
   * the script happen rapidly, we can't be racing with the cgi-fcgi
   * (which is recreating the socket */
  int lockfd = -1;
  int r;

  lockfd = acquire_lock();

  if (check_garbage()) {
    r = unlink(socket_path);
    if (r) {
      if (!(errno == ENOENT))
	diee("remove out-of-date socket (%s)", socket_path);
    }
  }

  r = close(lockfd);
  if (r) diee("close lock (%s)", lock_path);
}

/* stage2 predeclarations */
static void record_baseline_time(void);
static void become_pgrp(void);
static void setup_handlers(void);
static void spawn_script(void);
static void queue_alarm(void);
static void start_logging(void);
static void await_something(void);

int main(int unused_argc, const char *const *argv) {
  int r;

  stage2 = getenv(STAGE2_VAR);
  if (stage2) {
    int stderrfd = atoi(stage2);
    assert(stderrfd>2);

    r = dup2(stderrfd, 2);
    assert(r==2);

    r = open("/dev/null",O_WRONLY);
    if (r<0) diee("open /dev/null as stdout");
    if (r>=3) close(r);
    else if (r!=1) die("open /dev/null for stdout gave bad fd %d",r);

    r = close(stderrfd);
    if (r) diee("close saved stderr fd");
  }

  process_opts(&argv);
  if (!script) badusage("need script argument");

  if (!stage2) {
    
    find_socket_path();

    bool isgarbage = check_garbage();

    if (debugmode) {
      printf("socket: %s\n",socket_path);
      printf("interp: %s\n",interp);
      printf("script: %s\n",script);
      printf("garbage: %d\n",isgarbage);
      exit(0);
    }

    if (isgarbage)
      tidy_garbage();

    make_stderr_copy();
    prep_stage2();

    execlp("cgi-fcgi",
	   "cgi-fcgi", "-connect", socket_path,
	   script,
	   m_asprintf("%d", numservers),
	   (char*)0);
    diee("exec cgi-fcgi");
    
  } else { /*stage2*/

    record_baseline_time();
    become_pgrp();
    setup_handlers();
    spawn_script();
    queue_alarm();
    start_logging();
    await_something();
    abort();

  }
}

/* stage2 */

/* It is most convenient to handle the recheck timeout, as well as
 * child death, in signal handlers.  Our signals all block each other,
 * and the main program has signals blocked except in sigsuspend, so
 * we don't need to worry about async-signal-safety, or errno. */

static struct stat baseline_time;
static pid_t script_child, stage2_pgrp;
static bool out_of_date;
static int errpipe;

static void record_baseline_time(void) {
  stab_mtimenow(&baseline_time);
}

static void become_pgrp(void) {
  int r;

  stage2_pgrp = getpid();

  r = setpgid(0,0);
  if (r) diee("(stage2) setpgid");
}

static void atexit_handler(void) {
  int r;

  sighandler_t sigr = signal(SIGTERM,SIG_IGN);
  if (sigr == SIG_ERR) warninge("(stage2) signal(SIGTERM,SIG_IGN)");

  r = killpg(stage2_pgrp,SIGTERM);
  if (r) warninge("(stage) killpg failed");
}

static void alarm_handler(int dummy) {
  if (out_of_date)
    /* second timeout */
    exit(0); /* transfers control to atexit_handler */

  out_of_date = check_garbage_vs(&baseline_time);
  queue_alarm();
}

static void child_handler(int dummy) {
  for (;;) {
    int status;
    pid_t got = waitpid(-1, &status, WNOHANG);
    if (got == (pid_t)-1) diee("(stage2) waitpid");
    if (got != script_child) {
      warning("(stage2) waitpid got status %d for unknown child [%lu]",
	      status, (unsigned long)got);
      continue;
    }
    if (WIFEXITED(status)) {
      int v = WEXITSTATUS(status);
      if (v) warning("program failed with error exit status %d", v);
      exit(status);
    } else if (WIFSIGNALED(status)) {
      int s = WTERMSIG(status);
      warning("program died due to fatal signal %s%s",
	      strsignal(s), WCOREDUMP(status) ? " (core dumped" : "");
      assert(status & 0xff);
      exit(status & 0xff);
    } else {
      die("program failed with crazy wait status %#x", status);
    }
  }
  exit(127);
}

static void setup_handlers(void) {
  struct sigaction sa;
  int r;

  r = atexit(atexit_handler);
  if (r) diee("(stage2) atexit");

  sigemptyset(&sa.sa_mask);
  sigaddset(&sa.sa_mask, SIGALRM);
  sigaddset(&sa.sa_mask, SIGCHLD);
  sa.sa_flags = 0;

  r = sigprocmask(SIG_BLOCK, &sa.sa_mask, 0);
  if (r) diee("(stage2) sigprocmask(SIG_BLOCK,)");

  sa.sa_handler = alarm_handler;
  r = sigaction(SIGALRM, &sa, 0);
  if (r) diee("(stage2) sigaction SIGALRM");

  sa.sa_flags |= SA_NOCLDSTOP;
  sa.sa_handler = child_handler;
  r = sigaction(SIGCHLD, &sa, 0);
  if (r) diee("(stage2) sigaction SIGCHLD");
}

static void spawn_script(void) {
  int r;
  int errpipes[2];

  r = pipe(errpipes);
  if (r) diee("(stage2) pipe");

  script_child = fork();
  if (script_child == (pid_t)-1) diee("(stage2) fork");
  if (!script_child) {
    r = close(errpipes[0]);
    if (r) diee("(stage2 child) close errpipes[0]");

    r = dup2(errpipes[1], 2);
    if (r != 2) diee("(stage2 child) dup2 stderr");

    execlp(interp,
	   interp, script, (char*)0);
    diee("(stage2) exec interpreter (`%s', for `%s')\n",interp,script);
  }

  r = close(errpipes[1]);
  if (r) diee("(stage2) close errpipes[1]");

  errpipe = errpipes[0];
  r = fcntl(errpipe, F_SETFL, O_NONBLOCK);
  if (r) diee("(stage2) set errpipe nonblocking");
}

static void queue_alarm(void) {
  alarm(check_interval);
}

static void start_logging(void) {
  int r;

  openlog(script, LOG_NOWAIT|LOG_PID, LOG_USER);
  logging = 1;
  r = dup2(1,2);
  if (r!=2) diee("dup2 stdout to stderr");
}

static void errpipe_readable(void) {
  static char buf[1024];
  static int pending;

  /* %: does not contain newlines
   * _: empty (garbage)
   */ 

  /*           %%%%%%%%%%%__________________ */
  /*                      ^ pending          */

  for (;;) {
    int avail = sizeof(buf) - pending;
    ssize_t got = read(errpipe, buf+pending, avail);
    if (got==-1) {
      if (errno==EINTR) continue;
      else if (errno==EWOULDBLOCK || errno==EAGAIN) return;
      else diee("(stage2) errpipe read");
      got = 0;
    } else if (got==0) {
      warning("program closed its stderr fd");
      errpipe = -1;
      return;
    }
    int scanned = pending;
    pending += got;
    int eaten = 0;
    for (;;) {
      const char *newline = memchr(buf+scanned, '\n', pending-scanned);
      int printupto, eat;
      if (newline) {
	printupto = newline-buf;
	eat = printupto + 1;
      } else if (!eaten && pending==sizeof(buf)) { /* overflow */
	printupto = pending;
	eat = printupto;
      } else {
	break;
      }
      syslog(LOG_ERR,"stderr: %.*s", printupto-eaten, buf+eaten);
      eaten += eat;
      scanned = eaten;
    }
    pending -= eaten;
    memmove(buf, buf+eaten, pending);
  }
}     

static void await_something(void) {
  int r;
  sigset_t mask;
  sigemptyset(&mask);

  for (;;) {
    fd_set rfds;
    FD_ZERO(&rfds);
    if (errpipe >= 0)
      FD_SET(errpipe, &rfds);
    r = pselect(errpipe+1, &rfds,0,0, 0, &mask);
    if (r==-1) {
      if (errno != EINTR) diee("(stage2) sigsuspend");
      continue;
    }
    assert(r>0);
    assert(FD_ISSET(errpipe, &rfds));
    errpipe_readable();
  }
}
