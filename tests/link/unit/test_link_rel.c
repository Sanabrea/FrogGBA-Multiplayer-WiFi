// Unit test for link_rel.c (v2: records, adaptive RTO, packing).
// Two endpoints over a datagram channel that models the PSP ad-hoc link:
// limited capacity (datagrams/s, tail drop when the air queue is full),
// base latency, jitter (reordering), random loss and duplicates.
// Verifies exactly-once in-order delivery with intact content, and reports
// latency, retransmissions and datagram rate.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "link_rel.h"

#define DGRAM_PAYLOAD 1024
#define MAXQ 65536
typedef struct { uint16_t ack, len; uint32_t sack; uint8_t d[DGRAM_PAYLOAD]; uint32_t at; } pkt_t;

static pkt_t *wire[2]; static int wn[2];
static rel_chan_t ep[2];
static uint32_t now; static double loss, dup; static int base_lat, jitter;
static uint32_t cap_gap;          // us between datagrams on the air (0 = unlimited)
static int cap_queue;             // datagrams that fit in the air queue
static uint32_t air_free[2];      // when the channel towards [to] frees up
static uint32_t rxi[2], txi[2], dgrams, air_drops; static int fail;
static uint32_t *push_t[2]; static uint32_t *lat; static int nlat;
static uint8_t pay[DGRAM_PAYLOAD];

static double rnd(void){ return rand() / (double)RAND_MAX; }

static void put(int to, const uint8_t *d, uint16_t len) {
  int copies = (rnd() < dup) ? 2 : 1, k;
  uint32_t depart = now;
  dgrams++;
  if (cap_gap) {
    if (air_free[to] < now) air_free[to] = now;
    if ((air_free[to] - now) / cap_gap >= (uint32_t)cap_queue) { air_drops++; return; }
    depart = air_free[to];
    air_free[to] += cap_gap;
  }
  if (rnd() < loss) return;
  for (k = 0; k < copies; k++) {
    if (wn[to] >= MAXQ) { printf("wire overflow\n"); exit(2); }
    pkt_t *p = &wire[to][wn[to]++];
    p->len = len;
    p->ack = ep[1-to].expected_seq; p->sack = rel_sack_mask(&ep[1-to]);
    if (len) memcpy(p->d, d, len);
    p->at = depart + base_lat + (jitter ? rand() % jitter : 0);
  }
  ep[1-to].ack_pending = 0;
}

// Same policy as link.c: up to max datagrams of packed records.
static int send_data(int e, int new_only, int max) {
  int s = 0;
  while (s < max) {
    uint32_t n = rel_collect(&ep[e], now, pay, DGRAM_PAYLOAD, new_only);
    if (!n) break;
    put(1-e, pay, (uint16_t)n); s++;
  }
  return s;
}

static void make(int from, uint32_t idx, uint8_t *b, uint16_t *len) {
  uint32_t i; *len = 8 + (idx * 7 + from * 13) % 120;
  for (i = 0; i < *len; i++) b[i] = (uint8_t)(idx * 31 + i * 17 + from * 101);
  memcpy(b, &idx, 4);
}
static int deliver_to;
static void rxcb(void *c, const uint8_t *d, uint16_t len) {
  uint8_t e[REL_MAX_PAYLOAD]; uint16_t el; uint32_t idx; (void)c;
  memcpy(&idx, d, 4);
  if (idx != rxi[deliver_to]) { if (!fail) printf("ORDER ERROR got %u want %u\n", idx, rxi[deliver_to]); fail = 1; }
  make(1-deliver_to, idx, e, &el);
  if (el != len || memcmp(e, d, len)) { if (!fail) printf("CONTENT ERROR %u\n", idx); fail = 1; }
  lat[nlat++] = now - push_t[1-deliver_to][idx];
  rxi[deliver_to]++;
}
static int cmpu(const void *a, const void *b){ uint32_t x=*(const uint32_t*)a, y=*(const uint32_t*)b; return x<y?-1:x>y; }

