/*
 * rcopy-repeatedly
 *
 *   You say  rcopy-repeatedly local-file user@host:remote-file
 *   and it polls for changes to local-file and copies them to
 *   remote-file.  rcopy-repeatedly must be installed at the far end.
 *   You can copy in either direction but not between two remote
 *   locations.
 *
 *   Limitations:
 *    * Cannot cope with files which are modified between us opening
 *      and statting them for the first time; if the file shrinks
 *      we may bomb out.  Workaround: use rename-in-place.
 *    * When transferring large files, bandwidth limiter will
 *      be `lumpy' as the whole file is transferred and then we
 *      sleep.
 *    * Cannot copy between two local files.  Workaround: a symlink
 *      (but presumably there was some reason you weren't doing that)
 *    * No ability to synchronise more than just exactly one file
 *    * Polls.  It would be nice to use inotify or something.
 *
 *   Inherent limitations:
 *    * Can only copy plain files.
 *
 *   See the --help for options.
 */     
/*
 * rcopy-repeatedly is
 *  Copyright (C) 2008 Ian Jackson <ian@davenant.greenend.org.uk>
 * and the option parser we use is
 *  Copyright (C) 1994,1995 Ian Jackson <ian@davenant.greenend.org.uk>
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
 */
/*
 * protocol is:
 *   server sends banner
 *    - "#rcopy-repeatedly#\n"
 *    - length of declaration, as 4 hex digits, zero prefixed,
 *      and a newline [5 bytes].  In this protocol version this
 *      will be "0002" but client _must_ parse it.
 *   server sends declaration
 *    - one of "u " or "d" [1 byte]
 *    - optionally, some more ascii text, reserved for future use
 *      must be ignored by declaree (but not sent by declarer)
 *    - a newline [1 byte]
 *   client sends
 *    - 0x02   START
 *        n    2 bytes big endian declaration length
 *        ...  client's declaration (ascii text, including newline)
 8             see above
 * then for each update
 *   sender sends one of
 *    - 0x03   destination file should be deleted
 *             but note that contents must be retained by receiver
 *             as it may be used for rle updates
 *    - 0x04   complete new destination file follows, 64-bit length
 *        l    8 bytes big endian length
 *        ...  l bytes data
 *             receiver must then reply with 0x01 ACK
 */

#define _GNU_SOURCE

#include <stdio.h>
#include <time.h>
#include <stdarg.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <errno.h>
#include <limits.h>
#include <assert.h>
#include <math.h>

#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>

#include "myopt.h"

#define REPLMSG_ACK    0x01
#define REPLMSG_START  0x02
#define REPLMSG_RM     0x03
#define REPLMSG_FILE64 0x04

static const char banner[]= "#rcopy-repeatedly#\n";

static FILE *commsi, *commso;

static double max_bw_prop= 0.2;
static int txblocksz= INT_MAX, verbose=1;
static int min_interval_usec= 100000; /* 100ms */

static int nsargs;
static const char **sargs;

static const char *rsh_program= 0;
static const char *rcopy_repeatedly_program= "rcopy-repeatedly";
static int server_upcopy=-1; /* -1 means not yet known; 0 means download */
  /* `up' means towards the client,
   * since we regard the subprocess as `down' */

static int udchar;

static char mainbuf[65536]; /* must be at least 2^16 */

#define NORETURN __attribute__((noreturn))

static void vdie(int ec, const char *pfx, int eno,
		 const char *fmt, va_list al) NORETURN;
static void vdie(int ec, const char *pfx, int eno,
		 const char *fmt, va_list al) {
  fputs("rcopy-repeatedly: ",stderr);
  if (server_upcopy>=0) fputs("server: ",stderr);
  if (pfx) fprintf(stderr,"%s: ",pfx);
  vfprintf(stderr,fmt,al);
  if (eno!=-1) fprintf(stderr,": %s",strerror(eno));
  fputc('\n',stderr);
  exit(ec);
}
static void die(int ec, const char *pfx, int eno,
		const char *fmt, ...) NORETURN;
