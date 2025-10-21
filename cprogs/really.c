/*
 * really.c - program for gaining privilege
 *
 * Copyright (C) 1992-3 Ian Jackson <ian@davenant.greenend.org.uk>
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

#include <stdio.h>
#include <stdlib.h>
#include <limits.h>
#include <unistd.h>
#include <pwd.h>
#include <grp.h>
#include <sys/types.h>
#include <errno.h>

#include "myopt.h"

void usagemessage(void) {
  if (fputs("usage: really [<really-option> ...] [--]"
            " [<command> [<argument/option> ...]]\n"
            "really-options specifying the user:\n"
            " if no options given, set the uid to 0;\n"
            " -u|--user <username>     also sets their default group list\n"
            " -i|--useronly <username> } set the uid\n"
            " -I|--uidonly <uid>       }  but inherits the group list\n"
            "really-options specifying the group:\n"
            " -z|--groupsclear         only groups specified are to be used\n"
            " -g|--group <groupname>   } add this to\n"
            " -G|--gid <gid>           }  the group list\n"
            "other really-options:\n"
	    " -h|--help                display this message\n"
	    " -R|--chroot <dir>        chroot (but *not* chdir - danger!)\n",
            stderr) == EOF) { perror("write usage"); exit(-1); }
}

static const char *opt_user, *opt_useronly, *opt_chroot;
static int opt_groupsclear= 0, opt_ngids= 0, opt_uidonly= -1;
static int opt_gids[512];

static void af_uidonly(const struct cmdinfo *cip, const char *value) {
  unsigned long ul;
  char *ep;

  ul= strtoul(value,&ep,10);
  if (*ep) { fprintf(stderr,"bad uid `%s'\n",value); exit(-1); }
  opt_uidonly= ul;
}

static void af_group(const struct cmdinfo *cip, const char *value) {
  struct group *gr;

  if (opt_ngids >= sizeof(opt_gids)/sizeof(opt_gids[0]))
    badusage("too many groups specified");
  gr= getgrnam(value);
  if (!gr) { fprintf(stderr,"unknown group `%s'\n",value); exit(-1); }
  opt_gids[opt_ngids++]= gr->gr_gid;
}

static void af_gid(const struct cmdinfo *cip, const char *value) {
  char *ep;
  unsigned long ul;

  if (opt_ngids >= sizeof(opt_gids)/sizeof(opt_gids[0]))
    badusage("too many gids specified");
  ul= strtoul(value,&ep,0);
  if ((*ep) || (uid_t)ul != ul || ul>INT_MAX) badusage("bad gid `%s'",value);
  opt_gids[opt_ngids++]= ul;
}

static void af_help(const struct cmdinfo *cip, const char *value) {
  usagemessage(); exit(0);
}

static const struct cmdinfo cmdinfos[]= {
  { "user",         'u',  1,  0, &opt_user,          0,           },
  { "useronly",     'i',  1,  0, &opt_useronly,      0            },
  { "uidonly",      'I',  1,  0, 0,                  af_uidonly   },
  { "groupsclear",  'z',  0,  &opt_groupsclear, 0,   0,        1  },
  { "group",        'g',  1,  0, 0,                  af_group     },
  { "gid",          'G',  1,  0, 0,                  af_gid       },
  { "chroot",       'R',  1,  0, &opt_chroot,        0            },
  { "help",         'h',  0,  0, 0,                  af_help      },
  {  0,              0                                            }
};

#ifdef REALLY_CHECK_FILE
static int checkroot(void) {
  int r;
  r= access(REALLY_CHECK_FILE,   W_OK);
  if (!r) return 0;
#ifdef REALLY_CHECK_FILE_2
  r= access(REALLY_CHECK_FILE_2, W_OK);
  if (!r) return 0;
  /* If all fails we return the errno from file _2 */
#endif /*REALLY_CHECK_FILE_2*/
  return -1;
}
#endif
#ifdef REALLY_CHECK_GID
static int checkroot(void) {
  gid_t groups[512];
  int r, i;

  r= getgid(); if (r==REALLY_CHECK_GID) return 0;
  if (r<0) { perror("getgid check"); exit(-1); }
  r= getgroups(sizeof(groups)/sizeof(groups[0]),groups);
  if (r<0) { perror("getgroups check"); exit(-1); }
  for (i=0; i<r; i++)
    if (groups[i] == REALLY_CHECK_GID) return 0;
  return -1;
}
#endif
#ifdef REALLY_CHECK_NONE
static int checkroot(void) {
  return 0;
}
#endif

