/*
 * summer - program for summarising (with md5 checksums) filesystem trees
 *
 * usage:
 *    cat startpoints.list | summer >data.list
 *    summer startpoints... >data.list
 *  prints md5sum of data-list to stderr
 */
/*
 * Copyright (C) 2003,2006-2007 Ian Jackson <ian@davenant.greenend.org.uk>
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

#define _GNU_SOURCE

#include <search.h>
#include <dirent.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <stdio.h>
#include <inttypes.h>
#include <string.h>
#include <errno.h>
#include <stdarg.h>
#include <limits.h>
#include <assert.h>
#include <stdlib.h>

#include "nettle/md5-compat.h"

#define MAXFN 2048
#define MAXDEPTH 1024
#define CSUMXL 32

static int quiet=0, hidectime=0, hideatime=0, hidemtime=0;
static int hidedirsize=0, hidelinkmtime=0, hidextime=0, onefilesystem=0;
static int filenamefieldsep=' ';
static FILE *errfile;

#define nodeflag_fsvalid       1u

static void malloc_fail(void) { perror("summer: alloc failed"); exit(12); }

static void *mmalloc(size_t sz) {
  void *r;
  r= malloc(sz);  if (!r) malloc_fail();
  return r;
}

static void *mrealloc(void *p, size_t sz) {
  void *r;
  r= realloc(p, sz);  if (!r && sz) malloc_fail();
  return r;
}

static void fn_escaped(FILE *f, const char *fn) {
  int c;
  while ((c= *fn++)) {
    if (c>=33 && c<=126 && c!='\\') putc(c,f);
    else fprintf(f,"\\x%02x",(int)(unsigned char)c);
  }
}

static void add_pr(int *pr, int printf_ret) {
  if (printf_ret == EOF) return;
  *pr += printf_ret;
}

static void vproblemx(const char *path, int padto, int per,
		      const char *fmt, va_list al) {
  int e=errno, pr=0;
  
  if (errfile==stderr) fputs("summer: error: ",stderr);
  else add_pr(&pr, fprintf(errfile,"\\["));
  
  add_pr(&pr, vfprintf(errfile,fmt,al));
  if (per) add_pr(&pr, fprintf(errfile,": %s",strerror(e)));

  if (errfile==stderr) {
    fputs(": ",stderr);
    fn_escaped(stderr,path);
    fputc('\n',stderr);
    exit(2);
  }

  add_pr(&pr, printf("]"));

  while (pr++ < padto)
    putchar(' ');
}  

static void problem_e(const char *path, int padto, const char *fmt, ...) {
  va_list(al);
  va_start(al,fmt);
  vproblemx(path,padto,1,fmt,al);
  va_end(al);
}

static void problem(const char *path, int padto, const char *fmt, ...) {
  va_list(al);
  va_start(al,fmt);
  vproblemx(path,padto,0,fmt,al);
  va_end(al);
}

static void csum_file(const char *path) {
  FILE *f;
  MD5_CTX mc;
  char db[65536];
  unsigned char digest[16];
  size_t r;
  int i;

  f= fopen(path,"rb");
  if (!f) { problem_e(path,sizeof(digest)*2,"open"); return; }
  
  MD5Init(&mc);
  for (;;) {
    r= fread(db,1,sizeof(db),f);
    if (ferror(f)) {
      problem_e(path,sizeof(digest)*2,"read");
      fclose(f); return;
    }
    if (!r) { assert(feof(f)); break; }
    MD5Update(&mc,db,r);
  }
  MD5Final(digest,&mc);
  if (fclose(f)) { problem_e(path,sizeof(digest)*2,"close"); return; }

  for (i=0; i<sizeof(digest); i++)
    printf("%02x", digest[i]);
}

static void csum_dev(int cb, const struct stat *stab) {
  printf("%c 0x%08lx %3lu %3lu %3lu %3lu    ", cb,
	 (unsigned long)stab->st_rdev,
	 ((unsigned long)stab->st_rdev & 0x0ff000000U) >> 24,
	 ((unsigned long)stab->st_rdev & 0x000ff0000U) >> 16,
	 ((unsigned long)stab->st_rdev & 0x00000ff00U) >> 8,
	 ((unsigned long)stab->st_rdev & 0x0000000ffU) >> 0);
}

static void csum_str(const char *s) {
  printf("%-*s", CSUMXL, s);
}

static void linktargpath(const char *linktarg) {
  printf(" -> ");
  fn_escaped(stdout, linktarg);
}

static void pu10(void) { printf(" %10s", "?"); }

#define PTIME(stab, memb)  ((stab) ? ptime((stab), (stab)->memb) : pu10())

static void ptime(const struct stat *stab, uint64_t val) {
  const char *instead;

  if (!hidextime) goto justprint;
  else if (S_ISCHR(stab->st_mode)) instead= "char";
  else if (S_ISBLK(stab->st_mode)) instead= "block";
  else if (S_ISLNK(stab->st_mode)) instead= "link";
  else if (S_ISSOCK(stab->st_mode)) instead= "sock";
  else if (S_ISFIFO(stab->st_mode)) instead= "pipe";
  else {
  justprint:
    printf(" %10" PRIu64 "", val);
    return;
  }

  printf(" %10s",instead);
}

struct hardlink {
  dev_t dev;
  ino_t ino;
  char path[1];
};
static void *hardlinks;

static int hardlink_compar(const void *av, const void *bv) {
  const struct hardlink *a=av, *b=bv;
  if (a->ino != b->ino) return b->ino - a->ino;
  return b->dev - a->dev;
}

static void recurse(const char *path, unsigned nodeflags, dev_t fs);

static void node(const char *path, unsigned nodeflags, dev_t fs) {
  char linktarg[MAXFN+1];
  struct hardlink *foundhl;
  const struct stat *stab;
  struct stat stabuf;
  int r, mountpoint=0;

  r= lstat(path, &stabuf);
  stab= r ? 0 : &stabuf;

  foundhl= 0;
  if (stab && stab->st_nlink>1) {
    struct hardlink *newhl, **foundhl_node;
    newhl= mmalloc(sizeof(*newhl) + strlen(path));
    newhl->dev= stab->st_dev;
    newhl->ino= stab->st_ino;
    foundhl_node= tsearch(newhl, &hardlinks, hardlink_compar);
    if (!foundhl_node) malloc_fail();
    foundhl= *foundhl_node;
    if (foundhl!=newhl) {
      free(newhl); /* hardlink to an earlier object */
    } else {
      foundhl= 0; /* new object with link count>1 */
      strcpy(newhl->path, path);
    }
  }

  if (stab) {
    if ((nodeflags & nodeflag_fsvalid) && stab->st_dev != fs)
      mountpoint= 1;
    fs= stab->st_dev;
    nodeflags |= nodeflag_fsvalid;
  }

  if (!stab) problem_e(path,CSUMXL,"inaccessible");
  else if (foundhl) csum_str("hardlink");
  else if (S_ISREG(stab->st_mode)) csum_file(path);
  else if (S_ISCHR(stab->st_mode)) csum_dev('c',stab);
  else if (S_ISBLK(stab->st_mode)) csum_dev('b',stab);
  else if (S_ISFIFO(stab->st_mode)) csum_str("pipe");
  else if (S_ISLNK(stab->st_mode)) csum_str("symlink");
  else if (S_ISSOCK(stab->st_mode)) csum_str("sock");
  else if (S_ISDIR(stab->st_mode)) csum_str(mountpoint ? "mountpoint" : "dir");
  else problem(path,CSUMXL,"badobj: 0x%lx", (unsigned long)stab->st_mode);

  if (stab && S_ISLNK(stab->st_mode)) {
    r= readlink(path, linktarg, sizeof(linktarg)-1);
    if (r==sizeof(linktarg)) { problem(path,-1,"readlink too big"); r=-1; }
    else if (r<0) { problem_e(path,-1,"readlink"); }
    else assert(r<sizeof(linktarg));

    if (r<0) strcpy(linktarg,"\\?");
    else linktarg[r]= 0;
  }

  if (stab) {
    if (S_ISDIR(stab->st_mode) && hidedirsize)
      printf(" %10s","dir");
    else
      printf(" %10llu", 
	     (unsigned long long)stab->st_size);

    printf(" %4o %10ld %10ld",
	   (unsigned)stab->st_mode & 07777U,
	   (unsigned long)stab->st_uid,
	   (unsigned long)stab->st_gid);
  } else {
    printf(" %10s %4s %10s %10s", "?","?","?","?");
  }

  if (!hideatime)
    PTIME(stab, st_atime);

  if (!hidemtime) {
    if (stab && S_ISLNK(stab->st_mode) && hidelinkmtime)
      printf(" %10s","link");
    else
      PTIME(stab, st_mtime);
  }

  if (!hidectime)
    PTIME(stab, st_ctime);

  putchar(filenamefieldsep);
  fn_escaped(stdout, path);

  if (foundhl) linktargpath(foundhl->path);
  if (stab && S_ISLNK(stab->st_mode)) linktargpath(linktarg);

  putchar('\n');

  if (ferror(stdout)) { perror("summer: stdout"); exit(12); }

  if (stab && S_ISDIR(stab->st_mode) && !(mountpoint && onefilesystem))
    recurse(path, nodeflags, fs);
}