static void die(int ec, const char *pfx, int eno,
		const char *fmt, ...) {
  va_list al;
  va_start(al,fmt);
  vdie(ec,pfx,eno,fmt,al);
}

static void diem(void) NORETURN;
static void diem(void) { die(16,0,errno,"malloc failed"); }
static void *xmalloc(size_t sz) {
  assert(sz);
  void *p= malloc(sz);
  if (!p) diem();
  return p;
}
static void *xrealloc(void *p, size_t sz) {
  assert(sz);
  p= realloc(p,sz);
  if (!p) diem();
  return p;
}

static void diee(const char *fmt, ...) NORETURN;
static void diee(const char *fmt, ...) {
  va_list al;
  va_start(al,fmt);
  vdie(12,0,errno,fmt,al);
}
static void die_protocol(const char *fmt, ...) NORETURN;
static void die_protocol(const char *fmt, ...) {
  va_list al;
  va_start(al,fmt);
  vdie(10,"protocol error",-1,fmt,al);
}

static void die_badrecv(const char *what) NORETURN;
static void die_badrecv(const char *what) {
  if (ferror(commsi)) diee("communication failed while receiving %s", what);
  if (feof(commsi)) die_protocol("receiver got unexpected EOF in %s", what);
  abort();
}
static void die_badsend(void) NORETURN;
static void die_badsend(void) {
  diee("transmission failed");
}

static void send_flush(void) {
  if (ferror(commso) || fflush(commso))
    die_badsend();
}
static void sendbyte(int c) {
  if (putc(c,commso)==EOF)
    die_badsend();
}

static void mfreadcommsi(void *buf, int l, const char *what) {
  int r= fread(buf,1,l,commsi);  if (r!=l) die_badrecv(what);
}
static void mfwritecommso(const void *buf, int l) {
  int r= fwrite(buf,1,l,commso);  if (r!=l) die_badsend();
}

static void mpipe(int p[2]) { if (pipe(p)) diee("could not create pipe"); }
static void mdup2(int fd, int fd2) {
  if (dup2(fd,fd2)!=fd2) diee("could not dup2(%d,%d)",fd,fd2);
}

typedef void copyfile_die_fn(FILE *f, const char *xi);

struct timespec ts_sendstart;

static void mgettime(struct timespec *ts) {
  int r= clock_gettime(CLOCK_MONOTONIC, ts);
  if (r) diee("clock_gettime failed");
}

static void bandlimit_sendstart(void) {
  mgettime(&ts_sendstart);
}

static double mgettime_elapsed(struct timespec ts_base,
			       struct timespec *ts_ret) {
  mgettime(ts_ret);
  return (ts_ret->tv_sec - ts_base.tv_sec) +
         (ts_ret->tv_nsec - ts_base.tv_nsec)*1e-9;
}

static void flushstderr(void) {
  if (ferror(stderr) || fflush(stderr))
    diee("could not write progress to stderr");
}

static void verbosespinprintf(const char *fmt, ...) {
  static const char spinnerchars[]= "/-\\";
  static int spinnerchar;

  if (!verbose)
    return;

  va_list al;
  va_start(al,fmt);
  fprintf(stderr,"      %c ",spinnerchars[spinnerchar]);
  spinnerchar++; spinnerchar %= sizeof(spinnerchars)-1;
  vfprintf(stderr,fmt,al);
  putc('\r',stderr);
  flushstderr();
}

static void bandlimit_sendend(uint64_t bytes, int *interval_usec_update) {
  struct timespec ts_buf;
  double elapsed= mgettime_elapsed(ts_sendstart, &ts_buf);
  double secsperbyte_observed= elapsed / bytes;

  double min_update= elapsed / max_bw_prop;
  if (min_update > 1e3) min_update= 1e3;
  int min_update_usec= min_update * 1e6;

  if (*interval_usec_update < min_update_usec)
    *interval_usec_update= min_update_usec;

  verbosespinprintf("%12lluby %10.3fs %13.2fkby/s %8dms",
		    (unsigned long long)bytes, elapsed,
		    1e-3/secsperbyte_observed, *interval_usec_update/1000);
}
 
