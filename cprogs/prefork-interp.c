/*
 * "Interpreter" that you can put in #! like this
 *   #!/usr/bin/prefork-interp [<options>] <interpreter>
 * to amortise the startup time of a script.
 *
 * Usages:
 *   prefork-interp  [<option> ..] <interpreter>  [<script> [<args> ...]]
 *   prefork-interp  [<option>,..],<interpreter>   <script> [<args> ...]
 *   prefork-interp '[<option> ..] <interpreter>'  <script> [<args> ...]
 *
 * The script must load a corresponding library (eg Proc::Prefork::Interp
 * for Perl) and call its preform_initialisation_complete routine.
 *
 * Options must specify argument/environment mediation approach.
 * Currently the only args/env mediation supported is:
 *
 *   -U    unlaundered: setup and executor both get all arguments and env vars
 *         ident covers only env vars specified with -E
 *         ident covers only two arguments: interpreter and (if present) script
 *
 * Options for setting the operation mode:
 *
 *   (none)     Default: start new server if needed, then run service
 *   -f         Force a fresh service (old one is terminated)
 *   --kill     Kill any existing service; do not actually run anything
 *
 * Options for controlling whether different invocations share a server:
 *
 *   -E VAR      ident includes env var VAR (or its absence)
 *   -G STRING   ident includes string STRING
 *   -g IDENT    use IDENT rather than hex(SHA256(... identity things ...))
 *
 * (Ordering of -E and -G options is relevant; invocations with different
 * -E -G options are different even if the env var settings are the same)
 */

