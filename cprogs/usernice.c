/**/

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/time.h>
#include <pwd.h>
#include <sys/resource.h>

int main(int argc,char **argv) {
  long l;
  int mrenice,wrenice,newprio,eflag;
  uid_t ruid;
  char *ep;
  struct passwd *pw;

  mrenice=0;
  if (argc < 3) {
    fputs("usernice: too few args\n"
          " usage: usernice <nicelevel> <command> <arguments>\n"
          "        usernice <nicelevel>p <pid> <pid> ...\n"
          "        usernice <nicelevel>u <username|uid> ...\n",
          stderr);
    exit(-1);
  }
  l= strtol(*++argv,&ep,10);
  if (*ep == 'p' || *ep == 'u') { mrenice= *ep++; }
  if (*ep) { fputs("usernice: priority not numeric or bad flags\n",stderr); exit(-1); }
  if (l<-20 || l>20)
    { fputs("usernice: priority must be -20 .. 20\n",stderr); exit(-1); }
  newprio= l;
  if (mrenice) {
    eflag=0;
    while (*++argv) {
      if (mrenice == 'p') {
        wrenice= PRIO_PROCESS;
        l= strtoul(*argv,&ep,10);
        if (*ep) {
          fprintf(stderr,"usernice: pid `%s' not numeric\n",*argv); eflag=2;
          continue;
        }
      } else {
        wrenice= PRIO_USER;
        l= strtoul(*argv,&ep,10);
        if (*ep) {
          pw= getpwnam(*argv);
          if (!pw) {
            fprintf(stderr,"usernice: unknown user `%s'\n",*argv); eflag=2;
            continue;
          }
          l= pw->pw_uid;
        }
      }
      if (setpriority(wrenice,l,newprio)) {
        perror(*argv); if (!eflag) eflag=1;
      }
    }
    exit(eflag);
  } else {
    if (setpriority(PRIO_PROCESS,0,newprio))
      { perror("usernice: setpriority"); exit(-1); }
    ruid= getuid(); if (ruid == (uid_t)-1) { perror("usernice: getuid"); exit(-1); }
    if (setreuid(ruid,ruid)) { perror("usernice: setreuid"); exit(-1); }
    execvp(argv[1],argv+1); perror("usernice: exec"); exit(-1);
  }
}
