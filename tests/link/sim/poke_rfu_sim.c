/* Pokemon-like Wireless Adapter test. Models what Pokemon FireRed/Emerald
 * (pret link_rfu_2.c + librfu) do in the trade room ("MSC" mode), on top of
 * FrogGBA's real rfu.c / link.c / link_rel.c and the mocked PSP network:
 *
 * Parent (host), once per frame:
 *   SEND_DATAW (or RTX_WAIT if the last frame had no ACK), spin until the
 *   adapter event, RECV_DATA. ACK = rfu_UNI_PARENT_getDRAC_ACK (no params:
 *   all slots; else low byte of the param). No ACK -> the frame is skipped
 *   and data read for that slot would be ignored (we flag it as lost).
 * Child (client), on every adapter event (MscCallback_Child):
 *   RECV_DATA; new data -> push to a 20 slot queue (full = FATAL, like the
 *   game) and answer with SEND_DATAW; nothing -> WAIT. Once per frame the
 *   game pops one entry from that queue.
 * Emulated time runs at real-time speed (frame split in slices), so the
 * network is polled mid-frame like on the PSP.
 *
 * Usage: LINK_ROLE=host|guest SIM_FPS=60 ./poke_rfu_sim <seconds>
 */
#define _GNU_SOURCE
#include "common.h"
#include <time.h>
#include <unistd.h>

u16 io_registers[0x400];
u32 cpu_ticks;
char gamepak_filename[256] = "test.gba";
void mock_pump(void);
extern unsigned long mock_sent, mock_dropped, mock_recv;
bool rfu_update(unsigned cycles);

#define GAME_ID      0x46524F47
#define CYC_FRAME    280896
#define NSLICE       8
#define STEP         1024
#define RECVQ_SLOTS  20

static int rfu_cmd(u8 cmd, const u32 *args, int n, u32 *resp) {
  int i, len; u32 r;
  rfu_transfer(0x99660000 | (n << 8) | cmd);
  for (i = 0; i < n; i++) rfu_transfer(args[i]);
  r = rfu_transfer(0x80000000);
  if ((r >> 16) != 0x9966) return -2;
  if ((r & 0xFF) == 0xEE) { rfu_transfer(0x80000000); return -1; }
  if ((r & 0xFF) != (u32)(0x80 | cmd)) return -3;
  len = (r >> 8) & 0xFF;
  for (i = 0; i < len; i++) { u32 v = rfu_transfer(0x80000000); if (resp) resp[i] = v; }
  return len;
}

static u32 hashw(u32 seq, int from_host) { return (seq * 2654435761u) ^ (from_host ? 0xA5A5A5A5u : 0x5A5A5A5Au); }

static u64 now_ns(void) { struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); return (u64)ts.tv_sec * 1000000000ull + ts.tv_nsec; }
static void sleep_until(u64 t) { struct timespec ts; ts.tv_sec = t / 1000000000ull; ts.tv_nsec = t % 1000000000ull; clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &ts, NULL); }

/* ---- adapter event reception (GBA in slave mode) ---- */
static int waiting;          // In a clock-reversed wait
static u32 ev[8]; static int ev_n, ev_need;

static void start_wait(void) {
  waiting = 1; ev_n = 0; ev_need = 0;
  io_registers[REG_SIOCNT] = 0x4080;   // slave (bit0=0), START, IRQ enable
}

// Returns 1 when a whole event has been received.
static int poll_event(void) {
  if (io_registers[REG_SIOCNT] & 0x80) return 0;   // nothing pushed
  ev[ev_n++] = ((u32)io_registers[REG_SIODATA32_H] << 16) | io_registers[REG_SIODATA32_L];
  if (ev_n == 1) ev_need = 2 + ((ev[0] >> 8) & 0xFF);
  if (ev_n >= ev_need) { waiting = 0; io_registers[REG_SIOCNT] = 0x1; return 1; }
  io_registers[REG_SIOCNT] = 0x4080;   // ready for the next word
  return 0;
}

