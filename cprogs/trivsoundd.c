/*
 * triv-sound-d.c
 * writebuffer adapted for sound-playing
 *
 * readbuffer and writebuffer are:
 *  Copyright (C) 1997-1998,2000-2001 Ian Jackson <ian@chiark.greenend.org.uk>
 *
 * readbuffer is part of chiark backup, a system for backing up GNU/Linux and
 * other UN*X-compatible machines, as used on chiark.greenend.org.uk.
 * chiark backup is:
 *  Copyright (C) 1997-1998,2000-2001 Ian Jackson <ian@chiark.greenend.org.uk>
 *  Copyright (C) 1999 Peter Maydell <pmaydell@chiark.greenend.org.uk>
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
 *
 */

#include "rwbuffer.h"

const char *progname= "trivsoundd";

static int maxstartdelay=60, maxbadaccept=10;

struct inqnode {
  struct inqnode *next, *back;
  time_t accepted;
  int fd;
};

static struct { struct inqnode *head, *tail; } inq;
static int master, sdev;
static time_t now;

static void usageerr(const char *m) {
  fprintf(stderr,"bad usage: %s\n",m);
  exit(12);
}

static void bindmaster(const char *bindname) {
  union {
    struct sockaddr sa;
    struct sockaddr_in sin;
    struct sockaddr_un sun;
  } su;
  socklen_t sulen;
  const char *colon;
  char *copy, *ep;
  int r;
  unsigned long portul;
  struct hostent *he;
  struct servent *se;

  memset(&su,0,sizeof(su));

  if (bindname[0]=='/' || bindname[0]=='.') {

    if (strlen(bindname) >= sizeof(su.sun.sun_path))
      usageerr("AF_UNIX bind path too long");
    sulen= sizeof(su.sun);
    su.sun.sun_family= AF_UNIX;
    strcpy(su.sun.sun_path, bindname);

  } else if (bindname[0] != ':' && (colon= strrchr(bindname,':'))) {

    sulen= sizeof(su.sin);
    su.sin.sin_family= AF_INET;

    copy= xmalloc(colon - bindname + 1);
    memcpy(copy,bindname, colon - bindname + 1);
    copy[colon - bindname]= 0;
    portul= strtoul(colon+1,&ep,0);

    if (!*ep) {
      if (!portul || portul>=65536) usageerr("invalid port number");
      su.sin.sin_port= htons(portul);
    } else {
      se= getservbyname(colon+1, "tcp");
      if (!se) { fprintf(stderr,"unknown service `%s'\n",colon+1); exit(4); }
      su.sin.sin_port= htons(se->s_port);
    }

    if (!strcmp(copy,"any")) {
      su.sin.sin_addr.s_addr= INADDR_ANY;
    } else if (!inet_aton(copy,&su.sin.sin_addr)) {
      he= gethostbyname(copy);
      if (!he) { herror(copy); exit(4); }
      if (he->h_addrtype != AF_INET ||
	  he->h_length != sizeof(su.sin.sin_addr) ||
	  !he->h_addr_list[0] ||
	  he->h_addr_list[1]) {
	fprintf(stderr,"hostname lookup `%s' did not yield"
		" exactly one IPv4 address\n",copy);
	exit(4);
      }
      memcpy(&su.sin.sin_addr, he->h_addr_list[0], sizeof(su.sin.sin_addr));
    }

  } else {
    usageerr("unknown bind name");
    exit(12);
  }

  master= socket(su.sa.sa_family,SOCK_STREAM,0);
  if (master<0) { perror("socket"); exit(8); }

  r= bind(master, &su.sa, sulen);
  if (r) { perror("bind"); exit(8); }

  r= listen(master, 5);
  if (r) { perror("listen"); exit(8); }
}

static void opensounddevice(void) {
  int r;
  char cbuf[200];
  
  sdev= open("/dev/dsp", O_WRONLY);
  if (sdev<0) { perror("open sound device"); exit(8); }

  snprintf(cbuf, sizeof(cbuf), "sox -t raw -s -w -r 44100 -c 2"
	   " - </dev/null -t ossdsp - >&%d", sdev);
  r= system(cbuf);  if (r) { fprintf(stderr,"sox gave %d\n",r); exit(5); }
}

void wrbuf_report(const char *m) {
  printf("writing %s\n", m);
}

static void selectcopy(void) {
  int slave= inq.head ? inq.head->fd : -1;
  wrbufcore_prepselect(slave, sdev);
  fdsetset(master,&readfds);
  callselect();
  wrbufcore_afterselect(slave, sdev);
}

static void expireoldconns(void) {
  struct inqnode *searchold, *nextsearchold;
      
  for (searchold= inq.head ? inq.head->next : 0;
       searchold;
       searchold= nextsearchold) {
    nextsearchold= searchold->next;
    if (searchold->accepted < now-maxstartdelay) {
      printf("expired %p\n",searchold);
      LIST_UNLINK(inq,searchold);
      free(searchold);
    }
  }
}

static void acceptnewconns(void) {
  static int bad;
  
  int slave;
  struct inqnode *new;

  if (!FD_ISSET(master,&readfds)) return;

  slave= accept(master,0,0);
  if (slave < 0) {
    if (!(errno == EINTR ||
	  errno == EAGAIN ||
	  errno == EWOULDBLOCK)) {
      perror("accept");
      bad++;
      if (bad > maxbadaccept) {
	fprintf(stderr,"accept failures repeating\n");
	exit(4);
      }
    }
    /* any transient error will just send us round again via select */
    return;
  }

  bad= 0;
  new= xmalloc(sizeof(struct inqnode));
  new->accepted= now;
  new->fd= slave;
  LIST_LINK_TAIL(inq,new);

  printf("accepted %p\n",new);
}

static void switchinput(void) {
  struct inqnode *old;
  if (!seeneof) return;
  old= inq.head;
  assert(old);
  printf("finished %p\n",old);
  close(old->fd);
  LIST_UNLINK(inq,old);
  free(old);
  seeneof= 0;
}  

int main(int argc, const char *const *argv) {
  assert(argv[0]);
  if (!argv[1] || argv[2] || argv[1][0]=='-')
    usageerr("no options allowed, must have one argument (bindname)");

  buffersize= 44100*4* 5/*seconds*/;

  opensounddevice();
  bindmaster(argv[1]);
  nonblock(sdev,1);
  nonblock(master,1);

  startupcore();
  wrbufcore_startup();
  
  printf("started\n");
  for (;;) {
    selectcopy();
    if (time(&now)==(time_t)-1) { perror("time(2)"); exit(4); }
    expireoldconns();
    acceptnewconns();
    switchinput();
  }
}