/*
***************************************************************************

  State during service execution, process parentage and key fds

      CALLER
        ||
        ||
        ||                               listen     watch-err/in
        ||       call                 (accept) \     ,------2
        || ,-----------------------------.     SERVER -----0 WATCHER(C)
      CLIENT 2--=fdpassed>=---------.     \      || &&          |      &&
       (C)  1--=fdpassed>=---------. \     \     ||           inotify
           0--=fdpassed>=---------. \ \     \    ||           sockpath
                                   \ \ \     \   ||
                                   | | |\     |  ||
                                   | | | \    |  ||
                                   | \ |  \   \  ||
                                    \ \ \  \   MONITOR &
                                     \ \ \  `12  ||  |
                                      \ \ \      ||  |
                                       \ \ \     ||  |execterm
                                        \ \ \    ||  |
                                         \ \ \   ||  |
                                          \ \ 2  ||  |
                                           \ 1 EXECUTOR
                                            0
    ----      pipes, sockets
    012       descriptors
    -==-      fds shared
    ||        process parentage
    &&        session leader (daemon)
    &         process group leader

***************************************************************************

 Control flow and causality

      CALLER
         |
         |fork/exec
         |
      CLIENT
         |
      attempt to connect, and read greeting
         |failure?                \success?
         |                         \
      tidy up stale /run entries    *1 (continue from send_fds, below)
      acquire lock
         |
      retry attempt to connect, and read greeting
         |failure?                \success?
         |                         \
      create listening socket     release lock
         |                           \
      fork/daemonise                  *1
         |    `------------------.
         |                      WATCHER(C) &&
         |
       make "fake" initial call socketpair                               (C)
         |                                                    prefork-interp
       fork/exec   #########################################################
         |      `-------------.                                  application
         |         #        SCRIPT (setup)
         |         #          |
         |         #       script initialisation
         |         #          |                                  application
         |         ###########|#############################################
         |         #          |                               prefork-interp
         |         #       identify fds from envirnment               (Perl)
         |         #       open syslog
         |         #          |
         |         #       dzemonize
         |   ,.....<....../   |
      waitpid      #        fork for initial service
         |         #          |child?       |parent?
         |         #          |             |
         |         #          |         SCRIPT [server] &&
         |         #          |             |
         |         #          |         ** accept / event loop **
         |         #          |        accepted?    \      \ \
         |         #          |            /         \ watch\ \idle
         |         #          |        fork child     \stderr\ \timeout?
         |         #          | _________/            |       | |
         |         #          |/                      |read?  | |
         |         #     SCRIPT [monitor]             |   eof?| |
         |         #       setpgrpt &                 |       | |
         |         #          |                     log msg   | |
       read   ,....<.....send greeting                |       | |
      greeting     #          |                    ___________________
         |         #          |
      release      #          |
      lock    *1   #          |
         |   /     #          |
      send fds.....>....      |
         |         #    \receive fds
         |         #             |
         |         #         fork for executor                        (Perl)
         |         #          |parent?        \child?         prefork-interp
         |         #          |          ######\############################
         |         #          |          #  SCRIPT (executor)    application
         |         #          |          #  execute service
         |         #    wait for read    #       |
         |         #      (select)       #   terminates
         |         #        |   |        #       |
         |         #            |        #    kernel closes execterm
         |         #            | ,......<....../|
         |         #      execterm?      #       |
         |         #            |        #     zombie
         |         #        |   | ,......<...../
         |         #       waitpid       #  _______________
         |         #          |          #
         |    ,....<....,..send status   #
    read status    #  ________________   #
   _____________   #


  ********** Or, if client is killed **********

         |         #          |          #  execute service
     terminates    #    wait for read    #       |
         |         #      (select)       #       |
      kernel       #        |   |        #       |
     closes call   #        |            #       |
                \..>......_ |            #       |
   _____________   #       \|call?       #       |
                   #        |            #       |
                   #  kill whole pgrp... #    killled
                   #        |            #     zombie
                   #        |   | ,......<....../
                   #       waitpid       #  _______________
                   #          |          #
                   #   send exit status  #
                   #  _____SIGPIPE______ #

    | - \ /    process control flow
    ... < >    causes mediated by fds or other IPC etc.
    &&         session leader (daemon)
    &          process group leader
    #          language/implementation boundary
    *1         line continued elsewhere
    event?     condition
    ______     process termination (after reaping, if shown)

***************************************************************************

  Sequence of events and fd pluming.
  NB INCOMPLETE - does not cover execterm, cleanup
 
   client (C wrapper)        connects to server
                               (including reading ack byte)
                             if fails or garbage
                             === acquires lock ===
                             makes new listening socket
                             makes watcher pipes
                             forks watcher and awaits
                             makes first-instance socketpair
                             forks setup (script, sock fds indicated in env)
                             fd0, fd1, fd2: from-outer
                             other fd: call(client-end)(fake)
                             reaps setup (and reports error)
                             (implicitly releases lock)
 
      watcher                fd[012]: watcher pipes
                             starts watch on socket path
                             sets stderr to line buffered
                             sets stdin to nonblocking
                             daemonises (one fork, becomes session leader)
                             when socket stat changes, quit
 
      setup (pre-exec)       fd0: null,
                             fd[12]: fd2-from-outer
                             env fds: listener, call(server-end)(fake),
                                       watcher read, watcher write
                             close fd: lockfile
                             possibly clean env, argv
 
      setup (script)         runs initialisation parts of the script
                             at prefork establishment point:
      setup (pm) [1]         opens syslog
                             forks for server
                 [2]         exits
 
         server (pm) [1]     [fd0: null],
                             [fd[12]: fd2-from-outer]
                             setsid
                             right away, forks init monitor
                     [2]     closes outer caller fds and call(fake)
         [server (pm)]       fd[012]: null
                             other fds: listener, syslog
                             runs in loop accepting and forking,
                             reaping and limiting children (incl init monitor)
                             reports failures of monitors to syslog
 
   [client (C wrapper)]      if client connect succeeds:
                             now fd: call(client-end)
                                sends message with: cmdline, env
                                sends fds
 
         [server (script)]   accepts, forks subseq monitor
 
           monitor [1]       [fd0: null]
            (init            [fd[12]: init: fd2-from-outer; subseq: null]
              or             errors: init: fd2; subseq: syslog
             subseq)         other fds: syslog, call(server-end)
                             sends ack byte
                             receives args, env, fds
                             forks executor
 
             executor        sorts out fds:
                             fd0, fd1, fd2: from-outer
                             close fds: call(server-end)
                             retained fds: syslog
 
                             sets cmdline, env
                             runs main part of script
                             exits normally
 
           [monitor]         [fd[012]: null]
                             [fd[12]: init: fd2-from-outer; subseq: null]
                             [errors: init: fd2; subseq: syslog]
                             reaps executor
                             reports status via socket
 
     [client (C wrapper)]    [fd0, fd1, fd2: from-outer]
                             [other fd: call(client-end)]
                             receives status, exits appropriately
                             (if was bad signal, reports to stderr, exits 127)

***************************************************************************

  Protocol, and functions of the script

  1. Script interpreter will be spawned apparently as normal;
     should run synchronously in the normal way until
     "initialisation complete" point.  At initialisation complete:

  2. Env var PREFORK_INTERP contains:

         v1,SECS.NSECS[,...] LISTEN,CALL,WATCHE,WATCHI[,...][ ???]

     To parse it: treat as bytes and split on ASCII space, taking
     the first two words.  (There may or may not be
     further "words"; and if there are they might be binary data.)
     Then split each of the first two words (which will contain only
     ASCII printing characters) on comma.  Take the first two items:

        v1    Protocol version indicator - literal.  If something else,
              fail (means installation is incompatible somehow).

        SECS.NSECS
              timestamp just before script started running, as a
              decimal time_t.  NSECS is exactly 9 digits.
              To be used for auto reloading (see below).

     The 2nd word's items are file descriptors:

        LISTEN   listening socket                 nonblocking
        CALL     call socket for initial call     blocking
        WATCHE   liveness watcher stderr          nonblocking
        WATCHI   liveness sentinel                unspecified

        (any further descriptors should be ignored, not closed)

  3. Library should do the following:

     1. Read and understand the PREFORK_INTERP env var.
        If it is not set, initialisation complete should simply return.
        (This allows simple synchronous operation.)

     2. Open syslog
     3. fork/exit (fork and have parent exit) (to make server)
     4. setsid (to become session leader)
     5. fork initial service (monitor) child, using CALL (see below)
     6. Replace stdin/stdout/stderr with /dev/null,
        and make a note to send all error messages to syslog
     7. Enter select loop, looking for the following:

        A. accept on LISTEN:
            i. see if we need to reload: is any file forming part
               of the program newer than the SECS.NSECS ?
               If so, log at LOG_INFO, and exit immediately
               (dropping CALL, LISTEN, WATCHI, etc.)
            ii. see if we can reap any children, possibly waiting
               for children if we are at our concurrency limit
               (limit should be configured through library, default 4)
               Report child exit status if not zero or SIGPIPE.
            iii. fork service (monitor) child, using accepted fd

        B. WATCHE is readable:
            * EOF: log at LOG_INFO, and exit
            * data to read: read what is available immediately;
              it will be an error message: log it at LOG_ERR, and exit

  4. service (monitor) child does the following:

      1. close all of LISTEN, WATCHI, WATCHE
      2. setpgrp
      3. send a greeting (on CALL) "PFI\n\0\0\0\0" (8 bytes)
      4. read a single byte, fail if it's not zero
      5. three times, receive a single byte with a file descriptor
         attached as ancillary data.  (These descriptors will be
         service stdin, stdout, stderr.)
      6. read a 4-byte big-endian length
      7. read that many bytes, the initial service request message,
         which contains the following nul-terminated strings:
            * environment variable settings in the format NAME=value
            * an empty string
            * arguments NOT INCLUDING argv[0] or script filename
         (not that this means the service request must end in a nul)
      8. make a new pipe EXECTERM
      9. fork for the service executor; in the child
            i. redirect stdin/stdout/stderr to the recevied fds
            ii. replace environment and arguments with those received,
            iii. close descriptors: close the original received descriptors;
                 close CALL; keep only the writing end of EXECTERM
            iv. if the script programming language does things with SIGINT,
                set it set back to default handling (immediate termination).
            v. return back to script, now in the grandchild

      10. in the parent, close EXECTERM writing end, and
      11. select, looking for one of the following:
           * CALL is readable
           * EXECTERM reading end is readable
          No need to actually read, since these shouldn't produce
          spurious wakeups (but do loop on EINTR).
      12. set SIGINT to ignored
      13. send SIGINT to the entire process group
      14. wait, blocking, for the executor child
      15. write the wait status, in 32-bit big-endian, to CAL
      16. exit 0

     Errors detected in the service monitor should be sent to
     syslog, or stderr, depending on whether this is the initial
     service monitor (from part 3 step 5) or an accepted socket
     service monitor (from part 4 step 9); this can be achieved
     easily by having a global flag (set at part 3 step 6),
     or perhaps using logger(8) and redirecting stderr (but
     then be careful to ensure everyone gets only the necessary fds).

     EOF on CALL, or EPIPE/SIGPIPE writing to it, are not errors.
     In this case, exit zero or die with SIGPIPE, so parent
     won't report error either (part 3 step 7(A)(ii)).

***************************************************************************

*/

