/*
 * smtpallow.c - ld_preload for hacking with connect() !
 *
 * Copyright (C) 1994,1995 Ian Jackson <ian@davenant.greenend.org.uk>
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

#include <syscall.h>
#include <sys/socketcall.h>
#include <netinet/in.h>
#include <string.h>

_syscall2(long,socketcall,int,call,unsigned long *,args);

int real_connect(int sockfd, const struct sockaddr *saddr, int addrlen)
{
	unsigned long args[3];

	args[0] = sockfd;
	args[1] = (unsigned long)saddr;
	args[2] = addrlen;
	return socketcall(SYS_CONNECT, args);
}

int connect(int fd, const struct sockaddr *them_any, int addrlen) {
  struct sockaddr_in *them= (struct sockaddr_in*)them_any;
  int r,l,i;
  struct sockaddr_in us;
  
  if (addrlen == sizeof(us) &&
      them->sin_family == AF_INET &&
      them->sin_port == htons(25)) {
    memset(&us,0,sizeof(us));
    us.sin_port= 0;
    us.sin_family= AF_INET;
    us.sin_addr.s_addr= INADDR_ANY;
    r= getsockname(fd,(struct sockaddr*)&us,&l);
    if (r<0 && errno != EINVAL) return r;
    if (!ntohs(us.sin_port)) {
      for (i=1023; i>0; i--) {
        us.sin_port= htons(i);
        if (!bind(fd,(struct sockaddr*)&us,sizeof(us))) break;
        if (errno != EADDRINUSE) return -1;
      }
      if (!i) return -1;
    } else if (r<0) return r;
  }
  return real_connect(fd,them_any,addrlen);
}
