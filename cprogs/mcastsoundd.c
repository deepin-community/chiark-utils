/*
 * http://www.ibiblio.org/pub/Linux/docs/HOWTO/Multicast-HOWTO
 */

#include <stdio.h>
#include <errno.h>
#include <string.h>
#include <assert.h>
#include <unistd.h>
#include <signal.h>

#include <endian.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include "myopt.h"
#include "common.h"

typedef unsigned char Byte;

static int ov_mode= 'r';
static const char *ov_requ=  "127.0.0.1";
static const char *ov_mcast= "239.193.27.221";

static int ov_port_requ= 4101;
static int ov_port_ctrl= 4101;
static int ov_port_data= -1;

static const struct cmdinfo cmdinfos[]= {
  { "server",     0, &ov_mode,0,0, 's' },
  { "player",     0, &ov_mode,0,0, 'p' },
  { "request",    0, &ov_mode,0,0, 'r' },
  { "mcast-addr", 1, 0,&ov_mcast },
  { "requ-addr",  1, 0,&ov_requ },
  { "requ-port",  1, &ov_port_requ },
  { "ctrl-port",  1, &ov_port_ctrl },
  { "data-port",  1, &ov_port_data },
  0
};


static int mcast_fd, requ_fd;
static struct sockaddr_in requ_sa, ctrl_sa, data_sa;

static void sysfail(const char *m) { perror(m); exit(16); }

static Byte packet[1024];
static int packet_len;

/*---------- marshalling ----------*/

static uint64_t htonll(uint64_t v) {
#if LITTLE_ENDIAN
  return (v >> 32) | (v << 32);
#endif
#if BIG_ENDIAN
  return v;
#endif
}

#define OP_CTRL_PLAY 1
#define OP_CTRL_STOP 2
#define OP_CTRL_DATA 3

#define MAR_CTRL_PLAY				\
  FI8(operation)				\
  FI8(reserved)					\
  FI8(generation)				\
  FI8(counter)					\
  FI64(totallen)				\
  FI64(startts)					\
  FI32(starttns)				\
  FI32(txrate)					\
  FR(trackfn,char,256)
  
#define MAR_CTRL_STOP				\
  FI8(operation)				\
  FI8(reserved)					\
  FR0

#define MAR_DATA				\
  FI8(operation)				\
  FI8(reserved)					\
  FI8(generation)				\
  FI8(counter)					\
  FI64(offset)					\
  FR(data,Byte,1024)
     
#define FI8(f)      F(f, uint8_t,  v)
#define FI32(f)     F(f, uint32_t, htonl(v))
#define FI64(f)     F(f, uint64_t, htonll(v))

#define MARS					\
  MAR(CTRL_PLAY)				\
  MAR(CTRL_STOP)				\
  MAR(DATA)

#define F(f,t,c) t f;
#define FR(f,t,l) t f[(l)]; int f##_l;
#define FR0 /* */
#define MAR(m) typedef struct Mar_##m { MAR_##m } Mar_##m;
MARS
#undef F
#undef FR
#undef FR0
#undef MAR

#define F(f,t,c) { t v= d->f; *(t*)p= c; p += sizeof(t); };
#define FR(f,t,l) assert(d->f##_l<=l); memcpy(p,d->f,d->f##_l); p+=d->f##_l;
#define FR0 /* */
#define MAR(m)					\
  static void mar_##m(const Mar_##m *d) {	\
    Byte *p= packet;				\
    MAR_##m					\
    packet_len= p - packet;			\
    assert(packet_len < sizeof(packet));	\
  }
MARS
#undef F
#undef FR
#undef FR0
#undef MAR

#define F(f,t,c) {				\
    t v;					\
    if (lr < sizeof(t)) return -1;		\
    v= *(const t*)p;				\
    p += sizeof(t);  lr -= sizeof(t);		\
    d->f= c;					\
  };
#define FR(f,t,l) {				\
    if (lr > l) return -1;			\
    memcpy(d->f, p, lr);			\
    d->f##_l= lr;				\
  };
#define FR0					\
    if (lr) return -1;
#define MAR(m)					\
  static int unmar_##m(Mar_##m *d) {		\
    const Byte *p= packet;			\
    int lr= packet_len;				\
    MAR_##m					\
    return 0;					\
  }
MARS
#undef F
#undef FR
#undef FR0
#undef MAR

/*---------- general stuff ----------*/

static void blocksignals(int how) {
  sigset_t set;
  int r;

  sigemptyset(&set);
  sigaddset(&set,SIGCHLD);
  r= sigprocmask(how,&set,0);
  if (r) sysfail("sigprocmask");
}