static void process(const char *startpoint) {
  if (!quiet)
    fprintf(stderr,"summer: processing: %s\n",startpoint);
  node(startpoint, 0,0);
  tdestroy(hardlinks,free);
  hardlinks= 0;
}

static int recurse_maxlen;

static int recurse_filter(const struct dirent *de) {
  int l;
  if (de->d_name[0]=='.' &&
      (de->d_name[1]==0 ||
       (de->d_name[1]=='.' &&
	de->d_name[2]==0)))
    return 0;
  l= strlen(de->d_name);
  if (l > recurse_maxlen) recurse_maxlen= l;
  return 1;
}

static int recurse_compar(const struct dirent **a, const struct dirent **b) {
  return strcmp((*a)->d_name, (*b)->d_name);
}

static void recurse(const char *path_or_buf, unsigned nodeflags, dev_t fs) {
  static char *buf;
  static int buf_allocd;
  
  struct dirent **namelist, *const *de;
  const char *path_or_0= path_or_buf==buf ? 0 : path_or_buf;
  int nentries, pathl, esave, buf_want, i;

  pathl= strlen(path_or_buf);
  recurse_maxlen= 2;
  nentries= scandir(path_or_buf, &namelist, recurse_filter, recurse_compar);
  esave= errno;
  
  buf_want= pathl+1+recurse_maxlen+1;
  if (buf_want > buf_allocd) {
    buf= mrealloc(buf, buf_want);
    buf_allocd= buf_want;
  }
  /* NOTE that path_or_buf is invalid after this point because
   * it might have been realloc'd ! */
  if (path_or_0) strcpy(buf,path_or_0);

  buf[pathl]= '/';
  pathl++;
  if (nentries < 0) {
    buf[pathl]= 0;  errno= esave;
    problem_e(buf,CSUMXL+72,"scandir failed");
    fn_escaped(stdout,buf);  putchar('\n');
    return;
  }
  for (i=0, de=namelist; i<nentries; i++, de++) {
    strcpy(buf+pathl, (*de)->d_name);
    node(buf, nodeflags, fs);
    free(*de);
  }
  free(namelist);
}

