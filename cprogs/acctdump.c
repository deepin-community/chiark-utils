/*
 * acctdump.c - accounting data dump utility
 *
 * Copyright (C) 1998 Ian Jackson <ian@chiark.greenend.org.uk>
 *
 * This is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 2,
 * or (at your option) any later version.
 *
 * This is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public
 * License along with this file; if not, write to the Free Software
 * Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 */

#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <stdarg.h>
#include <wait.h>
#include <string.h>
#include <pwd.h>
#include <grp.h>
#include <dirent.h>
#include <ctype.h>
#include <errno.h>
#include <sys/stat.h>

typedef unsigned long long u64;


/* Sadly this thing is not very portable */

#if defined(__linux__)

#include <sys/types.h>
#include <sys/acct.h>

typedef struct acct_v3 struct_acct;
#define HAVE_AC_EXITCODE
#define HAVE_AC_FLT
#define FIELD_AC_FLAG(as) ((as)->ac_flag)

#elif defined(__FreeBSD__) || defined(__FreeBSD_kernel__)

#include <sys/param.h>
#include <sys/types.h>
#include <sys/acct.h>

typedef struct acctv2 struct_acct;
#define HAVE_AC_IO_MEM
#define FIELD_AC_FLAG(as) ((as)->ac_flagx & ~ANVER)

#else

#error Do not know what struct_acct to use on this platform

#endif


#include "myopt.h"

static int forwards, nobanner, usestdin, raw, usages;

static int de_used, de_allocd;
static struct deventry {
  const char *fn;
  dev_t dev;
} *deventries;

static const struct cmdinfo cmdinfos[]= {
  { "--forwards",  'f',  0, &forwards, 0, 0, 1, 0, 0 },
  { "--no-banner", 'q',  0, &nobanner, 0, 0, 1, 0, 0 },
  { "--stdin",     'p',  0, &usestdin, 0, 0, 1, 0, 0 },
  { "--raw",       'r',  0, &raw,      0, 0, 1, 0, 0 },
  { "--resource",  'u',  0, &usages,   0, 0, 1, 0, 0 },
  {  0                                                 }
};

#ifdef HAVE_AC_EXITCODE
static const char *sigabbrev[]= {
  "HUP", "INT", "QUIT", "ILL", "TRAP", "ABRT", "BUS", "FPE",
  "KILL", "USR1", "SEGV", "USR2", "PIPE", "ALRM", "TERM", "STKFLT",
  "CHLD", "CONT", "STOP", "TSTP", "TTIN", "TTOU", "URG", "XCPU",
  "XFSZ", "VTALRM", "PROF", "WINCH", "IO"
};
#endif

void usagemessage(void) {
  fputs("usage: acctdump [<options>] [<file> ...]\n"
	"options: -f|--forwards -q|--no-banner -p|--stdin -r|--raw -u|--resource\n",
	stderr);
  if (ferror(stderr)) { perror("print usage"); exit(8); }
}

static void checkstdout(void) {
  if (ferror(stdout)) { perror("stdout"); exit(8); }
}

static void scandev(const char *basename, int levelsleft) {
  /* We deliberately ignore most errors */
  DIR *dir;
  struct dirent *de;
  struct stat stab;
  int fnbufalloc, fnbufreq, r, basel, nallocd;
  char *fnbuf, *nfnbuf;
  struct deventry *ndeventries;
  
  if (levelsleft==0) return;

  dir= opendir(basename); 
  if (!dir) {
    fprintf(stderr, "%s: opendir: %s\n", basename, strerror(errno));
    return;
  }
  fnbufalloc= 0;
  fnbuf= 0;
  basel= strlen(basename);

  while ((errno=0, de= readdir(dir))) {
    fnbufreq= basel+1+strlen(de->d_name)+1;
    if (fnbufalloc<fnbufreq) {
      fnbufalloc= fnbufreq+10;
      nfnbuf= realloc(fnbuf,fnbufalloc);
      if (!nfnbuf) { free(fnbuf); fnbufalloc=0; continue; }
      fnbuf= nfnbuf;
    }
    sprintf(fnbuf,"%s/%s",basename,de->d_name);
    r= lstat(fnbuf,&stab);
    if (r) {
      fprintf(stderr, "%s: %s\n", fnbuf, strerror(errno));
      continue;
    }
    if (S_ISCHR(stab.st_mode)) {
      if (de_used >= de_allocd) {
	nallocd= (de_allocd+10)<<1;
	ndeventries= realloc(deventries,nallocd*sizeof(*deventries));
	if (!ndeventries) continue;
	de_allocd= nallocd;
	deventries= ndeventries;
      }
      deventries[de_used].fn= strdup(fnbuf+5); /* remove /dev */
      if (!deventries[de_used].fn) continue;
      deventries[de_used].dev= stab.st_rdev;
      de_used++;
    } else if (S_ISDIR(stab.st_mode) && de->d_name[0] != '.') {
      scandev(fnbuf,levelsleft-1);
    }
  }
  if (errno)
      fprintf(stderr, "%s: readdir: %s\n", basename, strerror(errno));
  closedir(dir);
  free(fnbuf);
}

