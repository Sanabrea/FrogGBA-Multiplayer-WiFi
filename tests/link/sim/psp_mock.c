/* Test-only PSP API mock: ad-hoc PDP sockets emulated over UDP on 127.0.0.1,
 * with configurable loss / latency / jitter (applied on the sender side).
 *
 * Env:
 *   LINK_ROLE      host | guest       (decides our fake MAC address)
 *   LINK_PORTBASE  base UDP port       (default 47000)
 *   LINK_LOSS      0.0 .. 1.0          packet loss probability
 *   LINK_DELAY_US  one-way delay       (default 3000)
 *   LINK_JITTER_US random extra delay  (default 0)
 *   LINK_RATE      datagrams/s the "air" carries per direction (0 = unlimited);
 *                  beyond a 24 datagram queue, datagrams are dropped
 */
#define _GNU_SOURCE
#include "common.h"
#include "pspnet.h"
#include "pspnet_adhoc.h"
#include "pspnet_adhocctl.h"
#include "psputility.h"
#include "pspwlan.h"
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>
#include <sys/stat.h>
#include "psprtc.h"

static int my_id = 1;
static int port_base = 47000;
static double loss = 0.0;
static u32 delay_us = 3000, jitter_us = 0;
static u32 rate_gap = 0, air_free = 0, air_drops = 0;
static u32 stall_us = 0, stall_every_us = 0;   // periodic delivery stalls (bursts)
static int sock = -1;
static int adhocctl_state_v = 0;
unsigned long mock_sent = 0, mock_dropped = 0, mock_recv = 0;

typedef struct { u8 data[2048]; int len; int port; u32 at; } qpkt;
#define QMAX 8192
static qpkt *queue; static int qn;

static u32 mono_us(void) {
  struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
  return (u32)((u64)ts.tv_sec * 1000000ull + ts.tv_nsec / 1000);
}

static void mock_init_env(void) {
  static int done = 0;
  const char *s;
  if (done) return;
  done = 1;
  s = getenv("LINK_ROLE");      if (s && !strcmp(s, "guest")) my_id = 2;
  s = getenv("LINK_PORTBASE");  if (s) port_base = atoi(s);
  s = getenv("LINK_LOSS");      if (s) loss = atof(s);
  s = getenv("LINK_DELAY_US");  if (s) delay_us = atoi(s);
  s = getenv("LINK_JITTER_US"); if (s) jitter_us = atoi(s);
  s = getenv("LINK_STALL_MS");  if (s) stall_us = atoi(s) * 1000;
  s = getenv("LINK_STALL_EVERY_MS"); if (s) stall_every_us = atoi(s) * 1000;
  s = getenv("LINK_RATE");      if (s && atoi(s) > 0) rate_gap = 1000000 / atoi(s);
  srand(1000 + my_id * 77 + (unsigned)getpid());
  queue = calloc(QMAX, sizeof(qpkt));
}

static void flush_queue(void) {
  u32 now = mono_us(); int i, j = 0;
  for (i = 0; i < qn; i++) {
    if ((s32)(now - queue[i].at) >= 0) {
      struct sockaddr_in a; memset(&a, 0, sizeof a);
      a.sin_family = AF_INET; a.sin_port = htons(queue[i].port); a.sin_addr.s_addr = htonl(0x7F000001);
      sendto(sock, queue[i].data, queue[i].len, 0, (struct sockaddr *)&a, sizeof a);
    } else queue[j++] = queue[i];
  }
  qn = j;
}

u32 sceKernelGetSystemTimeLow(void) { return mono_us(); }
int sceKernelDelayThread(u32 us) { usleep(us); return 0; }

int sceUtilityLoadNetModule(int m) { (void)m; mock_init_env(); return 0; }
int sceUtilityUnloadNetModule(int m) { (void)m; return 0; }
int sceNetInit(int a, int b, int c, int d, int e) { (void)a;(void)b;(void)c;(void)d;(void)e; return 0; }
int sceNetTerm(void) { return 0; }
int sceNetAdhocInit(void) { return 0; }
int sceNetAdhocTerm(void) { return 0; }
int sceNetAdhocctlInit(int s, int p, struct productStruct *pr) { (void)s;(void)p;(void)pr; return 0; }
int sceNetAdhocctlTerm(void) { return 0; }
int sceNetAdhocctlConnect(const char *name) { (void)name; adhocctl_state_v = 1; return 0; }
int sceNetAdhocctlDisconnect(void) { adhocctl_state_v = 0; return 0; }
int sceNetAdhocctlGetState(int *ev) { *ev = adhocctl_state_v; return 0; }
int sceWlanGetSwitchState(void) { return 1; }

int sceNetGetLocalEtherAddr(unsigned char *mac) {
  mock_init_env();
  memset(mac, 0, 6); mac[0] = 0x02; mac[5] = (u8)my_id; return 0;
}

int sceNetAdhocctlGetPeerList(int *length, void *buf) {
  SceNetAdhocctlPeerInfo *p = buf;
  if (*length < (int)sizeof(*p)) { *length = 0; return 0; }
  memset(p, 0, sizeof(*p));
  p->mac[0] = 0x02; p->mac[5] = (u8)(3 - my_id);
  *length = sizeof(*p);
  return 0;
}