static void from_stdin(void) {
  char buf[MAXFN+2];
  char *s;
  int l;

  if (!quiet)
    fprintf(stderr, "summer: processing stdin lines as startpoints\n");
  for (;;) {
    s= fgets(buf,sizeof(buf),stdin);
    if (ferror(stdin)) { perror("summer: stdin"); exit(12); }
    if (!s) { if (feof(stdin)) return; else abort(); }
    l= strlen(buf);
    assert(l>0);
    if (buf[l-1]!='\n') { fprintf(stderr,"summer: line too long\n"); exit(8); }
    buf[l-1]= 0;
    process(buf);
  }
}

int main(int argc, const char *const *argv) {
  const char *arg;
  int c;

  errfile= stderr;
  
  while ((arg=argv[1]) && *arg++=='-') {
    while ((c=*arg++)) {
      switch (c) {
      case 'h':
	fprintf(stderr,
		"summer: usage: summer startpoint... >data.list\n"
		"               cat startpoints.list | summer >data.list\n");
	exit(8);
      case 'q':
	quiet= 1;
	break;
      case 't':
	filenamefieldsep= '\t';
	break;
      case 'D':
	hidedirsize= 1;
	break;
      case 'b':
	hidelinkmtime= 1;
	break;
      case 'B':
	hidextime= 1;
	break;
      case 'x':
	onefilesystem= 1;
	break;
      case 'C':
	hidectime= 1;
	break;
      case 'A':
	hideatime= 1;
	break;
      case 'M':
	hidemtime= 1;
	break;
      case 'f':
	errfile= stdout;
	break;
      default:
	fprintf(stderr,"summer: bad usage, try -h\n");
	exit(8);
      }
    }
    argv++;
  }

  if (!argv[1]) {
    from_stdin();
  } else {
    if (!quiet)
      fprintf(stderr, "summer: processing command line args as startpoints\n");
    while ((arg=*++argv)) {
      process(arg);
    }
  }
  if (ferror(stdout) || fclose(stdout)) {
    perror("summer: stdout (at end)"); exit(12);
  }
  if (!quiet)
    fputs("summer: done.\n", stderr);
  return 0;
}