static void copyfile(FILE *sf, copyfile_die_fn *sdie, const char *sxi,
		     FILE *df, copyfile_die_fn *ddie, const char *dxi,
		     uint64_t lstart, int amsender) {
  int now, r;
  uint64_t l=lstart, done=0;

  while (l>0) {
    now= l < sizeof(mainbuf) ? l : sizeof(mainbuf);
    if (now > txblocksz) now= txblocksz;

    r= fread(mainbuf,1,now,sf);  if (r!=now) sdie(sf,sxi);
    r= fwrite(mainbuf,1,now,df);  if (r!=now) ddie(df,dxi);
    l -= now;
    done += now;

    if (verbose) {
      fprintf(stderr," %3d%% \r",
	      (int)(done*100.0/lstart));
      flushstderr();
    }
  }
}

static void copydie_inputfile(FILE *f, const char *filename) {
  diee("read failed on source file `%s'", filename);
}
static void copydie_tmpwrite(FILE *f, const char *tmpfilename) {
  diee("write failed to temporary receiving file `%s'", tmpfilename);
}
static void copydie_commsi(FILE *f, const char *what) {
  die_badrecv(what);
}
static void copydie_commso(FILE *f, const char *what) {
  die_badsend();
}
  
static int generate_declaration(void) {
  /* returns length; declaration is left in mainbuf */
  char *p= mainbuf;
  *p++= udchar;
  *p++= '\n';
  return p - mainbuf;
}

static void read_declaration(int decllen) {
  assert(decllen <= sizeof(mainbuf));
  if (decllen<2) die_protocol("declaration too short");
  mfreadcommsi(mainbuf,decllen,"declaration");
  if (mainbuf[decllen-1] != '\n')
    die_protocol("declaration missing final newline");
  if (mainbuf[0] != udchar)
    die_protocol("declaration incorrect direction indicator");
}

static void receiver(const char *filename) {
  FILE *newfile;
  char *tmpfilename;
  int r, c;

  char *lastslash= strrchr(filename,'/');
  if (!lastslash)
    r= asprintf(&tmpfilename, ".rcopy-repeatedly.#%s#", filename);
  else
    r= asprintf(&tmpfilename, "%.*s/.rcopy-repeatedly.#%s#",
		(int)(lastslash-filename), filename, lastslash+1);
  if (r==-1) diem();
  
  r= unlink(tmpfilename);
  if (r && errno!=ENOENT)
    diee("could not remove temporary receiving file `%s'", tmpfilename);
  
  for (;;) {
    send_flush();
    c= fgetc(commsi);

    switch (c) {

    case EOF:
      if (ferror(commsi)) die_badrecv("transfer message code");
      assert(feof(commsi));
      return;

    case REPLMSG_RM:
      r= unlink(filename);
      if (r && errno!=ENOENT)
	diee("source file removed but could not remove destination file `%s'",
	     filename);
      break;
      
    case REPLMSG_FILE64:
      newfile= fopen(tmpfilename, "wb");
      if (!newfile) diee("could not create temporary receiving file `%s'",
			 tmpfilename);
      uint8_t lbuf[8];
      mfreadcommsi(lbuf,8,"FILE64 l");

      uint64_t l=
	(lbuf[0] << 28 << 28) |
	(lbuf[1] << 24 << 24) |
	(lbuf[2] << 16 << 24) |
	(lbuf[3] <<  8 << 24) |
	(lbuf[4]       << 24) |
	(lbuf[5]       << 16) |
	(lbuf[6]       <<  8) |
	(lbuf[7]            ) ;

      copyfile(commsi, copydie_commsi,"FILE64 file data",
	       newfile, copydie_tmpwrite,tmpfilename,
	       l, 0);

      if (fclose(newfile)) diee("could not flush and close temporary"
				" receiving file `%s'", tmpfilename);
      if (rename(tmpfilename, filename))
	diee("could not install new version of destination file `%s'",
	     filename);

      sendbyte(REPLMSG_ACK);
      break;

    default:
      die_protocol("unknown transfer message code 0x%02x",c);

    }
  }
}

