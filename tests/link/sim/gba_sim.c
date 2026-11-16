/* Integration test: a minimal GBA model running a faithful C port of the
 * Pokemon Gen3 serial link driver (pret/pokeemerald src/link.c: handshake,
 * send/recv queues, checksums, Timer3-paced transfers, serial IRQ), on top
 * of FrogGBA's real link code (link.c, link_rel.c, serial.c, serial_proto.c,
 * rfu.c). Two processes = two PSPs, connected through psp_mock.c.
 *
 * Each side enqueues one numbered 8-word command block per frame, like the
 * game does, and verifies the blocks received from the other side: they
 * must arrive complete, uncorrupted and in sequence, without the driver
 * ever flagging a bad checksum.
 *
 * Usage: LINK_ROLE=host|guest ./gba_sim <game_code> <title> <seconds>
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

// ---------------------------------------------------------------- GBA model
#define CYCLES_PER_LINE   1232
#define LINES_PER_FRAME   228
#define CYCLES_PER_FRAME  (CYCLES_PER_LINE * LINES_PER_FRAME)
#define TM3_PERIOD        (197 * 64)

static int ime_serial, ime_timer3;
static int tm3_enabled, tm3_irq_enable;
static s32 tm3_left;

static void w_siocnt(u16 v) { if (serial_mode != SERIAL_MODE_DISABLED) write_siocnt(v); else io_registers[REG_SIOCNT] = v; }
static void w_rcnt(u16 v)   { if (serial_mode != SERIAL_MODE_DISABLED) write_rcnt(v);   else io_registers[REG_RCNT] = v; }
#define R_SIOCNT          (io_registers[REG_SIOCNT])
#define W_SIOMLT_SEND(v)  (io_registers[REG_SIOMLT_SEND] = (u16)(v))
static void w_siomlt_recv_zero(void) { int i; for (i = 0; i < 4; i++) io_registers[REG_SIOMULTI0 + i] = 0; }
static void r_siomlt_recv(u16 *out) { int i; for (i = 0; i < 4; i++) out[i] = io_registers[REG_SIOMULTI0 + i]; }

static void tm3_set_enable(int en) {
  if (en && !tm3_enabled) tm3_left = TM3_PERIOD;   // enabling reloads the counter
  tm3_enabled = en;
}

// ------------------------------------------------- pokeemerald link driver
#define CMD_LENGTH        8
#define QUEUE_CAPACITY    50
#define MAX_LINK_PLAYERS  4
#define MASTER_HANDSHAKE  0x8FFF
#define SLAVE_HANDSHAKE   0xB9A0
#define LINK_SLAVE        0
#define LINK_MASTER       8
#define SIO_MULTI_MODE    0x2000
#define SIO_115200_BPS    0x0003
#define SIO_INTR_ENABLE   0x4000
#define SIO_START         0x0080
#define SIO_MULTI_SI      0x0004
#define SIO_MULTI_SD      0x0008
#define SIO_ID_SHIFT      4
#define SIO_ERROR         0x0040
enum { LINK_STATE_START0, LINK_STATE_START1, LINK_STATE_HANDSHAKE, LINK_STATE_INIT_TIMER, LINK_STATE_CONN_ESTABLISHED };
enum { LAG_NONE, LAG_MASTER, LAG_SLAVE };
enum { QUEUE_FULL_NONE, QUEUE_FULL_SEND, QUEUE_FULL_RECV };

static struct {
  u8 isMaster, state, localId, playerCount;
  u8 receivedNothing;
  s8 serialIntrCounter;
  u8 handshakeAsMaster, link_field_F;
  u8 hardwareError, badChecksum, queueFull, lag;
  u16 checksum;
  u8 sendCmdIndex, recvCmdIndex;
  struct { u16 data[CMD_LENGTH][QUEUE_CAPACITY]; u8 pos, count; } sendQueue;
  struct { u16 data[MAX_LINK_PLAYERS][CMD_LENGTH][QUEUE_CAPACITY]; u8 pos, count; } recvQueue;
  u16 handshakeBuffer[MAX_LINK_PLAYERS];
} gLink;

static u8 sNumVBlanksWithoutSerialIntr, sSendBufferEmpty, sHandshakePlayerCount;
static u16 sSendNonzeroCheck, sRecvNonzeroCheck;
static u8 sChecksumAvailable;
static u32 stat_bad_checksum_events;
static u32 frame = 0, first_bad = 0, first_corrupt = 0, first_gap = 0, first_qfull = 0;

static void DisableSerial(void) {
  ime_serial = 0; ime_timer3 = 0;
  w_siocnt(SIO_MULTI_MODE);
  tm3_set_enable(0); tm3_irq_enable = 0;
  W_SIOMLT_SEND(0);
  w_siomlt_recv_zero();
  memset(&gLink, 0, sizeof(gLink));
}

static void EnableSerial(void) {
  ime_serial = 0; ime_timer3 = 0;
  w_rcnt(0);
  w_siocnt(SIO_MULTI_MODE);
  w_siocnt(R_SIOCNT | SIO_115200_BPS | SIO_INTR_ENABLE);
  ime_serial = 1;
  W_SIOMLT_SEND(0);
  memset(&gLink, 0, sizeof(gLink));
  sNumVBlanksWithoutSerialIntr = 0;
  sSendNonzeroCheck = 0; sRecvNonzeroCheck = 0;
  sChecksumAvailable = 0; sHandshakePlayerCount = 0;
}

static void CheckMasterOrSlave(void) {
  u32 terminals = R_SIOCNT & (SIO_MULTI_SD | SIO_MULTI_SI);
  gLink.isMaster = (terminals == SIO_MULTI_SD && gLink.localId == 0) ? LINK_MASTER : LINK_SLAVE;
}

static void InitTimer(void) {
  if (gLink.isMaster) { tm3_irq_enable = 1; ime_timer3 = 1; }   // TM3CNT_L = -197, 64clk, irq (not enabled yet)
}

static void EnqueueSendCmd(u16 *sendCmd) {
  u8 i, offset;
  if (gLink.sendQueue.count < QUEUE_CAPACITY) {
    offset = gLink.sendQueue.pos + gLink.sendQueue.count;
    if (offset >= QUEUE_CAPACITY) offset -= QUEUE_CAPACITY;
    for (i = 0; i < CMD_LENGTH; i++) {
      sSendNonzeroCheck |= *sendCmd;
      gLink.sendQueue.data[i][offset] = *sendCmd;
      *sendCmd = 0; sendCmd++;
    }
  } else gLink.queueFull = QUEUE_FULL_SEND;
  if (sSendNonzeroCheck) { gLink.sendQueue.count++; sSendNonzeroCheck = 0; }
}

static void DequeueRecvCmds(u16 (*recvCmds)[CMD_LENGTH]) {
  u8 i, j;
  if (gLink.recvQueue.count == 0) {
    for (i = 0; i < gLink.playerCount; i++) for (j = 0; j < CMD_LENGTH; j++) recvCmds[i][j] = 0;
    gLink.receivedNothing = 1;
  } else {
    for (i = 0; i < gLink.playerCount; i++) for (j = 0; j < CMD_LENGTH; j++)
      recvCmds[i][j] = gLink.recvQueue.data[i][j][gLink.recvQueue.pos];
    gLink.recvQueue.count--;
    if (++gLink.recvQueue.pos >= QUEUE_CAPACITY) gLink.recvQueue.pos = 0;
    gLink.receivedNothing = 0;
  }
}

static void LinkMain1(u8 *shouldAdvanceLinkState, u16 *sendCmd, u16 (*recvCmds)[CMD_LENGTH]) {
  switch (gLink.state) {
    case LINK_STATE_START0: DisableSerial(); gLink.state = 1; break;
    case LINK_STATE_START1: if (*shouldAdvanceLinkState == 1) { EnableSerial(); gLink.state = 2; } break;
    case LINK_STATE_HANDSHAKE:
      switch (*shouldAdvanceLinkState) {
        default: CheckMasterOrSlave(); break;
        case 1: if (gLink.isMaster == LINK_MASTER && gLink.playerCount > 1) gLink.handshakeAsMaster = 1; break;
        case 2: gLink.state = LINK_STATE_START0; W_SIOMLT_SEND(0); break;
      }
      break;
    case LINK_STATE_INIT_TIMER: InitTimer(); gLink.state = LINK_STATE_CONN_ESTABLISHED; /* fallthrough */
    case LINK_STATE_CONN_ESTABLISHED: EnqueueSendCmd(sendCmd); DequeueRecvCmds(recvCmds); break;
  }
  *shouldAdvanceLinkState = 0;
}