#include <arpa/inet.h>
#include <sys/utsname.h>

#include <uv.h>

#include "prefork.h"

const char our_name[] = "prefork-interp";

static struct sockaddr_un sockaddr_sun;
static FILE *call_sock;

#define ACK_BYTE '\n'

static const char *const *executor_argv;

static const char header_magic[4] = "PFI\n";

void fusagemessage(FILE *f) {
  fprintf(f, "usage: #!/usr/bin/prefork-interp [<options>]\n");
}

#define MODE_NORMAL 0
#define MODE_KILL   'k'
#define MODE_FRESH  'f'

#define MEDIATION_UNSPECIFIED 0
#define MEDIATION_UNLAUNDERED 'U'

static int mediation = MEDIATION_UNSPECIFIED;
static int mode = MODE_NORMAL;
static int max_sockets = 100; // maximum entries in the run dir is 2x this

static struct stat initial_stab;

const struct cmdinfo cmdinfos[]= {
  PREFORK_CMDINFOS
  { 0,         'U',   0, .iassignto= &mediation, .arg= MEDIATION_UNLAUNDERED },
  { "kill",     0,    0, .iassignto= &mode,      .arg= MODE_KILL   },
  { 0,         'f',   0, .iassignto= &mode,      .arg= MODE_FRESH  },
  { 0 }
};