static void sender(const char *filename) {
  FILE *f, *fold;
  int interval_usec, r, c;
  struct stat stabtest, stab;
  enum { told_nothing, told_file, told_remove } told;

  interval_usec= 0;
  fold= 0;
  told= told_nothing;
  
  for (;;) {
    if (interval_usec) {
      send_flush();
      usleep(interval_usec);
    }
    interval_usec= min_interval_usec;

    r= stat(filename, &stabtest);
    if (r) {
      f= 0;
    } else {
      if (told == told_file &&
	  stabtest.st_mode  == stab.st_mode  &&
	  stabtest.st_dev   == stab.st_dev   &&
	  stabtest.st_ino   == stab.st_ino   &&
	  stabtest.st_mtime == stab.st_mtime &&
	  stabtest.st_size  == stab.st_size)
	continue;
      f= fopen(filename, "rb");
    }
    
    if (!f) {
      if (errno!=ENOENT) diee("could not access source file `%s'",filename);
      if (told != told_remove) {
	verbosespinprintf
	  (" ENOENT                                                    ");
	sendbyte(REPLMSG_RM);
	told= told_remove;
      }
      continue;
    }

    if (fold) fclose(fold);
    fold= 0;

    r= fstat(fileno(f),&stab);
    if (r) diee("could not fstat source file `%s'",filename);

    if (!S_ISREG(stab.st_mode))
      die(8,0,-1,"source file `%s' is not a plain file",filename);

    uint8_t hbuf[9]= {
      REPLMSG_FILE64,
      stab.st_size >> 28 >> 28,
      stab.st_size >> 24 >> 24,
      stab.st_size >> 16 >> 24,
      stab.st_size >>  8 >> 24,
      stab.st_size       >> 24,
      stab.st_size       >> 16,
      stab.st_size       >>  8,
      stab.st_size
    };

    bandlimit_sendstart();

    mfwritecommso(hbuf,9);

    copyfile(f, copydie_inputfile,filename,
	     commso, copydie_commso,0,
	     stab.st_size, 1);

    send_flush();

    c= fgetc(commsi);  if (c==EOF) die_badrecv("ack");
    if (c!=REPLMSG_ACK) die_protocol("got %#02x instead of ACK",c);

    bandlimit_sendend(stab.st_size, &interval_usec);

    fold= f;
    told= told_file;
  }
}

typedef struct {
  const char *userhost, *path;
} FileSpecification;

static FileSpecification srcspec, dstspec;

static void of__server(const struct cmdinfo *ci, const char *val) {
  int ncount= nsargs + 1 + !!val;
  sargs= xrealloc(sargs, sizeof(*sargs) * ncount);
  sargs[nsargs++]= ci->olong;
  if (val)
    sargs[nsargs++]= val;
}

static int of__server_int(const struct cmdinfo *ci, const char *val) {
  of__server(ci,val);
  long v;
  char *ep;
  errno= 0; v= strtol(val,&ep,10);
  if (!*val || *ep || errno || v<INT_MIN || v>INT_MAX)
    badusage("bad integer argument `%s' for --%s",val,ci->olong);
  return v;
}

static void of_help(const struct cmdinfo *ci, const char *val) {
  usagemessage();
  if (ferror(stdout)) diee("could not write usage message to stdout");
  exit(0);
}

