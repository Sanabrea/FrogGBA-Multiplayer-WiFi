/* FrogGBA - Link over ad-hoc: diagnostic log (see link_log.h)
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 2 of
 * the License, or (at your option) any later version.
 */

#include "common.h"
#include <stdarg.h>
#include <psprtc.h>

#define LOG_BUF_SIZE       (96 * 1024)
#define LOG_FLUSH_AT       (64 * 1024)
#define LOG_MAX_FILE       (3 * 1024 * 1024)   // Start a new file above this
#define LOG_MAX_LINES_SEC  1500                // Runaway protection

u32 option_link_log = 1;
int link_log_active = 0;
link_counters_t link_ctr;

static SceUID log_fd = -1;
static char log_status[64] = "";
static char log_buf[LOG_BUF_SIZE];
static u32 log_len = 0;

static char last_msg[192];
static u32 last_repeat = 0;
static u32 lines_this_sec = 0;
static u32 dropped_this_sec = 0;

static u32 log_frame = 0;
static u32 frame_start_ticks = 0;
static u32 pc_samples[4];

static void log_append(const char *text, u32 len)
{
  if (log_len + len > LOG_BUF_SIZE)
    link_log_flush();

  if (len > LOG_BUF_SIZE)
    len = LOG_BUF_SIZE;

  memcpy(log_buf + log_len, text, len);
  log_len += len;
}

static void log_raw_line(const char *msg)
{
  char line[256];
  int n = snprintf(line, sizeof(line), "[f%06u +%06u] %s\n", (unsigned)log_frame,
                   (unsigned)(cpu_ticks - frame_start_ticks), msg);

  if (n > (int)sizeof(line) - 1)
    n = sizeof(line) - 1;

  log_append(line, n);
}

static void emit_repeat(void)
{
  if (last_repeat)
  {
    char msg[64];
    snprintf(msg, sizeof(msg), "    ^ repeated %u more times", (unsigned)last_repeat);
    log_raw_line(msg);
    last_repeat = 0;
  }
}

void link_log(const char *fmt, ...)
{
  char msg[192];
  va_list ap;
  int n;

  if (!link_log_active)
    return;

  va_start(ap, fmt);
  n = vsnprintf(msg, sizeof(msg), fmt, ap);
  va_end(ap);

  // Strip trailing newlines (ported code logs with "\n").
  if (n > (int)sizeof(msg) - 1)
    n = sizeof(msg) - 1;
  while (n > 0 && (msg[n - 1] == '\n' || msg[n - 1] == '\r'))
    msg[--n] = 0;

  if (strcmp(msg, last_msg) == 0)
  {
    last_repeat++;
    return;
  }

  emit_repeat();
  strcpy(last_msg, msg);

  if (lines_this_sec >= LOG_MAX_LINES_SEC)
  {
    dropped_this_sec++;
    return;
  }

  lines_this_sec++;
  log_raw_line(msg);
}

void link_log_flush(void)
{
  if (log_fd >= 0 && log_len)
    sceIoWrite(log_fd, log_buf, log_len);

  log_len = 0;
}

void link_log_start(const char *role, const char *mode_name,
                    const char *game_code, const char *game_title)
{
  char path[MAX_PATH];
  SceIoStat st;
  int flags = PSP_O_WRONLY | PSP_O_CREAT | PSP_O_APPEND;
  ScePspDateTime t;
  char hdr[256];

  if (link_log_active)
    link_log_stop("restart");

  if (!option_link_log)
  {
    snprintf(log_status, sizeof(log_status), "Debug log: off");
    return;
  }

  // main_path already ends with '/' on the PSP ("ms0:/PSP/GAME/FrogGBA/");
  // a double slash makes sceIoOpen fail.
  {
    size_t n = strlen(main_path);
    snprintf(path, sizeof(path), "%s%slink_log.txt", main_path,
             (n && main_path[n - 1] == '/') ? "" : "/");
  }

  memset(&st, 0, sizeof(st));
  if (sceIoGetstat(path, &st) >= 0 && st.st_size > LOG_MAX_FILE)
    flags = PSP_O_WRONLY | PSP_O_CREAT | PSP_O_TRUNC;

  log_fd = sceIoOpen(path, flags, 0777);
  if (log_fd < 0)
  {
    snprintf(log_status, sizeof(log_status), "Debug log FAILED (%08X)", (unsigned)log_fd);
    log_fd = -1;
    return;
  }

  snprintf(log_status, sizeof(log_status), "Debug log: link_log.txt");

  log_len = 0;
  last_msg[0] = 0;
  last_repeat = 0;
  lines_this_sec = 0;
  dropped_this_sec = 0;
  log_frame = 0;
  frame_start_ticks = cpu_ticks;
  memset(&link_ctr, 0, sizeof(link_ctr));
  memset(pc_samples, 0, sizeof(pc_samples));
  link_log_active = 1;

  memset(&t, 0, sizeof(t));
  sceRtcGetCurrentClockLocalTime(&t);

  snprintf(hdr, sizeof(hdr),
           "\n==== FrogGBA link log  %04d-%02d-%02d %02d:%02d:%02d ====\n"
           "role=%s mode=%s game=[%s] %s\n",
           t.year, t.month, t.day, t.hour, t.minute, t.second,
           role, mode_name, game_code, game_title);
  log_append(hdr, strlen(hdr));
}