int main(int argc, char **argv) {
  const char *role = getenv("LINK_ROLE");
  int as_host = role && !strcmp(role, "host");
  int seconds = argc > 1 ? atoi(argv[1]) : 20;
  double fps = getenv("SIM_FPS") ? atof(getenv("SIM_FPS")) : 60.0;
  u64 frame_ns = (u64)(1e9 / fps), t0;
  u32 resp[64], args[32], frame;
  u32 devid = 0, clnum = 0, host_devid = 0;
  int phase = 0, n, i, slice;
  /* parent */
  u32 h_seq = 1, h_exp = 1, p_ok = 0, p_noack = 0, p_nodata = 0, p_timeouts = 0, p_lost = 0, p_bad = 0, p_got = 0;
  int main_failed = 0, p_started_at = -1;
  /* child */
  u32 c_seq = 1, c_exp = 1, c_got = 0, c_bad = 0, c_timeouts = 0, c_popped = 0;
  int recvq = 0, recvq_max = 0, fatal = 0, c_started = 0;
  u32 fatal_frame = 0;

  setvbuf(stdout, NULL, _IOLBF, 0);
  if (getenv("LINK_LOGDIR")) snprintf(main_path, sizeof(main_path), "%s/", getenv("LINK_LOGDIR"));
  option_link_mode = LINK_MODE_RFU;
  link_game_loaded("BPRE", "POKEMON FIRE");
  if (!link_connect(as_host)) { printf("[%s] connect failed\n", role); return 2; }
  printf("[%s] %s (fps %.1f)\n", role, link_status_line1(), fps);

  rfu_transfer(0x0000494E);
  rfu_transfer(0xB0BB8001);
  rfu_cmd(0x10, NULL, 0, NULL);
  rfu_cmd(0x3d, NULL, 0, NULL);
  args[0] = (4 << 8) | 32; rfu_cmd(0x17, args, 1, NULL);   // SYSCFG: 32 frames timeout, 4 rtx
  io_registers[REG_SIOCNT] = 0x1;

  t0 = now_ns();
  for (frame = 0; frame < (u32)(seconds * fps); frame++) {
    /* ---- VBlank: per frame adapter/link work + game main loop ---- */
    link_frame_update();
    mock_pump();
    if (!link_is_active()) { printf("[%s] session closed by peer at frame %u\n", role, frame); break; }

    if (as_host) {
      if (phase == 0) {
        for (i = 0; i < 6; i++) args[i] = GAME_ID + i;
        rfu_cmd(0x16, args, 6, NULL); rfu_cmd(0x19, NULL, 0, NULL); phase = 1;
      } else if (phase == 1) {
        n = rfu_cmd(0x1a, NULL, 0, resp);
        if (n >= 1) { printf("[host] client joined (frame %u)\n", frame); phase = 2; p_started_at = frame + 30; }
      } else if (phase == 2 && (int)frame >= p_started_at && !waiting) {
        /* RfuMain1_Parent */
        if (!main_failed) {
          args[0] = 14; args[1] = h_seq; args[2] = hashw(h_seq, 1); args[3] = 0; args[4] = 0;
          rfu_cmd(0x25, args, 5, NULL); h_seq++;
        } else
          rfu_cmd(0x37, NULL, 0, NULL);
        start_wait();
      }
    } else {
      if (phase == 0) { rfu_cmd(0x1c, NULL, 0, NULL); phase = 1; }
      else if (phase == 1) {
        n = rfu_cmd(0x1d, NULL, 0, resp);
        for (i = 0; i + 7 <= n; i += 7) if (resp[i + 1] == GAME_ID) host_devid = resp[i] & 0xFFFF;
        if (host_devid) { rfu_cmd(0x1e, NULL, 0, NULL); args[0] = host_devid; rfu_cmd(0x1f, args, 1, NULL); phase = 2; }
      } else if (phase == 2) {
        n = rfu_cmd(0x20, NULL, 0, resp);
        if (n == 1 && !(resp[0] & 0xFF000000)) {
          devid = resp[0] & 0xFFFF; clnum = resp[0] >> 16; rfu_cmd(0x21, NULL, 0, resp);
          printf("[guest] connected (frame %u)\n", frame); phase = 3;
        }
      } else if (phase == 3) {
        /* RfuMain1_Child: one queue entry per frame */
        if (recvq > 0) { recvq--; c_popped++; }
        if (!c_started) { rfu_cmd(0x27, NULL, 0, NULL); start_wait(); c_started = 1; }
      }
    }

    /* ---- the frame's emulated time, spread over its real time ---- */
    for (slice = 0; slice < NSLICE; slice++) {
      u32 c;
      for (c = 0; c < CYC_FRAME / NSLICE; c += STEP) {
        cpu_ticks += STEP;
        netpacket_poll_receive();
        rfu_update(STEP);
        if (!waiting || !poll_event()) continue;
        /* a whole adapter event arrived */
        {
          u32 cmd = ev[0] & 0xFF, np = (ev[0] >> 8) & 0xFF;
          if (as_host) {
            int ack = 1;
            if (cmd == 0x28 && np) ack = ev[1] & 1;
            else if (cmd == 0x27) { p_timeouts++; ack = 0; }
            else if (cmd == 0x29) { printf("[host] DISCONNECTED event\n"); phase = 9; }
            n = rfu_cmd(0x26, NULL, 0, resp);   /* RECV_DATA */
            {
              u32 bytes = n >= 1 ? (resp[0] >> 8) & 0x1F : 0;
              if (!ack) {
                main_failed = 1; p_noack++;
                if (bytes) p_lost++;
              } else {
                main_failed = 0; p_ok++;
                if (bytes) {
                  p_got++;
                  if (bytes != 14 || resp[1] != c_exp || resp[2] != hashw(resp[1], 0)) p_bad++;
                  c_exp = resp[1] + 1;
                } else p_nodata++;
              }
            }
          } else {
            if (cmd == 0x28) {
              n = rfu_cmd(0x26, NULL, 0, resp);
              if (n >= 1 && resp[0]) {
                c_got++;
                if (resp[0] != 14 || resp[1] != h_exp || resp[2] != hashw(resp[1], 1)) c_bad++;
                h_exp = resp[1] + 1;
                if (recvq >= RECVQ_SLOTS) { if (!fatal) { fatal = 1; fatal_frame = frame; printf("[guest] FATAL: game receive queue full at frame %u\n", frame); } }
                else if (++recvq > recvq_max) recvq_max = recvq;
                args[0] = 14u << (8 + clnum * 5); args[1] = c_seq; args[2] = hashw(c_seq, 0); args[3] = 0; args[4] = 0;
                rfu_cmd(0x25, args, 5, NULL); c_seq++;
              } else
                rfu_cmd(0x27, NULL, 0, NULL);
              start_wait();
            } else if (cmd == 0x27) { c_timeouts++; rfu_cmd(0x27, NULL, 0, NULL); start_wait(); }
            else if (cmd == 0x29) { printf("[guest] DISCONNECTED event\n"); phase = 9; }
          }
        }
      }
      sleep_until(t0 + (u64)frame * frame_ns + (u64)(slice + 1) * frame_ns / NSLICE);
    }
  }

  {
    double secs = (now_ns() - t0) / 1e9;
    int pass;
    if (as_host) {
      double active = secs - p_started_at / fps;
      pass = p_lost == 0 && p_bad == 0 && p_ok > active * fps * 0.5 && p_got > active * fps * 0.5;
      printf("[host] %s | game frames advanced=%u (%.1f/s) noack(stall)=%u nodata=%u timeouts=%u got=%u bad=%u LOST=%u | dgrams=%lu dropped=%lu | %s\n",
             pass ? "PASS" : "FAIL", p_ok, p_ok / active, p_noack, p_nodata, p_timeouts, p_got, p_bad, p_lost,
             mock_sent, mock_dropped, link_status_line2());
    } else {
      pass = !fatal && c_bad == 0 && c_got > (secs - 2) * fps * 0.5;
      printf("[guest] %s | recv=%u (%.1f/s) bad=%u game queue max=%d%s timeouts=%u popped=%u | dgrams=%lu dropped=%lu | %s\n",
             pass ? "PASS" : "FAIL", c_got, c_got / secs, c_bad, recvq_max, fatal ? " FATAL" : "", c_timeouts, c_popped,
             mock_sent, mock_dropped, link_status_line2());
      (void)fatal_frame; (void)devid;
    }
    link_shutdown();
    return pass ? 0 : 1;
  }
}