static void of_bw(const struct cmdinfo *ci, const char *val) {
  int pct= of__server_int(ci,val);
  if (pct<1 || pct>100)
    badusage("bandwidth percentage must be between 1 and 100 inclusive");
  *(double*)ci->parg= pct * 0.01;
}

static void of_server_int(const struct cmdinfo *ci, const char *val) {
  *(int*)ci->parg= of__server_int(ci,val);
}

void usagemessage(void) {
  printf(
	 "usage: rcopy-repeatedly [<options>] <file> <file>\n"
	 "  <file> may be <local-file> or [<user>@]<host>:<file>\n"
	 "  exactly one of each of the two forms must be provided\n"
	 "  a file is taken as remote if it has a : before the first /\n"
	 "general options:\n"
	 "  --help\n"
	 "  --quiet | -q\n"
	 "options for bandwidth (and cpu time) control:\n"
	 "  --max-bandwidth-percent  (default %d)\n"
	 "  --tx-block-size      (default/max %d)\n"
	 "  --min-interval-usec  (default %d)\n"
	 "options for finding programs:\n"
	 "  --rcopy-repeatedly  (default: rcopy-repeatedly)\n"
	 "  --rsh-program       (default: $RCOPY_REPEATEDLY_RSH or $RSYNC_RSH or ssh)\n"
	 "options passed to server side via ssh:\n"
	 "  --receiver --sender, bandwidth control options\n",
         (int)(max_bw_prop*100), (int)sizeof(mainbuf), min_interval_usec);
}

static const struct cmdinfo cmdinfos[]= {
  { "help",     .call= of_help },
  { "max-bandwidth-percent", 0,1,.call=of_bw,.parg=&max_bw_prop            },
  { "tx-block-size",0,     1,.call=of_server_int, .parg=&txblocksz         },
  { "min-interval-usec",0, 1,.call=of_server_int, .parg=&min_interval_usec },
  { "rcopy-repeatedly",0,  1, .sassignto=&rcopy_repeatedly_program         },
  { "rsh-program",0,       1, .sassignto=&rsh_program                      },
  { "quiet",'q',  .iassignto= &verbose,       .arg=0                       },
  { "receiver",   .iassignto= &server_upcopy, .arg=0                       },
  { "sender",     .iassignto= &server_upcopy, .arg=1                       },
  { 0 }
};

static void server(const char *filename) {
  int c, l;
  char buf[2];

  udchar= server_upcopy?'u':'d';

  commsi= stdin;
  commso= stdout;
  l= generate_declaration();
  fprintf(commso, "%s%04x\n", banner, l);
  mfwritecommso(mainbuf, l);
  send_flush();

  c= fgetc(commsi);
  if (c==EOF) {
    if (feof(commsi)) exit(14);
    assert(ferror(commsi));  die_badrecv("initial START message");
  }
  if (c!=REPLMSG_START) die_protocol("initial START was %#02x instead",c);

  mfreadcommsi(buf,2,"START l");
  l= (buf[0] << 8) | buf[1];

  read_declaration(l);

  if (server_upcopy)
    sender(filename);
  else
    receiver(filename);
}