static void StartTransfer(void) { w_siocnt(R_SIOCNT | SIO_START); }
static void StopTimer(void) { if (gLink.isMaster) tm3_set_enable(0); }

static void LinkVSync(void) {
  if (gLink.isMaster) {
    switch (gLink.state) {
      case LINK_STATE_CONN_ESTABLISHED:
        if (gLink.serialIntrCounter < 9) {
          if (gLink.hardwareError != 1) gLink.lag = LAG_MASTER; else StartTransfer();
        } else if (gLink.lag != LAG_MASTER) { gLink.serialIntrCounter = 0; StartTransfer(); }
        break;
      case LINK_STATE_HANDSHAKE: StartTransfer(); break;
    }
  } else if (gLink.state == LINK_STATE_CONN_ESTABLISHED || gLink.state == LINK_STATE_HANDSHAKE) {
    if (++sNumVBlanksWithoutSerialIntr > 10) {
      if (gLink.state == LINK_STATE_CONN_ESTABLISHED) gLink.lag = LAG_SLAVE;
      if (gLink.state == LINK_STATE_HANDSHAKE) { gLink.playerCount = 0; gLink.link_field_F = 0; }
    }
  }
}

static int DoHandshake(void) {
  u8 i, playerCount = 0; u16 minRecv = 0xFFFF;
  W_SIOMLT_SEND(gLink.handshakeAsMaster == 1 ? MASTER_HANDSHAKE : SLAVE_HANDSHAKE);
  r_siomlt_recv(gLink.handshakeBuffer);
  w_siomlt_recv_zero();
  gLink.handshakeAsMaster = 0;
  for (i = 0; i < MAX_LINK_PLAYERS; i++) {
    if ((gLink.handshakeBuffer[i] & ~0x3) == SLAVE_HANDSHAKE || gLink.handshakeBuffer[i] == MASTER_HANDSHAKE) {
      playerCount++;
      if (minRecv > gLink.handshakeBuffer[i] && gLink.handshakeBuffer[i] != 0) minRecv = gLink.handshakeBuffer[i];
    } else {
      if (gLink.handshakeBuffer[i] != 0xFFFF) playerCount = 0;
      break;
    }
  }
  gLink.playerCount = playerCount;
  if (gLink.playerCount > 1 && gLink.playerCount == sHandshakePlayerCount && gLink.handshakeBuffer[0] == MASTER_HANDSHAKE)
    return 1;
  gLink.link_field_F = (gLink.playerCount > 1) ? (minRecv & 3) + 1 : 0;
  sHandshakePlayerCount = gLink.playerCount;
  return 0;
}

