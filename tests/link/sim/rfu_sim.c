/* Integration test for the Wireless Adapter (RFU) mode over the link
 * transport. Each process drives FrogGBA's real rfu.c through its SPI
 * command interface (like a game would), on top of link.c/link_rel.c with
 * the mocked PSP ad-hoc network:
 *
 *   PSP host  -> game hosts:  BCST_DATA + HOST_START, polls HOST_ACCEPT
 *   PSP guest -> game client: BCRD_START/FETCH, CONNECT, ISCONNECTED
 *   then both exchange one numbered packet per frame (SEND_DATA/RECV_DATA)
 *
 * Usage: LINK_ROLE=host|guest LINK_MODE=1 ./rfu_sim <seconds>
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

#define GAME_ID 0x46524F47   // broadcast payload marker

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

static u32 payload_word(u32 seq, int i, int from_host) { return (seq * 2654435761u) ^ (i * 0x01010101u) ^ (from_host ? 0xA5A5A5A5u : 0x5A5A5A5Au); }

static void sleep_until(u64 target_ns) {
  struct timespec ts; ts.tv_sec = target_ns / 1000000000ull; ts.tv_nsec = target_ns % 1000000000ull;
  clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &ts, NULL);
}

int main(int argc, char **argv) {
  const char *role = getenv("LINK_ROLE");
  int as_host = role && !strcmp(role, "host");
  int seconds = argc > 1 ? atoi(argv[1]) : 15;
  u32 resp[256], args[32];
  u32 frame, connected_at = 0, tx_seq = 1, rx_last = 0, rx_ok = 0, rx_gap = 0, rx_bad = 0, devid = 0, clnum = 0, host_devid = 0;
  int phase = 0, n, i;
  struct timespec ts; u64 t0;

  setvbuf(stdout, NULL, _IOLBF, 0);
  if (getenv("LINK_LOGDIR")) snprintf(main_path, sizeof(main_path), "%s/", getenv("LINK_LOGDIR"));  /* PSP style: trailing slash */
  option_link_mode = LINK_MODE_RFU;
  link_game_loaded("BPRE", "POKEMON FIRE");
  if (!link_connect(as_host)) { printf("[%s] connect failed\n", role); return 2; }
  printf("[%s] %s\n", role, link_status_line1());

  // SPI handshake with the adapter ("NI" ... "Nintendo")
  rfu_transfer(0x0000494E);
  rfu_transfer(0xB0BB8001);
  rfu_cmd(0x10, NULL, 0, NULL);                       // INIT1
  rfu_cmd(0x3d, NULL, 0, NULL);                       // INIT2
  args[0] = 0x0420 | 32; rfu_cmd(0x17, args, 1, NULL); // SYSCFG: timeout / retries

  clock_gettime(CLOCK_MONOTONIC, &ts);
  t0 = (u64)ts.tv_sec * 1000000000ull + ts.tv_nsec;

  for (frame = 0; frame < (u32)seconds * 60; frame++) {
    if (as_host) {
      if (phase == 0) {
        for (i = 0; i < 6; i++) args[i] = GAME_ID + i;
        rfu_cmd(0x16, args, 6, NULL);                 // BCST_DATA
        rfu_cmd(0x19, NULL, 0, NULL);                 // HOST_START
        phase = 1;
      } else if (phase == 1) {
        n = rfu_cmd(0x1a, NULL, 0, resp);             // HOST_ACCEPT: list clients
        if (n >= 1) { printf("[host] client joined: devid=%04x slot=%u (frame %u)\n", resp[0] & 0xFFFF, resp[0] >> 16, frame); phase = 2; connected_at = frame; }
      }
    } else {
      if (phase == 0) { rfu_cmd(0x1c, NULL, 0, NULL); phase = 1; }   // BCRD_START
      else if (phase == 1) {
        n = rfu_cmd(0x1d, NULL, 0, resp);             // BCRD_FETCH
        for (i = 0; i + 7 <= n; i += 7) if (resp[i + 1] == GAME_ID) host_devid = resp[i] & 0xFFFF;
        if (host_devid) {
          rfu_cmd(0x1e, NULL, 0, NULL);               // BCRD_STOP
          args[0] = host_devid; rfu_cmd(0x1f, args, 1, NULL);   // CONNECT
          printf("[guest] found host devid=%04x (frame %u), connecting\n", host_devid, frame);
          phase = 2;
        }
      } else if (phase == 2) {
        n = rfu_cmd(0x20, NULL, 0, resp);             // ISCONNECTED
        if (n == 1 && !(resp[0] & 0xFF000000)) {
          devid = resp[0] & 0xFFFF; clnum = resp[0] >> 16;
          rfu_cmd(0x21, NULL, 0, resp);               // CONCOMPL
          printf("[guest] connected: devid=%04x slot=%u (frame %u)\n", devid, clnum, frame);
          phase = 3; connected_at = frame;
        } else if (n == 1 && (resp[0] & 0x02000000)) {
          printf("[guest] connection failed\n"); return 1;
        }
      }
    }

    // Data exchange: one numbered 12-byte packet per frame each way.
    if ((as_host && phase == 2) || (!as_host && phase == 3)) {
      args[0] = as_host ? 12 : (12u << (8 + clnum * 5));
      args[1] = tx_seq; args[2] = payload_word(tx_seq, 1, as_host); args[3] = payload_word(tx_seq, 2, as_host);
      if (rfu_cmd(0x24, args, 4, NULL) >= 0) tx_seq++;   // SEND_DATA
      for (;;) {                                          // RECV_DATA until empty
        n = rfu_cmd(0x26, NULL, 0, resp);
        if (n <= 1) break;
        {
          u32 bytes = as_host ? ((resp[0] >> 8) & 0x1F) : resp[0];
          u32 seq = resp[1];
          if (bytes != 12 || resp[2] != payload_word(seq, 1, !as_host) || resp[3] != payload_word(seq, 2, !as_host)) rx_bad++;
          else { if (seq != rx_last + 1) rx_gap++; else rx_ok++; rx_last = seq; }
        }
      }
    }

    link_frame_update();
    mock_pump();
    if (!link_is_active()) { printf("[%s] session closed by peer at frame %u\n", role, frame); break; }
    sleep_until(t0 + (u64)(frame + 1) * 16666667ull);
  }

  {
    u32 active = connected_at ? frame - connected_at : 0;
    int pass = connected_at && rx_bad == 0 && rx_ok + rx_gap > active * 0.8 && rx_gap * 50 < rx_ok + 1;
    printf("[%s] %s | mode=%d connected@%u sent=%u recv_in_order=%u gaps=%u bad=%u | net dgrams=%lu dropped=%lu | %s\n",
           role, pass ? "PASS" : "FAIL", serial_mode, connected_at, tx_seq - 1, rx_ok, rx_gap, rx_bad,
           mock_sent, mock_dropped, link_status_line2());
    link_shutdown();
    return pass ? 0 : 1;
  }
}