int sceNetAdhocPdpCreate(unsigned char *mac, unsigned short port, unsigned int bufsize, int unk1) {
  struct sockaddr_in a; (void)port; (void)bufsize; (void)unk1;
  mock_init_env();
  sock = socket(AF_INET, SOCK_DGRAM, 0);
  memset(&a, 0, sizeof a);
  a.sin_family = AF_INET; a.sin_port = htons(port_base + mac[5]); a.sin_addr.s_addr = htonl(0x7F000001);
  if (bind(sock, (struct sockaddr *)&a, sizeof a) < 0) { perror("bind"); return -1; }
  fcntl(sock, F_SETFL, O_NONBLOCK);
  return 1;
}

int sceNetAdhocPdpDelete(int id, int unk1) { (void)id; (void)unk1; if (sock >= 0) close(sock); sock = -1; return 0; }

int sceNetAdhocPdpSend(int id, unsigned char *dest, unsigned short port, void *data, unsigned int len,
                       unsigned int timeout, int nonblock) {
  int dest_id; (void)id; (void)port; (void)timeout; (void)nonblock;
  if (sock < 0) return -1;
  dest_id = (dest[0] == 0xFF) ? (3 - my_id) : dest[5];
  if (dest_id == my_id) return 0;
  mock_sent++;
  u32 depart = mono_us();
  if (rate_gap) {
    u32 now = depart;
    if ((int32_t)(air_free - now) < 0) air_free = now;
    if ((air_free - now) / rate_gap >= 24) { air_drops++; mock_dropped++; flush_queue(); return len; }
    depart = air_free; air_free += rate_gap;
  }
  if ((rand() / (double)RAND_MAX) < loss) { mock_dropped++; flush_queue(); return len; }
  if (qn < QMAX && len <= sizeof(queue[0].data)) {
    qpkt *q = &queue[qn++];
    memcpy(q->data, data, len); q->len = len; q->port = port_base + dest_id;
    q->at = depart + delay_us + (jitter_us ? (u32)(rand() % jitter_us) : 0);
    if (stall_us && stall_every_us) {
      u32 ph = q->at % stall_every_us;     // inside a stall window: held until it ends
      if (ph < stall_us) q->at += stall_us - ph;
    }
  } else fprintf(stderr, "MOCK: send queue overflow / datagram too big (%u)\n", len);
  flush_queue();
  return len;
}

int sceNetAdhocPdpRecv(int id, unsigned char *src, unsigned short *port, void *data, void *dataLength,
                       unsigned int timeout, int nonblock) {
  struct sockaddr_in a; socklen_t al = sizeof a; int *len = dataLength; ssize_t r;
  (void)id; (void)timeout; (void)nonblock;
  if (sock < 0) return -1;
  flush_queue();
  r = recvfrom(sock, data, *len, MSG_DONTWAIT, (struct sockaddr *)&a, &al);
  if (r < 0) return (int)0x80410709;   // WOULD_BLOCK
  memset(src, 0, 6); src[0] = 0x02; src[5] = (u8)(ntohs(a.sin_port) - port_base);
  *port = 0x4C4B; *len = (int)r; mock_recv++;
  return 0;
}

// GUI stubs
GUI_ACTION_TYPE get_gui_input(void) { return CURSOR_SELECT; }   // dismisses message boxes, never cancels
void clear_screen(u32 c) { (void)c; }
void print_string(const char *s, s16 x, u16 y, u16 fg, s16 bg) { (void)s;(void)x;(void)y;(void)fg;(void)bg; }
void flip_screen(u32 vsync) { (void)vsync; flush_queue(); usleep(16667); }

// Called by the sim every frame to push delayed packets out.
void mock_pump(void) { if (sock >= 0) flush_queue(); }

// File IO / RTC used by link_log.c
u32 reg[64];
char main_path[MAX_PATH] = ".";
SceUID sceIoOpen(const char *file, int flags, int mode) {
  int f = O_WRONLY | ((flags & PSP_O_CREAT) ? O_CREAT : 0) | ((flags & PSP_O_APPEND) ? O_APPEND : 0) | ((flags & PSP_O_TRUNC) ? O_TRUNC : 0);
  return open(file, f, mode);
}
int sceIoWrite(SceUID fd, const void *data, unsigned size) { return (int)write(fd, data, size); }
int sceIoClose(SceUID fd) { return close(fd); }
int sceIoGetstat(const char *file, SceIoStat *st) { struct stat s; if (stat(file, &s) < 0) return -1; st->st_size = s.st_size; return 0; }
int sceRtcGetCurrentClockLocalTime(ScePspDateTime *t) { time_t now = time(NULL); struct tm *tm = localtime(&now);
  t->year = tm->tm_year + 1900; t->month = tm->tm_mon + 1; t->day = tm->tm_mday; t->hour = tm->tm_hour; t->minute = tm->tm_min; t->second = tm->tm_sec; return 0; }