static void DoRecv(void) {
  u16 recv[4]; u8 i, index;
  r_siomlt_recv(recv);
  if (gLink.sendCmdIndex == 0) {
    for (i = 0; i < gLink.playerCount; i++)
      if (gLink.checksum != recv[i] && sChecksumAvailable) { gLink.badChecksum = 1; stat_bad_checksum_events++; if (!first_bad) first_bad = frame; }
    gLink.checksum = 0;
    sChecksumAvailable = 1;
  } else {
    index = gLink.recvQueue.pos + gLink.recvQueue.count;
    if (index >= QUEUE_CAPACITY) index -= QUEUE_CAPACITY;
    if (gLink.recvQueue.count < QUEUE_CAPACITY) {
      for (i = 0; i < gLink.playerCount; i++) {
        gLink.checksum += recv[i];
        sRecvNonzeroCheck |= recv[i];
        gLink.recvQueue.data[i][gLink.recvCmdIndex][index] = recv[i];
      }
    } else gLink.queueFull = QUEUE_FULL_RECV;
    gLink.recvCmdIndex++;
    if (gLink.recvCmdIndex == CMD_LENGTH && sRecvNonzeroCheck) { gLink.recvQueue.count++; sRecvNonzeroCheck = 0; }
  }
}

static void DoSend(void) {
  if (gLink.sendCmdIndex == CMD_LENGTH) {
    W_SIOMLT_SEND(gLink.checksum);
    if (!sSendBufferEmpty) {
      gLink.sendQueue.count--;
      if (++gLink.sendQueue.pos >= QUEUE_CAPACITY) gLink.sendQueue.pos = 0;
    } else sSendBufferEmpty = 0;
  } else {
    if (!sSendBufferEmpty && gLink.sendQueue.count == 0) sSendBufferEmpty = 1;
    if (sSendBufferEmpty) W_SIOMLT_SEND(0);
    else W_SIOMLT_SEND(gLink.sendQueue.data[gLink.sendCmdIndex][gLink.sendQueue.pos]);
    gLink.sendCmdIndex++;
  }
}