static void ident_add_stat(const char *path) {
  struct stat stab;
  int r = stat(path, &stab);
  if (r) diee("failed to stat %s", path);

  IDENT_ADD_OBJ(path[0], stab.st_dev);
  IDENT_ADD_OBJ('i',     stab.st_ino);
}

void ident_addinit(void) {
  ident_add_key_byte(1);

  struct utsname uts = { };
  size_t utslen = sizeof(uts);
  int r = uname(&uts);
  if (r) diee("uname failed!");
  IDENT_ADD_OBJ('u', utslen);
  IDENT_ADD_OBJ('u', uts);

  ident_add_stat(".");
  ident_add_stat("/");
}

static void propagate_exit_status(int status, const char *what) {
  int r;

  if (WIFEXITED(status)) {
    _exit(WEXITSTATUS(status));
  }

  if (WIFSIGNALED(status)) {
    int sig = WTERMSIG(status);
    const char *signame = strsignal(sig);
    if (signame == 0) signame = "unknown signal";

    if (! WCOREDUMP(status) &&
	(sig == SIGINT ||
	 sig == SIGTERM ||
	 sig == SIGHUP ||
	 sig == SIGPIPE ||
	 sig == SIGKILL)) {
      struct sigaction sa;
      FILLZERO(sa);
      sa.sa_handler = SIG_DFL;
      if (sig != SIGKILL) {
        r = sigaction(sig, &sa, 0);
        if (r) diee("failed to reset signal handler while propagating %s",
                    signame);

        sigset_t sset;
        sigemptyset(&sset);
        sigaddset(&sset, sig);
        r = sigprocmask(SIG_UNBLOCK, &sset, 0);
        if (r) diee("failed to reset signal block while propagating %s",
                    signame);
      }

      raise(sig);
      die("unexpectedly kept running after raising (to propagate) %s",
	  signame);
    }

    die("%s failed due to signal %d %s%s", what, sig, signame,
	WCOREDUMP(status) ? " (core dumped)" : "");
  }

  die("%s failed with weird wait status %d 0x%x", what, status, status);
}

typedef struct {
  char *name_hash;
  time_t atime;
} PrecleanEntry;

static int preclean_entry_compar_name(const void *av, const void *bv) {
  const PrecleanEntry *a = av;
  const PrecleanEntry *b = bv;
  return strcmp(a->name_hash, b->name_hash);
}

static int preclean_entry_compar_atime(const void *av, const void *bv) {
  const PrecleanEntry *ae = av;  time_t a = ae->atime;
  const PrecleanEntry *be = bv;  time_t b = be->atime;
  return (a > b ? +1 :
	  a < b ? -1 : 0);
}

static time_t preclean_stat_atime(const char *s_path) {
  struct stat stab;
  int r= lstat(s_path, &stab);
  if (r) {
    if (errno!=ENOENT) diee("pre-cleanup: stat socket (%s)", s_path);
    return 0;
  }
  return stab.st_atime;
}

