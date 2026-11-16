/* FrogGBA - Link over ad-hoc: diagnostic log
 *
 * While a link session is open (and the "Debug log" option is on) events
 * are written to <FrogGBA folder>/link_log.txt: connection steps, every
 * Wireless Adapter command, serial mode changes, protocol state changes,
 * plus a one line snapshot per second of the CPU / serial / timer / network
 * state. Lines are buffered in RAM and written about once per second;
 * identical consecutive lines are collapsed ("repeated N times").
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 2 of
 * the License, or (at your option) any later version.
 */

#ifndef LINK_LOG_H
#define LINK_LOG_H

extern u32 option_link_log;      // 1 = write link_log.txt during sessions
extern int link_log_active;      // Log file open (session running)

typedef struct
{
  u32 sio_normal;      // Normal mode transfers started (internal clock)
  u32 sio_multi;       // Multiplayer transfers started (master)
  u32 sio_irq;         // Serial IRQs raised by the emulation
  u32 sio_clk_switch;  // Internal <-> external clock switches
  u32 rfu_cmds;        // Wireless Adapter commands processed
  u32 rfu_errors;      // Commands answered with an error
  u32 rfu_waits;       // Reverse clock waits started
  u32 rfu_ev_data;     // Wait ended: data available
  u32 rfu_ev_noresp;   // Wait ended: host got no client answer
  u32 rfu_ev_timeout;  // Wait ended: timeout
  u32 rfu_ev_disc;     // Wait ended: disconnected
  u32 rfu_words_out;   // Words pushed to the GBA as adapter (slave mode)
  u32 rfu_push_block;  // Pushes delayed because SO/SI/START not ready
  u32 rfu_pkt_tx;      // Wireless: data packets sent to the peer
  u32 rfu_pkt_rx;      // Wireless: data packets received from the peer
  u32 rfu_q_max;       // Wireless: deepest receive queue seen
  u32 rfu_q_drops;     // Wireless: packets dropped (receive queue full)
  u32 rfu_ev_stall;    // Wireless host: waits answered "no ACK" (client behind)
  u32 rfu_paced;       // Wireless client: waits held back (one packet per frame)
  u32 rfu_lag_max;     // Wireless host: most host packets a client had unread
  u32 rfu_rtx;         // Wireless: retransmission requests (RTX_WAIT)
  u32 net_msg_tx;      // Protocol messages sent
  u32 net_msg_rx;      // Protocol messages received
  u32 poke_drops;      // Cable mode: packets dropped (queue full)
  u32 poke_slave_irq;  // Cable mode: fake master IRQs (slave side)
  u32 poke_blocks;     // Cable mode: 1+8 word blocks completed
  u32 poke_nopeer;     // Cable mode: blocks completed without peer data
  u32 poke_rx_data;    // Cable mode: data packets received
  u32 poke_tx_data;    // Cable mode: data packets sent
} link_counters_t;

extern link_counters_t link_ctr;

void link_log_start(const char *role, const char *mode_name,
                    const char *game_code, const char *game_title);
void link_log_stop(const char *reason);
// Note: u32 is "unsigned long" on the PSP (32 bit), so %u is fine at runtime.
void link_log(const char *fmt, ...);
void link_log_flush(void);

// "Debug log: link_log.txt", "Debug log FAILED (code)" or "Debug log: off".
const char *link_log_status(void);

// Once per emulated frame (summary every second, periodic flush).
void link_log_frame(void);

#define LLOG(...)                     \
  do                                  \
  {                                   \
    if (link_log_active)              \
      link_log(__VA_ARGS__);          \
  } while (0)

#endif