static void SendRecvDone(void) {
  if (gLink.recvCmdIndex == CMD_LENGTH) { gLink.sendCmdIndex = 0; gLink.recvCmdIndex = 0; }
  else if (gLink.isMaster) tm3_set_enable(1);
}

static void SerialCB(void) {
  gLink.localId = (R_SIOCNT >> SIO_ID_SHIFT) & 3;
  switch (gLink.state) {
    case LINK_STATE_CONN_ESTABLISHED:
      gLink.hardwareError = (R_SIOCNT & SIO_ERROR) ? 1 : 0;
      DoRecv(); DoSend(); SendRecvDone();
      break;
    case LINK_STATE_HANDSHAKE:
      if (DoHandshake()) {
        if (gLink.isMaster) { gLink.state = LINK_STATE_INIT_TIMER; gLink.serialIntrCounter = 8; }
        else gLink.state = LINK_STATE_CONN_ESTABLISHED;
      }
      break;
  }
  gLink.serialIntrCounter++;
  sNumVBlanksWithoutSerialIntr = 0;
}

static void Timer3Intr(void) { StopTimer(); StartTransfer(); }

// ------------------------------------------------------------ test driver
static u16 block_word(u32 seq, int i) { return (u16)((seq * 2654435761u) >> (i * 3)) ^ (u16)(i * 0x1111); }

static u32 tx_seq = 1, rx_expected = 1;
static u32 rx_ok, rx_gap, rx_corrupt, rx_empty, max_gap;
static u32 conn_frame = 0, lag_frames = 0, queuefull_events = 0, catchup_frames = 0;

static void make_block(u16 *cmd, u32 seq) {
  int i;
  cmd[0] = 0x8000 | (seq & 0x7FFF);
  cmd[1] = (u16)(seq >> 15);
  for (i = 2; i < CMD_LENGTH; i++) cmd[i] = block_word(seq, i);
}

static void check_block(const u16 *cmd) {
  int i, zero = 1; u32 seq;
  for (i = 0; i < CMD_LENGTH; i++) if (cmd[i]) zero = 0;
  if (zero) { rx_empty++; return; }
  if (!(cmd[0] & 0x8000)) { rx_corrupt++; if (!first_corrupt) first_corrupt = frame; return; }
  seq = (cmd[0] & 0x7FFF) | ((u32)cmd[1] << 15);
  for (i = 2; i < CMD_LENGTH; i++) if (cmd[i] != block_word(seq, i)) { rx_corrupt++; if (!first_corrupt) first_corrupt = frame; return; }
  if (seq != rx_expected) {
    u32 g = seq - rx_expected;
    rx_gap++; if (g > max_gap) max_gap = g; if (!first_gap) first_gap = frame;
  } else rx_ok++;
  rx_expected = seq + 1;
}

static void sleep_until(u64 target_ns) {
  struct timespec ts; ts.tv_sec = target_ns / 1000000000ull; ts.tv_nsec = target_ns % 1000000000ull;
  clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &ts, NULL);
}