void link_log_stop(const char *reason)
{
  if (!link_log_active)
    return;

  emit_repeat();
  link_log("log closed: %s", reason);
  emit_repeat();
  link_log_flush();

  sceIoClose(log_fd);
  log_fd = -1;
  link_log_active = 0;
}

static void log_summary(void)
{
  u32 rfu_comstate, rfu_state, rfu_clients, rfu_bcst;
  u32 pk_state[2], pk_count[2];
  u32 ln_state, ln_retx, ln_unacked, ln_full, ln_tx, ln_rx, ln_ms_rx;
  u32 ln_srtt, ln_rto, ln_err;
  u32 cpsr = reg[REG_CPSR];

  rfu_debug_info(&rfu_comstate, &rfu_state, &rfu_clients, &rfu_bcst);
  serialpoke_debug_info(pk_state, pk_count);
  link_debug_info(&ln_state, &ln_retx, &ln_unacked, &ln_full, &ln_tx, &ln_rx, &ln_ms_rx,
                  &ln_srtt, &ln_rto, &ln_err);

  link_log("S cpu pc=%08X/%08X/%08X/%08X %s halt=%u ime=%u ie=%04X if=%04X",
           pc_samples[0], pc_samples[1], pc_samples[2], pc_samples[3],
           (cpsr & 0x20) ? "T" : "A", (unsigned)reg[CPU_HALT_STATE],
           io_registers[REG_IME], io_registers[REG_IE], io_registers[REG_IF]);

  link_log("S sio cnt=%04X rcnt=%04X d32=%04X%04X mul=%04X/%04X/%04X/%04X send=%04X tm2=%04X:%04X tm3=%04X:%04X",
           io_registers[REG_SIOCNT], io_registers[REG_RCNT],
           io_registers[REG_SIODATA32_H], io_registers[REG_SIODATA32_L],
           io_registers[REG_SIOMULTI0], io_registers[REG_SIOMULTI1],
           io_registers[REG_SIOMULTI2], io_registers[REG_SIOMULTI3],
           io_registers[REG_SIOMLT_SEND],
           io_registers[REG_TM2CNT], io_registers[REG_TM2D],
           io_registers[REG_TM3CNT], io_registers[REG_TM3D]);

  link_log("S ctr nrm=%u mul=%u irq=%u clk=%u | rfu cs=%u st=%u cl=%u bc=%u cmd=%u err=%u wait=%u ev(d/n/t/x)=%u/%u/%u/%u out=%u blk=%u pkt tx/rx=%u/%u qmax=%u qdrop=%u stall=%u paced=%u lag=%u rtx=%u | poke st=%u/%u q=%u/%u blk=%u nopeer=%u tx=%u rx=%u drop=%u sirq=%u",
           link_ctr.sio_normal, link_ctr.sio_multi, link_ctr.sio_irq, link_ctr.sio_clk_switch,
           rfu_comstate, rfu_state, rfu_clients, rfu_bcst,
           link_ctr.rfu_cmds, link_ctr.rfu_errors, link_ctr.rfu_waits,
           link_ctr.rfu_ev_data, link_ctr.rfu_ev_noresp, link_ctr.rfu_ev_timeout, link_ctr.rfu_ev_disc,
           link_ctr.rfu_words_out, link_ctr.rfu_push_block,
           link_ctr.rfu_pkt_tx, link_ctr.rfu_pkt_rx, link_ctr.rfu_q_max, link_ctr.rfu_q_drops,
           link_ctr.rfu_ev_stall, link_ctr.rfu_paced, link_ctr.rfu_lag_max, link_ctr.rfu_rtx,
           pk_state[0], pk_state[1], pk_count[0], pk_count[1],
           link_ctr.poke_blocks, link_ctr.poke_nopeer, link_ctr.poke_tx_data, link_ctr.poke_rx_data,
           link_ctr.poke_drops, link_ctr.poke_slave_irq);

  link_log("S net st=%u msg tx=%u rx=%u | dgram tx=%u rx=%u err=%u | retx=%u unacked=%u full=%u | rtt=%ums rto=%ums | last_rx=%ums ago | dropped_log_lines=%u",
           ln_state, link_ctr.net_msg_tx, link_ctr.net_msg_rx, ln_tx, ln_rx, ln_err,
           ln_retx, ln_unacked, ln_full, ln_srtt, ln_rto, ln_ms_rx, dropped_this_sec);

  memset(&link_ctr, 0, sizeof(link_ctr));
  lines_this_sec = 0;
  dropped_this_sec = 0;
}

void link_log_frame(void)
{
  if (!link_log_active)
    return;

  log_frame++;
  frame_start_ticks = cpu_ticks;

  if ((log_frame % 15) == 0)
    pc_samples[(log_frame / 15) & 3] = reg[REG_PC];

  if ((log_frame % 60) == 0)
  {
    log_summary();
    emit_repeat();
    link_log_flush();
  }
  else if (log_len >= LOG_FLUSH_AT)
  {
    link_log_flush();
  }
}

const char *link_log_status(void)
{
  return log_status;
}