static void preclean(void) {
  DIR *dir = opendir(run_base);
  if (!dir) {
    if (errno == ENOENT) return;
    diee("pre-cleanup: open run dir (%s)", run_base);
  }

  PrecleanEntry *entries=0;
  size_t avail_entries=0;
  size_t used_entries=0;

  struct dirent *de;
  while ((errno = 0, de = readdir(dir))) {
    char c0 = de->d_name[0];
    if (!(c0 == 'l' || c0 == 's')) continue;
    char *name_hash = m_asprintf("%s", de->d_name+1);
    char *s_path = m_asprintf("%s/s%s", run_base, name_hash);
    time_t atime = preclean_stat_atime(s_path);

    if (avail_entries == used_entries) {
      assert(avail_entries < INT_MAX / 4 / sizeof(PrecleanEntry));
      avail_entries <<= 1;
      avail_entries += 10;
      entries = realloc(entries, avail_entries * sizeof(PrecleanEntry));
    }
    entries[used_entries].name_hash = name_hash;
    entries[used_entries].atime = atime;
    used_entries++;
  }
  if (errno) diee("pre-cleanup: read run dir (%s)", run_base);

  // First we dedupe (after sorting by path)
  qsort(entries, used_entries, sizeof(PrecleanEntry),
	preclean_entry_compar_name);
  PrecleanEntry *p, *q;
  for (p=entries, q=entries; p < entries + used_entries; p++) {
    if (q > entries && !strcmp(p->name_hash, (q-1)->name_hash))
      continue;
    *q++ = *p;
  }
  used_entries = q - entries;

  // Now maybe delete some things
  //
  // Actually this has an off-by-one error since we are about
  // to create a socket, so the actual number of sockets is one more.
  // But, *actually*, since there might be multiple of us running at once,
  // we might have even more than that.  This doesn't really matter.
  if (used_entries > max_sockets) {
    qsort(entries, used_entries, sizeof(PrecleanEntry),
	  preclean_entry_compar_atime);
    for (p=entries; p < entries + max_sockets; p++) {
      char *l_path = m_asprintf("%s/l%s", run_base, p->name_hash);
      char *s_path = m_asprintf("%s/s%s", run_base, p->name_hash);
      int lock_fd = flock_file(l_path);
      // Recheck atime - we might have raced!
      time_t atime = preclean_stat_atime(s_path);
      if (atime != p->atime) {
	// Raced.  This will leave use deleting too few things.  Whatever.
      } else {
	int r= unlink(s_path);
	if (r && errno!=ENOENT) diee("preclean: delete stale (%s)", s_path);
	r= unlink(l_path);
	if (r) diee("preclean: delete stale lock (%s)", s_path);
	// NB we don't hold the lock any more now.
      }
      close(lock_fd);
      free(l_path);
      free(s_path);
    }
  }

  for (p=entries; p < entries + used_entries; p++)
    free(p->name_hash);
  free(entries);
}

static __attribute((noreturn)) void die_data_overflow(void) {
  die("cannot handle data with length >2^32");
}

static void prepare_data(size_t *len, char **buf,
			 const void *data, size_t dl) {
  if (len) {
    if (dl >= SIZE_MAX - *len)
      die_data_overflow();
    *len += dl;
  }
  if (buf) {
    memcpy(*buf, data, dl);
    *buf += dl;
  }
}

static void prepare_length(size_t *len, char **buf, size_t dl_sz) {
  if (dl_sz > UINT32_MAX) die_data_overflow();
  uint32_t dl = htonl(dl_sz);
  prepare_data(len, buf, &dl, sizeof(dl));
}

static void prepare_string(size_t *len, char **buf, const char *s) {
  size_t sl = strlen(s);
  prepare_data(len, buf, s, sl+1);
}

static void prepare_message(size_t *len, char **buf) {
  const char *s;

  const char *const *p = (void*)environ;
  while ((s = *p++)) {
    if (strchr(s, '='))
      prepare_string(len, buf, s);
  }

  prepare_string(len, buf, "");

  p = executor_argv;
  while ((s = *p++))
    prepare_string(len, buf, s);
}