int main(int argc, char **argv) {
  const char *role = getenv("LINK_ROLE");
  int as_host = role && !strcmp(role, "host");
  const char *code = argc > 1 ? argv[1] : "AXVE";
  const char *title = argc > 2 ? argv[2] : "POKEMON RUBY";
  int seconds = argc > 3 ? atoi(argv[3]) : 20;
  const char *mode_env = getenv("LINK_MODE");
  u8 adv = 0; int master_ready_frames = 0;
  u16 sendCmd[CMD_LENGTH], recvCmds[MAX_LINK_PLAYERS][CMD_LENGTH];
  struct timespec ts; u64 t0;

  if (mode_env) option_link_mode = atoi(mode_env);
  setvbuf(stdout, NULL, _IOLBF, 0);
  if (getenv("LINK_LOGDIR")) snprintf(main_path, sizeof(main_path), "%s/", getenv("LINK_LOGDIR"));  /* PSP style: trailing slash */

  io_registers[REG_RCNT] = 0x800F;   // FrogGBA reset values
  io_registers[REG_SIOCNT] = 0x0004;

  link_game_loaded(code, title);
  if (!link_connect(as_host)) { printf("[%s] connect failed: %s\n", role, link_status_line1()); return 2; }
  printf("[%s] %s\n", role, link_status_line1());

  clock_gettime(CLOCK_MONOTONIC, &ts);
  t0 = (u64)ts.tv_sec * 1000000000ull + ts.tv_nsec;

  for (frame = 0; frame < (u32)(seconds * 60); frame++) {
    s32 frame_left = CYCLES_PER_FRAME, vblank_at = CYCLES_PER_FRAME - 160 * CYCLES_PER_LINE;
    int vblank_done = 0;

    while (frame_left > 0) {
      s32 slice = frame_left < CYCLES_PER_LINE / 2 ? frame_left : CYCLES_PER_LINE / 2;
      u32 se = serial_next_event();
      if (serial_mode != SERIAL_MODE_DISABLED && se < (u32)slice) slice = se ? (s32)se : 1;
      if (tm3_enabled && tm3_left < slice) slice = tm3_left > 0 ? tm3_left : 1;

      cpu_ticks += slice; frame_left -= slice;

      if (serial_mode != SERIAL_MODE_DISABLED && update_serial(slice) && ime_serial) SerialCB();

      if (tm3_enabled) {
        tm3_left -= slice;
        if (tm3_left <= 0) { tm3_left += TM3_PERIOD; if (tm3_irq_enable && ime_timer3) Timer3Intr(); }
      }

      if (!vblank_done && frame_left <= vblank_at) {
        vblank_done = 1;
        LinkVSync();
        // Game main loop for this frame
        if (gLink.state == LINK_STATE_START1) adv = 1;
        if (gLink.state == LINK_STATE_HANDSHAKE && gLink.isMaster == LINK_MASTER && gLink.playerCount > 1) {
          if (++master_ready_frames == 30) adv = 1;   // "press A to start" on the master
        }
        if (gLink.state == LINK_STATE_CONN_ESTABLISHED) {
          if (!conn_frame) { conn_frame = frame; printf("[%s] link established at frame %u (players=%u, master=%d)\n", role, frame, gLink.playerCount, gLink.isMaster == LINK_MASTER); }
          make_block(sendCmd, tx_seq++);
        } else memset(sendCmd, 0, sizeof sendCmd);
        LinkMain1(&adv, sendCmd, recvCmds);
        if (gLink.state == LINK_STATE_CONN_ESTABLISHED && conn_frame && frame > conn_frame) {
          check_block(recvCmds[gLink.localId ^ 1]);
          // pokeemerald AgbMain: Overworld_RecvKeysFromLinkIsRunning() runs one
          // extra link update in the same frame when the receive queue backs up
          // (GetLinkRecvQueueLength() >= OVERWORLD_RECV_QUEUE_MAX - 1).
          if (gLink.recvQueue.count >= 2) {
            memset(sendCmd, 0, sizeof sendCmd);
            LinkMain1(&adv, sendCmd, recvCmds);
            check_block(recvCmds[gLink.localId ^ 1]);
            catchup_frames++;
          }
          if (gLink.lag) lag_frames++;
          if (gLink.queueFull) { queuefull_events++; if (!first_qfull) first_qfull = frame; gLink.queueFull = 0; }
        }
      }
    }

    link_frame_update();   // FrogGBA calls this once per frame
    mock_pump();
    if (!link_is_active()) { printf("[%s] session closed by peer at frame %u\n", role, frame); break; }

    sleep_until(t0 + (u64)(frame + 1) * 16666667ull);
  }

  {
    u32 sent_blocks = tx_seq - 1;
    int pass = conn_frame && rx_ok > (u32)((frame - conn_frame) * 0.8) && rx_corrupt == 0 && rx_gap == 0 && stat_bad_checksum_events == 0;
    printf("[%s] %s | mode=%d established@%u sent=%u recv_ok=%u gaps=%u(max %u) corrupt=%u empty=%u bad_checksum=%u lag_frames=%u qfull=%u first(bad/corrupt/gap/qfull)=%u/%u/%u/%u frames=%u catchup=%u | net: dgrams=%lu dropped=%lu recv=%lu retx=%s\n",
           role, pass ? "PASS" : "FAIL", serial_mode, conn_frame, sent_blocks, rx_ok, rx_gap, max_gap, rx_corrupt, rx_empty,
           stat_bad_checksum_events, lag_frames, queuefull_events, first_bad, first_corrupt, first_gap, first_qfull, frame, catchup_frames, mock_sent, mock_dropped, mock_recv, link_status_line2());
    link_shutdown();
    return pass ? 0 : 1;
  }
}