// period_ms: one burst of `per` messages every period_ms per side.
// immediate: send each message right away (RFU style) instead of per frame.
static int run(const char *name, double l, double d, int bl, int jit, uint32_t rate,
               uint32_t msgs, int period_ms, int per, int immediate) {
  int e, i, j; uint32_t t = 0, full_events = 0;
  loss=l; dup=d; base_lat=bl; jitter=jit; now=0; nlat=0; fail=0; dgrams=0; air_drops=0;
  cap_gap = rate ? 1000000 / rate : 0; cap_queue = 24; air_free[0] = air_free[1] = 0;
  memset(wn,0,sizeof wn); memset(rxi,0,sizeof rxi); memset(txi,0,sizeof txi);
  rel_init(&ep[0]); rel_init(&ep[1]); srand(1234);
  while ((rxi[0] < msgs || rxi[1] < msgs) && t < 4000000) {
    now = t * 1000;
    for (e = 0; e < 2; e++) {
      if ((t % period_ms) == 0) {
        int k; for (k = 0; k < per && txi[e] < msgs; k++) {
          uint8_t b[REL_MAX_PAYLOAD]; uint16_t len; make(e, txi[e], b, &len);
          if (rel_push(&ep[e], b, len) == 0) {
            push_t[e][txi[e]] = now; txi[e]++;
            if (immediate) send_data(e, 1, 1);
          } else { full_events++; break; }
        }
      }
      if ((t % 16) == 0) {          // once per frame, like the PSP code
        if (!send_data(e, 0, 2) && ep[e].ack_pending) put(1-e, NULL, 0);
      }
      for (i = 0, j = 0; i < wn[e]; i++) {
        pkt_t *p = &wire[e][i];
        if (p->at > now) { wire[e][j++] = *p; continue; }
        rel_on_ack(&ep[e], p->ack, p->sack, now);
        if (p->len) {
          deliver_to = e;
          if (rel_on_records(&ep[e], p->d, p->len, rxcb, NULL) < 0) { printf("MALFORMED\n"); fail = 1; }
        }
      }
      wn[e] = j;
    }
    t++;
  }
  qsort(lat, nlat, 4, cmpu);
  int ok = !fail && rxi[0] == msgs && rxi[1] == msgs;
  double secs = t / 1000.0;
  printf("%-40s %s p50=%6.1fms p99=%7.1fms max=%7.1fms retx=%5u full=%4u dgram/s/side=%4.0f airdrop=%u rto=%ums\n",
         name, ok ? "PASS" : "FAIL",
         lat[nlat/2]/1000.0, lat[nlat*99/100]/1000.0, lat[nlat-1]/1000.0,
         ep[0].stat_retx + ep[1].stat_retx, full_events, dgrams / secs / 2, air_drops,
         ep[0].rto / 1000);
  return ok;
}

int main(void) {
  int ok = 1;
  wire[0] = malloc(sizeof(pkt_t)*MAXQ); wire[1] = malloc(sizeof(pkt_t)*MAXQ);
  push_t[0] = malloc(4*200000); push_t[1] = malloc(4*200000); lat = malloc(4*400000);
  printf("-- Cable-like load (2 msg/frame), batched per frame --\n");
  ok &= run("clean LAN 3ms",                       0.00, 0.00,  3000,     0,   0, 20000, 16, 2, 0);
  ok &= run("PSP-like: 25ms, jit 20ms, 200 dg/s",  0.00, 0.00, 25000, 20000, 200, 20000, 16, 2, 0);
  ok &= run("PSP-like + 5% loss",                  0.05, 0.00, 25000, 20000, 200, 20000, 16, 2, 0);
  ok &= run("PSP-like + 15% loss + dup",           0.15, 0.05, 25000, 30000, 200, 20000, 16, 2, 0);
  ok &= run("slow: 40ms, jit 40ms, 120 dg/s, 5%",  0.05, 0.00, 40000, 40000, 120, 20000, 16, 2, 0);
  ok &= run("30% loss, 25ms, 200 dg/s",            0.30, 0.00, 25000, 20000, 200, 10000, 16, 2, 0);
  printf("-- Pokemon-like burst: 9 msgs per frame --\n");
  ok &= run("9/frame PSP-like 5% loss",            0.05, 0.00, 25000, 20000, 200, 40000, 16, 9, 0);
  printf("-- RFU-like: immediate send --\n");
  ok &= run("immediate 1/4ms PSP-like 5%",         0.05, 0.00, 25000, 20000, 200, 20000,  4, 1, 1);
  printf("-- Stress --\n");
  ok &= run("1 msg/ms, 5% loss, reorder 30ms",     0.05, 0.05,  2000, 30000,   0, 30000,  1, 1, 0);
  ok &= run("wraparound 70000 msgs, 3% loss",      0.03, 0.01,  3000,  3000,   0, 70000,  4, 1, 0);
  printf(ok ? "ALL PASS\n" : "SOME FAILED\n");
  return ok ? 0 : 1;
}