static void send_fd(int payload_fd) {
  int via_fd = fileno(call_sock);

  union {
    struct cmsghdr align;
    char buf[CMSG_SPACE(sizeof(payload_fd))];
  } cmsg_buf;

  struct msghdr msg;
  FILLZERO(msg);
  FILLZERO(cmsg_buf);

  char dummy_byte = 0;
  struct iovec iov;
  FILLZERO(iov);
  iov.iov_base = &dummy_byte;
  iov.iov_len = 1;

  msg.msg_name = 0;
  msg.msg_iov = &iov;
  msg.msg_iovlen = 1;
  msg.msg_control = cmsg_buf.buf;
  msg.msg_controllen = sizeof(cmsg_buf.buf);

  struct cmsghdr *cmsg = CMSG_FIRSTHDR(&msg);
  cmsg->cmsg_level = SOL_SOCKET;
  cmsg->cmsg_type = SCM_RIGHTS;
  cmsg->cmsg_len = CMSG_LEN(sizeof(payload_fd));
  *(int*)CMSG_DATA(cmsg) = payload_fd;

  msg.msg_controllen = sizeof(cmsg_buf.buf);

  for (;;) {
    ssize_t r = sendmsg(via_fd, &msg, 0);
    if (r == -1) {
      if (errno == EINTR) continue;
      diee("send fd");
    }
    assert(r == 1);
    break;
  }
}

static void send_request(void) {
  char ibyte= 0;
  ssize_t sr = fwrite(&ibyte, 1, 1, call_sock);
  if (sr != 1) diee("write signalling byte");

  // Sending these before the big message makes it easier for the script to
  // use buffered IO for the message.
  send_fd(0);
  send_fd(1);
  send_fd(2);

  size_t len = 0;
  prepare_message(&len, 0);

  size_t tlen = len + 4;
  char *m = xmalloc(tlen);
  char *p = m;
  prepare_length(0, &p, len);
  prepare_message(0, &p);
  assert(p == m + tlen);

  sr = fwrite(m, tlen, 1, call_sock);
  if (sr != 1) diee("write request (buffer)");

  if (fflush(call_sock)) diee("write request");
}

static FILE *call_sock_from_fd(int fd) {
  int r;

  FILE *call_sock = fdopen(fd, "r+");
  if (!call_sock) diee("fdopen socket");

  r = setvbuf(call_sock, 0, _IONBF, 0);
  if (r) die("setvbuf socket");

  return call_sock;
}

static bool was_eof(FILE *call_sock) {
  return feof(call_sock) || errno==ECONNRESET;
}

// Returns -1 on EOF
static int protocol_read_maybe(void *data, size_t sz) {
  if (!sz) return 0;
  size_t sr = fread(data, sz, 1, call_sock);
  if (sr != 1) {
    if (was_eof(call_sock)) return -1;
    diee("read() on monitor call socket (%zd)", sz);
  }
  return 0;
}

static void protocol_read(void *data, size_t sz) {
  if (protocol_read_maybe(data, sz) < 0)
    die("monitor process quit unexpectedly");
}

// Returns 0 if OK, error msg if peer was garbage.
static const char *read_greeting(void) {
  char got_magic[sizeof(header_magic)];

  if (protocol_read_maybe(&got_magic, sizeof(got_magic)) < 0)
    return "initial monitor process quit"
      " (maybe script didn't call preform_initialisation_complete?)";

  if (memcmp(got_magic, header_magic, sizeof(header_magic)))
    die("got unexpected protocol magic 0x%02x%02x%02x%02x",
	got_magic[0], got_magic[1], got_magic[2], got_magic[3]);

  uint32_t xdata_len;
  protocol_read(&xdata_len, sizeof(xdata_len));
  void *xdata = xmalloc(xdata_len);
  protocol_read(xdata, xdata_len);

  return 0;
}

// Returns: call(client-end), or 0 to mean "is garbage"
// find_socket_path must have been called
static FILE *connect_existing(void) {
  int r;
  int fd = -1;

  if (mode != MODE_NORMAL) return 0;

  fd = socket(AF_UNIX, SOCK_STREAM, 0);
  if (fd==-1) diee("socket() for client");

  socklen_t salen = sizeof(sockaddr_sun);
  r = connect(fd, (const struct sockaddr*)&sockaddr_sun, salen);
  if (r==-1) {
    if (errno==ECONNREFUSED || errno==ENOENT) goto x_garbage;
    diee("connect() %s", socket_path);
  }

  call_sock = call_sock_from_fd(fd);
  fd = -1;

  if (read_greeting())
    goto x_garbage;

  return call_sock;

 x_garbage:
  if (call_sock) { fclose(call_sock); call_sock=0; }
  if (fd >= 0) close(fd);
  return 0;
}

static void watcher_cb_stdin(uv_poll_t *handle, int status, int events) {
  char c;
  int r;

  if ((errno = -status)) diee("watcher: poll stdin");
  for (;;) {
    r= read(0, &c, 1);
    if (r!=-1) _exit(0);
    if (!(errno==EINTR || errno==EWOULDBLOCK || errno==EAGAIN))
      diee("watcher: read sentinel stdin");
  }
}