static void client(void) {
  int uppipe[2], downpipe[2], r;
  pid_t child;
  FileSpecification *remotespec;
  const char *remotemode;

  mpipe(uppipe);
  mpipe(downpipe);

  if (srcspec.userhost) {
    udchar= 'u';
    remotespec= &srcspec;
    remotemode= "--sender";
  } else {
    udchar= 'd';
    remotespec= &dstspec;
    remotemode= "--receiver";
  }

  sargs= xrealloc(sargs, sizeof(*sargs) * (7 + nsargs));
  memmove(sargs+5, sargs, sizeof(*sargs) * nsargs);
  sargs[0]= rsh_program;
  sargs[1]= remotespec->userhost;
  sargs[2]= rcopy_repeatedly_program;
  sargs[3]= remotemode;
  sargs[4]= "--";
  sargs[5+nsargs]= remotespec->path;
  sargs[6+nsargs]= 0;
    
  child= fork();
  if (child==-1) diee("fork failed");
  if (!child) {
    mdup2(downpipe[0],0);
    mdup2(uppipe[1],1);
    close(uppipe[0]); close(downpipe[0]);
    close(uppipe[1]); close(downpipe[1]);

    execvp(rsh_program, (char**)sargs);
    diee("failed to execute rsh program `%s'",rsh_program);
  }

  commso= fdopen(downpipe[1],"wb");
  commsi= fdopen(uppipe[0],"rb");
  if (!commso || !commsi) diee("fdopen failed");
  close(downpipe[0]);
  close(uppipe[1]);
  
  char banbuf[sizeof(banner)-1 + 5 + 1];
  r= fread(banbuf,1,sizeof(banbuf)-1,commsi);
  if (ferror(commsi)) die_badrecv("read banner");

  if (r!=sizeof(banbuf)-1 ||
      memcmp(banbuf,banner,sizeof(banner)-1) ||
      banbuf[sizeof(banner)-1 + 4] != '\n') {
    const char **sap;
    int count=0;
    for (count=0, sap=sargs; *sap; sap++) count+= strlen(*sap)+1;
    char *cmdline= xmalloc(count+1);
    cmdline[0]=' ';
    for (sap=sargs; *sap; sap++) {
      strcat(cmdline," ");
      strcat(cmdline,*sap);
    }
    
    die(8,0,-1,"did not receive banner as expected -"
	" shell dirty? ssh broken?\n"
	" try running\n"
	"  %s\n"
	" and expect the first line to be\n"
	"  %s",
	cmdline, banner);
  }
  
  banbuf[sizeof(banbuf)-1]= 0;
  char *ep;
  long decllen= strtoul(banbuf + sizeof(banner)-1, &ep, 16);
  if (ep != banbuf + sizeof(banner)-1 + 4)
    die_protocol("declaration length syntax error");

  read_declaration(decllen);

  int l= generate_declaration();
  sendbyte(REPLMSG_START);
  sendbyte((l >> 8) & 0x0ff);
  sendbyte( l       & 0x0ff);
  mfwritecommso(mainbuf,l);

  if (remotespec==&srcspec)
    receiver(dstspec.path);
  else
    sender(srcspec.path);
}

static void parse_file_specification(FileSpecification *fs, const char *arg,
				     const char *what) {
  const char *colon;
  
  if (!arg) badusage("too few arguments - missing %s\n",what);

  for (colon=arg; ; colon++) {
    if (!*colon || *colon=='/') {
      fs->userhost=0;
      fs->path= arg;
      return;
    }
    if (*colon==':') {
      char *uh= xmalloc(colon-arg + 1);
      memcpy(uh,arg, colon-arg);  uh[colon-arg]= 0;
      fs->userhost= uh;
      fs->path= colon+1;
      return;
    }
  }
}

int main(int argc, const char *const *argv) {
  setvbuf(stderr,0,_IOLBF,BUFSIZ);

  myopt(&argv, cmdinfos);

  if (!rsh_program) rsh_program= getenv("RCOPY_REPEATEDLY_RSH");
  if (!rsh_program) rsh_program= getenv("RSYNC_RSH");
  if (!rsh_program) rsh_program= "ssh";

  if (txblocksz<1) badusage("transmit block size must be at least 1");
  if (min_interval_usec<0) badusage("minimum update interval may not be -ve");

  if (server_upcopy>=0) {
    if (!argv[0] || argv[1])
      badusage("server mode must have just the filename as non-option arg");
    server(argv[0]);
  } else {
    parse_file_specification(&srcspec, argv[0], "source");
    parse_file_specification(&dstspec, argv[1], "destination");
    if (argv[2]) badusage("too many non-option arguments");
    if (!!srcspec.userhost == !!dstspec.userhost)
      badusage("need exactly one remote file argument");
    client();
  }
  return 0;
}