int main(int argc, const char *const *argv) {
  struct passwd *pw= 0;
  gid_t groups[512];
  int i, j, ngroups, ngroups_in, maingid, orgmaingid, mainuid, orgmainuid, r;
  const char *cp;
  
  orgmainuid= getuid();
  if (orgmainuid && checkroot()) { perror("sorry"); exit(-1); }
  myopt(&argv,cmdinfos);

  if (opt_groupsclear && !opt_ngids)
    badusage("-z|--groupsclear must be accompanied by some groups");
  if (opt_user && (opt_useronly || opt_uidonly!=-1))
    badusage("-u|--user may not be used with -i|--useronly or -I|--uidonly");
  if (opt_user && opt_groupsclear)
    badusage("-u|--user may not be used with -z|--groupsclear");
  if (opt_uidonly != -1 && (uid_t)opt_uidonly != opt_uidonly)
    badusage("-I|--uidonly value %d is out of range for a uid",opt_uidonly);

  if (!opt_user && !opt_useronly && opt_uidonly==-1 && !opt_ngids) {
    opt_uidonly= 0;
  }
  if (opt_user || opt_useronly) {
    cp= opt_user ? opt_user : opt_useronly;
    pw= getpwnam(cp);
    if (!pw) { fprintf(stderr,"unknown user `%s'\n",cp); exit(-1); }
    opt_uidonly= pw->pw_uid;
  }
  if (opt_chroot) {
    if (chroot(opt_chroot)) { perror("chroot failed"); exit(-1); }
  }
  orgmaingid= getgid();
  if (orgmaingid<0) { perror("getgid failed"); exit(-1); }
  if (opt_user) {
    r= initgroups(opt_user,pw->pw_gid);
    if (r) { perror("initgroups failed"); exit(-1); }
    maingid= pw->pw_gid;
  } else {
    maingid= -1;
  }
  if (opt_groupsclear) {
    ngroups= 0;
    if (opt_ngids > sizeof(groups)/sizeof(groups[0])) {
      fputs("too many groups to set\n",stderr);
      exit(-1);
    }
  } else {
    ngroups= getgroups(0,0);
    if (ngroups<0) { perror("getgroups(0,0) failed"); exit(-1); }
    if (ngroups+opt_ngids > sizeof(groups)/sizeof(groups[0])) {
      fputs("too many groups already set for total to fit\n",stderr);
      exit(-1);
    }
    ngroups= getgroups(ngroups,groups);
    if (ngroups<0) { perror("getgroups failed"); exit(-1); }
  }
  if (opt_ngids) {
    maingid= opt_gids[0];
  }
  if (opt_ngids || opt_groupsclear) {
    ngroups_in= ngroups; ngroups= 0;
    for (i=0; i<ngroups_in; i++) {
      for (j=0; j<ngroups && groups[j] != groups[i]; j++);
      if (j<ngroups) continue;
      groups[ngroups++]= groups[i];
    }
    for (i=0; i<opt_ngids; i++) {
      for (j=0; j<ngroups && groups[j] != opt_gids[i]; j++);
      if (j<ngroups) continue;
      groups[ngroups++]= opt_gids[i];
    }
    r= setgroups(ngroups,groups);
    if (r) { perror("setgroups failed"); exit(-1); }
  }
  if (maingid != -1) {
    r= setgid(maingid); if (r) { perror("setgid failed"); exit(-1); }
    r= setgid(maingid); if (r) { perror("2nd setgid failed"); exit(-1); }
  }
  if (opt_uidonly != -1) {
    mainuid= opt_uidonly;
  } else {
    mainuid= orgmainuid;
  }
  r= setuid(mainuid); if (r) { perror("setuid failed"); exit(-1); }
  r= setuid(mainuid); if (r) { perror("2nd setuid failed"); exit(-1); }
  if (mainuid != 0) {
    r= seteuid(0); if (r>=0) { fputs("could seteuid 0",stderr); exit(-1); }
    if (errno != EPERM) {
      perror("unexpected failure mode for seteuid 0"); exit(-1);
    }
  }
  r= getuid(); if (r<0) { perror("getuid failed"); exit(-1); }
  if (r != mainuid) { fputs("getuid mismatch",stderr); exit(-1); }
  r= geteuid(); if (r<0) { perror("geteuid failed"); exit(-1); }
  if (r != mainuid) { fputs("geteuid mismatch",stderr); exit(-1); }
  if (maingid != -1) {
    for (i=0; i<ngroups && maingid != groups[i]; i++);
    if (i>=ngroups && maingid != orgmaingid) {
      r= setgid(orgmaingid);
      if (r>=0) { fputs("could setgid back",stderr); exit(-1); }
      if (errno != EPERM) {
        perror("unexpected failure mode for setgid back"); exit(-1);
      }
    }
    r= getgid(); if (r<0) { perror("getgid failed"); exit(-1); }
    if (r != maingid) { fputs("getgid mismatch",stderr); exit(-1); }
    r= getegid(); if (r<0) { perror("getegid failed"); exit(-1); }
    if (r != maingid) { fputs("getegid mismatch",stderr); exit(-1); }
  }
  if (!*argv) {
    cp= getenv("SHELL");
    if (!cp) cp= "sh";
    execlp(cp,cp,"-i",(const char*)0);
  } else {
    execvp(argv[0],(char**)argv);
  }
  perror("exec failed");
  exit(-1);
}