static int walkdev_cptr(const void *av, const void *bv) {
  const struct deventry *a= av;
  const struct deventry *b= bv;
  return a->dev - b->dev;
}
  
static void printbanner(void) {
  if (raw) {
    fputs("begin date command          "
	  "uid      gid      tty dev  FSDX "
#ifdef HAVE_AC_EXITCODE
	  "exit"
#endif
	  , stdout);
  } else {
    fputs("begin date and time command          "
	  "user     group    tty dev    FSDX "
#ifdef HAVE_AC_EXITCODE
	  "sigexit"
#endif
	  , stdout);
  }
  if (usages) {
    fputs("  user time   sys time  elap time "
#ifdef HAVE_AC_FLT
	  "  minflt   maxflt"
#endif
#ifdef HAVE_AC_IO_MEM
	  "  avg.mem      io"
#endif
	  , stdout);
  }
  putchar('\n');
  checkstdout();
}

static void printrecord(const struct_acct *as, const char *filename) {
  static int walkeddev;

  int i, r;
  const char *fp;
  char buf[100];
  struct tm *tm;
  struct deventry *deve, devlookfor;
  struct passwd *pw;
  struct group *gr;
  time_t btime;
  char commbuf[sizeof(as->ac_comm)];

  if (raw) {
    printf("%10lu ",(unsigned long)as->ac_btime);
  } else {
    btime= as->ac_btime;
    tm= localtime(&btime);
    if (tm) {
      strftime(buf,sizeof(buf),"%Y-%m-%d %H:%M:%S",tm); buf[sizeof(buf)-1]= 0;
    } else {
      snprintf(buf,sizeof(buf),"@%lu",(unsigned long)btime);
    }
    printf("%19s ",buf);
  }

  for (i=0; i<sizeof(as->ac_comm); i++) {
    int c=as->ac_comm[i];
    commbuf[i]= ((c!=0 && c<=32) || c>=127) ? '?' : c;
  }
  printf("%-*.*s ", (int)sizeof(commbuf),(int)sizeof(commbuf), commbuf);
  
  pw= raw ? 0 : getpwuid(as->ac_uid);
  if (pw) printf("%-8s ",pw->pw_name);
  else printf("%-8ld ",(long)as->ac_uid);
  
  gr= raw ? 0 : getgrgid(as->ac_gid);
  if (gr) printf("%-8s ",gr->gr_name);
  else printf("%-8ld ",(long)as->ac_gid);

  if (raw) {
    if (!(as->ac_tty + 1) /* check for -1 without knowing type */) {
      printf("-        ");
    } else {
      printf("%08lx ",(unsigned long)as->ac_tty);
    }
  } else {
    if (!(as->ac_tty + 1)) {
      printf("-          ");
    } else {
      if (!walkeddev) {
	scandev("/dev",4);
	qsort(deventries,de_used,sizeof(*deventries),walkdev_cptr);
	walkeddev= 1;
      }
      devlookfor.fn= 0;
      devlookfor.dev= as->ac_tty;
      deve= bsearch(&devlookfor,deventries,de_used,sizeof(*deventries),walkdev_cptr);
      if (deve) {
	printf("%-10s ",deve->fn);
      } else {
	printf("%08lx   ",(unsigned long)as->ac_tty);
      }
    }
  }

  r= FIELD_AC_FLAG(as);
  for (i=1, fp= "FS4DX"; *fp; fp++, i<<=1) {
    if (r&i) {
      putchar(*fp);
      r &= ~i;
    } else if (!isdigit(*fp)) {
      putchar(' ');
    }
  }
  if (r) {
    printf("#%x",r);
  }
  putchar(' ');

#ifdef HAVE_AC_EXITCODE
  int dc;
  dc= WCOREDUMP(as->ac_exitcode) ? 'd' : 'k';
  if (raw) {
    if (WIFEXITED(as->ac_exitcode)) {
      printf(" %3d",WEXITSTATUS(as->ac_exitcode));
    } else if (WIFSIGNALED(as->ac_exitcode)) {
      printf("%c%3d",
	     dc,
	     WTERMSIG(as->ac_exitcode));
    } else {
      printf("%04lx",(unsigned long)as->ac_exitcode);
    }
  } else {
    if (WIFEXITED(as->ac_exitcode)) {
      printf(" %6d",WEXITSTATUS(as->ac_exitcode));
    } else if (WIFSIGNALED(as->ac_exitcode)) {
      r= WTERMSIG(as->ac_exitcode);
      if (r>0 && r<=sizeof(sigabbrev)/sizeof(*sigabbrev)) {
	printf("%c%6s",
	       dc,
	       sigabbrev[r-1]);
      } else {
	printf("%cSIG%-3d",
	       dc,
	       r);
      }
    } else {
      printf("#%04lx",(unsigned long)as->ac_exitcode);
    }
  }
#endif /*HAVE_AC_EXITCODE*/

  if (usages) {
    printf(" %10lu %10lu %10lu",
	   (unsigned long)as->ac_utime,
	   (unsigned long)as->ac_stime,
	   (unsigned long)as->ac_etime);
#ifdef HAVE_AC_FLT
    printf(" %8lu %8lu",
	   (unsigned long)as->ac_minflt,
	   (unsigned long)as->ac_majflt);
#endif
#ifdef HAVE_AC_IO_MEM
    printf(" %4e %4e",
	   as->ac_mem,
	   as->ac_io);
#endif
  }
  putchar('\n');

  checkstdout();
}