static void watcher_cb_sockpath(uv_fs_event_t *handle, const char *filename,
				int events, int status) {
  int r;
  struct stat now_stab;

  if ((errno = -status)) diee("watcher: poll stdin");
  for (;;) {
    r= stat(socket_path, &now_stab);
    if (r==-1) {
      if (errno==ENOENT) _exit(0);
      if (errno==EINTR) continue;
      diee("stat socket: %s", socket_path);
    }
    if (!stabs_same_inode(&now_stab, &initial_stab))
      _exit(0);
  }
}

// On entry, stderr is still inherited, but 0 and 1 are the pipes
static __attribute__((noreturn))
void become_watcher(void) {
  uv_loop_t loop;
  uv_poll_t uvhandle_stdin;
  uv_fs_event_t uvhandle_sockpath;
  int r;

  nonblock(0);

  errno= -uv_loop_init(&loop);
  if (errno) diee("watcher: uv_loop_init");

  errno= -uv_poll_init(&loop, &uvhandle_stdin, 0);
  if (errno) diee("watcher: uv_poll_init");
  errno= -uv_poll_start(&uvhandle_stdin,
			UV_READABLE | UV_WRITABLE | UV_DISCONNECT,
			watcher_cb_stdin);
  if (errno) diee("watcher: uv_poll_start");

  errno= -uv_fs_event_init(&loop, &uvhandle_sockpath);
  if (errno) diee("watcher: uv_fs_event_init");

  errno= -uv_fs_event_start(&uvhandle_sockpath, watcher_cb_sockpath,
			    socket_path, 0);
  if (errno) diee("watcher: uv_fs_event_start");

  // OK everything is set up, let us daemonise
  if (dup2(1,2) != 2) diee("watcher: set daemonised stderr");
  r= setvbuf(stderr, 0, _IOLBF, BUFSIZ);
  if (r) diee("watcher: setvbuf stderr");

  pid_t child = fork();
  if (child == (pid_t)-1) diee("watcher: fork");
  if (child) _exit(0);

  if (setsid() == (pid_t)-1) diee("watcher: setsid");

  r= uv_run(&loop, UV_RUN_DEFAULT);
  die("uv_run returned (%d)", r);
}

static __attribute__((noreturn))
void become_setup(int sfd, int lockfd, int fake_pair[2],
		  int watcher_stdin, int watcher_stderr) {
  close(lockfd);
  close(fake_pair[0]);
  int call_fd = fake_pair[1];

  int null_0 = open("/dev/null", O_RDONLY);  if (null_0 < 0) diee("open null");
  if (dup2(null_0, 0)) diee("dup2 /dev/null onto stdin");
  close(null_0);
  if (dup2(2, 1) != 1) die("dup2 stderr onto stdout");

  nonblock(sfd);

  // Extension could work like this:
  //
  // We could advertise a new protocol (perhaps one which is nearly entirely
  // different after the connect) by putting a name for it comma-separated
  // next to "v1".  Simple extension can be done by having the script
  // side say something about it in the ack xdata, which we currently ignore.
  // Or we could add other extra data after v1.
  putenv(m_asprintf("PREFORK_INTERP=v1,%jd.%09ld %d,%d,%d,%d",
                    (intmax_t)initial_stab.st_mtim.tv_sec,
                    (long)initial_stab.st_mtim.tv_nsec,
		    sfd, call_fd, watcher_stdin, watcher_stderr));

  execvp(executor_argv[0], (char**)executor_argv);
  diee("execute %s", executor_argv[0]);
}