static int mksocket(int type, int proto,
		    const struct sockaddr_in *sa, const char *what) {
  int fd, r;

  fd= socket(PF_INET, type, proto);
  if (fd<0) sysfail("socket %s",what);

  r= bind(fd, (struct sockaddr*)&mcast_sa, sizeof(*sa));
  if (r) sysfail("bind %s",what);

  return fd;
}

static void mkmcastrecv(const struct sockaddr_in *sa, const char *what) {
  struct ip_mreq mreq;
  int r;

  mcast_fd= mksocket(SOCK_DGRAM, IPPROTO_UDP, sa, what);

  mreq.imr_multiaddr= sa->sin_addr;
  mreq.imr_interface.s_addr= INADDR_ANY;
  r= setsockopt(mcast_fd, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mreq, sizeof(mreq));
  if (r) sysfail("add mcast membership %s", what);
}

/*---------- player ----------*/

static void recvd_play(void) {
  Mar_CTRL_PLAY pkt;
  int r;

  r= unmar_CTRL_PLAY(&pkt);
  if (r) { fprintf(stderr,"bad PLAY packet\n"); return; }
  
}

static void recvd_stop(void) {
  Mar_CTRL_STOP pkt;
  int r;

  r= unmar_CTRL_STOP(&pkt);
  if (r) { fprintf(stderr,"bad STOP packet\n"); return; }
}
  
static void player(void) {
  struct sockaddr_in peer_sa, old_peer_sa;
  socklen_t peer_salen;
  int r;

  mkmcastrecv(&ctrl_sa, "ctrl");
  
  memset(&old_peer_sa, 0, sizeof(old_peer_sa));

  for (;;) {
    peer_salen= sizeof(peer_sa);
    memset(&peer_sa, 0, sizeof(peer_sa));
    
    blocksignals(SIG_UNBLOCK);
    packet_len= recvfrom(mcast_fd, packet, sizeof(packet),
			 MSG_TRUNC, (struct sockaddr*)&peer_sa, &peer_salen);
    blocksignals(SIG_BLOCK);

    if (packet_len<0) {
      if (errno==EINTR) continue;
      perror("mcast_fd recvfrom");
      continue;
    }
    if (peer_salen != sizeof(peer_sa)) {
      fprintf(stderr,"mcast_fd recvfrom salen %ld not %ld\n",
	      (unsigned long)peer_salen, (unsigned long)sizeof(peer_sa));
      continue;
    }
    if (packet_len > sizeof(packet)) {
      fprintf(stderr,"mcast_fd recvfrom packet len %ld longer than max %ld\n",
	      (unsigned long)packet_len, (unsigned long)sizeof(packet));
      continue;
    }
    if (memcmp(&old_peer_sa, &peer_sa, sizeof(old_peer_sa))) {
      char *p= inet_ntoa(peer_sa.sin_addr);
      fprintf(stderr,"receiving from %s:%d\n",p,ntohs(peer_sa.sin_port));
      memcpy(&old_peer_sa, &peer_sa, sizeof(old_peer_sa));
    }
    if (packet_len==0) {
      fprintf(stderr,"empty packet!\n");
      continue;
    }
    switch (packet[0]) {
    case OP_CTRL_PLAY:
      recvd_play();
      break;
    case OP_CTRL_STOP:
      recvd_stop();
      break;
    default:
      fprintf(stderr,"unknown opcode %d\n",packet[0]);
    }
  }
}

/*---------- server ----------*/

void server(void) {
  requ_fd= mksocket(SOCK_STREAM, IPPROTO_TCP, &requ_sa, "requ");
  

/*---------- main ----------*/

static void argaddr(struct sin_addr *sa, const char *addr_name, int port) {
  memset(sa,0,sizeof(*sa));
  sa->sin_family= AF_INET;
  
  r= inet_aton(ov_mcast, &mcast_sa.sin_addr);
  if (!r) badusage("invalid addr `%s'", addr_name);

  if (port<0 || port>65536) badusage("invalid port %d",port);

  sa->sin_port= htons(port);
}

int main(int argc, const char **argv) {
  int r;

  if (ov_port_data < 0) ov_port_data= ov_port_ctrl+1;
  myopt(&argv, cmdinfos);

  argaddr(&requ_sa, ov_requ,  ov_requ_port);
  argaddr(&ctrl_sa, ov_mcast, ov_ctrl_port);
  argaddr(&data_sa, ov_data,  ov_data_port);

  if (argv[1] && ov_mode != 'p')
    badusage("mode takes no non-option arguments");

  switch (ov_mode) {
  case 'p':
    player();
    break;
  case 's':
    server();
    break;
  case 'r':
    if (!argv[1] || argv[2])
      badusage("play-requester takes one non-option argument");
    request(argv[1]);
    break;
  default:
    abort();
  }

  nonblock(0);
  mar_CTRL_PLAY(0);
  mar_CTRL_STOP(0);
  mar_DATA(0);
  unmar_DATA(0);
  return 0;
}