static void processfile(FILE *file, const char *filename) {
  struct_acct as;
  long pos;
  int r;
  
  if (forwards) {
    while ((r= fread(&as,1,sizeof(as),file)) == sizeof(as)) {
      printrecord(&as,filename);
    }
  } else {
    r= fseek(file,0,SEEK_END); if (r) { perror(filename); exit(8); }
    pos= ftell(file); if (pos==-1) { perror(filename); exit(8); }
    if (pos % sizeof(as)) { 
      fprintf(stderr, "%s: File size is not an integral number "
	      "of accounting records\n", filename);
      exit(8);
    }
    for (;;) {
      if (pos<sizeof(as)) break;
      pos -= sizeof(as);
      r= fseek(file,pos,SEEK_SET); if (r==-1) { perror(filename); exit(8); }
      r= fread(&as,1,sizeof(as),file); if (r!=sizeof(as)) { perror(filename); exit(8); }
      printrecord(&as,filename);
    }
  }
  if (ferror(file) || fclose(file)) { perror(filename); exit(8); }
}

static void processnamedfile(const char *filename) {
  FILE *file;

  file= fopen(filename,"rb"); if (!file) { perror(filename); exit(8); }
  processfile(file,filename);
}

int main(int argc, const char *const *argv) {
  myopt(&argv,cmdinfos);
  if (!nobanner) printbanner();
  if (usestdin) {
    processfile(stdin,"<standard input>");
  } else if (!*argv) {
    processnamedfile("/var/log/account/pacct");
  } else {
    while (*argv) {
      processnamedfile(*argv);
      argv++;
    }
  }
  checkstdout();
  if (fflush(stdout)) { perror("flush stdout"); exit(8); }
  return 0;
}