static void connect_or_spawn(void) {
  int r;

  call_sock = connect_existing();
  if (call_sock) return;

  // We're going to make a new one, so clean out old ones
  preclean();

  int lockfd = acquire_lock();

  if (mode == MODE_KILL) {
    r= unlink(socket_path);
    if (r && errno != ENOENT) diee("remove socket %s", socket_path);

    r= unlink(lock_path);
    if (r) diee("rmeove lock %s", lock_path);
    _exit(0);
  }

  call_sock = connect_existing();
  if (call_sock) { close(lockfd); return; }

  // We must start a fresh one, and we hold the lock

  r = unlink(socket_path);
  if (r<0 && errno!=ENOENT)
    diee("failed to remove stale socket %s", socket_path);

  int sfd = socket(AF_UNIX, SOCK_STREAM, 0);
  if (sfd<0) diee("socket() for new listener");

  socklen_t salen = sizeof(sockaddr_sun);
  r= bind(sfd, (const struct sockaddr*)&sockaddr_sun, salen);
  if (r<0) diee("bind() on new listener");

  r= stat(socket_path, &initial_stab);
  if (r<0) diee("stat() fresh socket");

  // We never want callers to get ECONNREFUSED.  But:
  // There is a race here: from my RTFM they may get ECONNREFUSED
  // if they try between our bind() and listen().  But if they do, they'll
  // acquire the lock (serialising with us) and retry, and then it will work.
  r = listen(sfd, INT_MAX);
  if (r<0) diee("listen() for new listener");

  // Fork watcher

  int watcher_stdin[2];
  int watcher_stderr[2];
  if (pipe(watcher_stdin) || pipe(watcher_stderr))
    diee("pipe() for socket inode watcher");

  pid_t watcher = fork();
  if (watcher == (pid_t)-1) diee("fork for watcher");
  if (!watcher) {
    close(sfd);
    close(lockfd);
    close(watcher_stdin[1]);
    close(watcher_stderr[0]);
    if (dup2(watcher_stdin[0], 0) != 0 ||
	dup2(watcher_stderr[1], 1) != 1)
      diee("initial dup2() for watcher");
    close(watcher_stdin[0]);
    close(watcher_stderr[1]);
    become_watcher();
  }

  close(watcher_stdin[0]);
  close(watcher_stderr[1]);
  nonblock(watcher_stderr[0]);

  // Fork setup

  int fake_pair[2];
  r = socketpair(AF_UNIX, SOCK_STREAM, 0, fake_pair);
  if (r<0) diee("socketpair() for fake initial connection");

  pid_t setup_pid = fork();
  if (setup_pid == (pid_t)-1) diee("fork for spawn setup");
  if (!setup_pid) become_setup(sfd, lockfd, fake_pair,
			       watcher_stdin[1], watcher_stderr[0]);
  close(fake_pair[1]);
  close(sfd);

  call_sock = call_sock_from_fd(fake_pair[0]);

  int status;
  pid_t got = waitpid(setup_pid, &status, 0);
  if (got == (pid_t)-1) diee("waitpid setup [%ld]", (long)setup_pid);
  if (got != setup_pid) diee("waitpid setup [%ld] gave [%ld]!",
			     (long)setup_pid, (long)got);
  if (status != 0) propagate_exit_status(status, "setup");

  const char *emsg = read_greeting();
  if (emsg) die("setup failed: %s", emsg);

  close(lockfd);
  return;
}

static void make_executor_argv(const char *const *argv) {
  switch (mediation) {
  case MEDIATION_UNLAUNDERED: break;
  default: die("need -U (specifying unlaundered argument handling)");
  }

  const char *arg;
  #define EACH_NEW_ARG(EACH) {			\
    arg = interp; { EACH }			\
    if ((arg = script)) { EACH }		\
    const char *const *walk = argv;		\
    while ((arg = *walk++)) { EACH }		\
  }

  size_t count = 1;
  EACH_NEW_ARG( (void)arg; count++; );

  const char **out = calloc(count, sizeof(char*));
  executor_argv = (const char* const*)out;
  if (!executor_argv) diee("allocate for arguments");

  EACH_NEW_ARG( *out++ = arg; );
  *out++ = 0;
}

int main(int argc_unused, const char *const *argv) {
  process_opts(&argv);

  // Now we have
  //  - possibly interp
  //  - possibly script
  //  - remaining args
  // which ought to be passed on to the actual executor.
  make_executor_argv(argv);

  find_socket_path();
  FILLZERO(sockaddr_sun);
  sockaddr_sun.sun_family = AF_UNIX;
  assert(strlen(socket_path) <= sizeof(sockaddr_sun.sun_path));
  strncpy(sockaddr_sun.sun_path, socket_path, sizeof(sockaddr_sun.sun_path));

  connect_or_spawn();

  // We're committed now, send the request (or bail out)
  send_request();

  uint32_t status;
  protocol_read(&status, sizeof(status));

  status = ntohl(status);
  if (status > INT_MAX) die("status 0x%lx does not fit in an int",
			    (unsigned long)status);

  propagate_exit_status(status, "invocation");
}
