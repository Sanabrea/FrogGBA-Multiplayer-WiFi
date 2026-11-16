/* gameplaySP
 *
 * Copyright (C) 2023 David Guillen Fandos <david@davidgf.net>
 * Ported to FrogGBA (PSP ad-hoc link). Protocol logic as in gpSP, plus flow
 * control for the PSP transport (see docs/HOW_IT_WORKS.md).
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 2 of
 * the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

#include "common.h"

// Debug print logic:
#ifdef RFU_DEBUG
  #define RFU_DEBUG_LOG(...) printf(__VA_ARGS__)
#else
  // FrogGBA: goes to link_log.txt when the debug log is enabled.
  #define RFU_DEBUG_LOG(...) LLOG("RFU " __VA_ARGS__)
#endif

// Config knobs, update with care.
#define BCST_ANNOUNCE_VB               30   // Send broadcast twice per second.
#define MAX_RFU_PEERS                 2   // Two PSPs: client IDs 0 (host) and 1 (guest).

#define RFU_DEF_TIMEOUT              32   // Expressed as frames (~533ms)
#define RFU_DEF_RTXMAX                4   // Up to 4 transmissions per send

// Packets buffered per peer. The original value (4) drops packets when the
// network delivers a burst (e.g. after a retransmission); 16 absorbs it.
#define RFU_PKT_QUEUE                64   // Ring buffer (power of 2). The ad-hoc
                                           // link delivers in bursts: keep room.
#define RFU_PKT_MASK      (RFU_PKT_QUEUE - 1)

/* FrogGBA flow control for the "clock reversed" (MSC) data mode.
 *
 * On a real adapter the host sends one packet per frame and the client gets
 * it within the same frame, so the games never see more than ~1 packet per
 * frame. Over WiFi packets arrive in bursts (a 300ms hiccup = 18 packets at
 * once). Pokemon's client code (link_rfu_2.c, MscCallback_Child) queues every
 * packet it is handed in a 20 slot queue that the game drains once per
 * frame; when it fills up the game raises a fatal link error and resets the
 * adapter. And since both sides consume exactly what the other produces,
 * any backlog created by a hiccup never drained (latency only grew).
 *
 *  - Client: hand at most one packet per frame to the game (plus a small
 *    burst allowance), keeping the rest in the adapter queue.
 *  - Client: report how many host packets the game has read.
 *  - Host: when the client is more than RFU_HOST_MAX_LAG packets behind,
 *    answer the host wait with "client did not ACK" so the game retransmits
 *    (RTX_WAIT, which we turn into a plain wait: the link is reliable) and
 *    skips a frame, instead of producing even more packets.
 */
#define RFU_CLIENT_BURST              2   // Packets the client game may get in one frame
#define RFU_HOST_MAX_LAG             12   // Unread host packets before the host stalls (~200ms)
#define RFU_HOST_MAX_STALL          180   // Consecutive stalls before giving up (3s)
#define RFU_REPORT_KEEPALIVE         30   // Client reports at least every N frames
#define RFU_REPORT_MAGIC     0x434E5331   // "CNS1": consumed counter in a client packet
#define RFU_REPORT_OFFSET            84   // Its offset in the packet payload (data is <= 16 bytes)

// Per transfer tracing (several lines per frame). Off: the per-second
// counters in the log summary cover it, and logging costs CPU on the PSP.
#ifndef RFU_TRACE_TRANSFERS
#define RFU_TRACE_TRANSFERS 0
#endif
#if RFU_TRACE_TRANSFERS
  #define RFU_TRACE(...) RFU_DEBUG_LOG(__VA_ARGS__)
#else
  #define RFU_TRACE(...) do { } while (0)
#endif


// Unpacks big endian integers used for signaling
static inline u32 upack32(const u8 *ptr) {
  return (u32)ptr[3] | ((u32)ptr[2] << 8) | ((u32)ptr[1] << 16) | ((u32)ptr[0] << 24);
}

// Unpacks payload data, which is little-endian
static inline u32 leupack32(const u8 *ptr) {
  return (u32)ptr[0] | ((u32)ptr[1] << 8) | ((u32)ptr[2] << 16) | ((u32)ptr[3] << 24);
}

// The following commands, names and bit fields are not a 100% known.
// Most of them is guessed via reverse engineering the adapter and/or
// games that use it. Please take it with a grain of salt.
// You might wanna check:
// https://github.com/afska/gba-link-connection/
// https://blog.kuiper.dev/gba-wireless-adapter

#define RFU_CONN_INPROGRESS  0x01000000     // Connection ongoing
#define RFU_CONN_FAILED      0x02000000     // Connection failed

#define RFU_CONN_COMP_FAIL   0x01000000     // Failed to connect

#define RFU_CMD_INIT1        0x10   // Dummy after-init command
#define RFU_CMD_INIT2        0x3d   // Dummy after-init command
#define RFU_CMD_SYSCFG       0x17   // System configuration (comms. config)

#define RFU_CMD_LINKPWR      0x11   // Link/RF strength (0 to 0x16/0xFF)

// These are not really well documented.
#define RFU_CMD_SYSVER       0x12   // Return some 1 word with version info.
#define RFU_CMD_SYSSTAT      0x13   // System/Connection status.
#define RFU_CMD_SLOTSTAT     0x14   // Reads slot status information.
#define RFU_CMD_CFGSTAT      0x15   // Reads some sort of config state.

#define RFU_CMD_BCST_DATA    0x16   // Broadcast data setup

#define RFU_CMD_HOST_START   0x19   // Start broadcasting and accepting clients
#define RFU_CMD_HOST_ACCEPT  0x1a   // Poll for incoming connections
#define RFU_CMD_HOST_STOP    0x1b   // Stop accepting new connections

#define RFU_CMD_BCRD_START   0x1c   // Broadcast read session start
#define RFU_CMD_BCRD_FETCH   0x1d   // Broadcast read (actual data read)
#define RFU_CMD_BCRD_STOP    0x1e   // Broadcast read session end

#define RFU_CMD_CONNECT      0x1f   // Connect to host
#define RFU_CMD_ISCONNECTED  0x20   // Check for conection status
#define RFU_CMD_CONCOMPL     0x21   // Complete connection?

#define RFU_CMD_SEND_DATA    0x24   // Sends a data packet
#define RFU_CMD_SEND_DATAW   0x25   // Sends a data packet and waits
#define RFU_CMD_RECV_DATA    0x26   // Receive (poll) some data
#define RFU_CMD_WAIT         0x27   // Wait (for response or timeout)
#define RFU_CMD_RTX_WAIT     0x37   // Wait after some retransmit?

#define RFU_CMD_DISCONNECT   0x30   // Disconnects clients

// RFU commands for slave mode (~command responses)
#define RFU_CMD_RESP_TIMEO   0x27   // Timeout!
#define RFU_CMD_RESP_DATA    0x28   // There's data available
#define RFU_CMD_RESP_DISC    0x29   // Some clients disconnected


// These are internal FSM states for the communication steps.
#define RFU_COMSTATE_RESET       0    // Just out of reset
#define RFU_COMSTATE_HANDSHAKE   1    // Performing initial nintendo handshake
#define RFU_COMSTATE_WAITCMD     2    // Idle, waiting for a command
#define RFU_COMSTATE_WAITDAT     3    // Waiting for follow up data
#define RFU_COMSTATE_RESPCMD     4    // RFU to device response (cmd)
#define RFU_COMSTATE_RESPDAT     5    // RFU to device response (N words)
#define RFU_COMSTATE_RESPERR     6    // Send back the error command
#define RFU_COMSTATE_RESPERR2    7    // Send back the error code
#define RFU_COMSTATE_WAITEVENT   8    // Waiting for event or timeout
#define RFU_COMSTATE_WAITRESP    9    // Responding a wait command

// These FSM states describe the adapter wifi state.
#define RFU_STATE_IDLE            0
#define RFU_STATE_HOST            1    // Hosting (with broadcast)
#define RFU_STATE_CONNECTING      2    // Trying to connect to a parent
#define RFU_STATE_CLIENT          3    // Client, connected to a host


static u32 rfu_prev_data;
static u32 rfu_comstate, rfu_cnt, rfu_state;
static u32 rfu_buf[255];
static u8 rfu_cmd, rfu_plen;
static u32 rfu_timeout_cycles, rfu_resp_timeout;
static u8 rfu_timeout, rfu_rtx_max;

// FrogGBA diagnostics
static u32 rfu_wait_start_ticks;      // cpu_ticks when the last wait began
static u32 rfu_push_blocked_since;    // cpu_ticks of the first blocked push
static u8  rfu_push_blocked_logged;
static u8  rfu_wait_paced;            // This wait was held back by client pacing

static const char *rfu_cmd_name(u32 cmd) {
  switch (cmd) {
    case 0x10: return "INIT1";        case 0x3d: return "INIT2";
    case 0x11: return "LINKPWR";      case 0x12: return "SYSVER";
    case 0x13: return "SYSSTAT";      case 0x14: return "SLOTSTAT";
    case 0x15: return "CFGSTAT";      case 0x16: return "BCST_DATA";
    case 0x17: return "SYSCFG";       case 0x19: return "HOST_START";
    case 0x1a: return "HOST_ACCEPT";  case 0x1b: return "HOST_STOP";
    case 0x1c: return "BCRD_START";   case 0x1d: return "BCRD_FETCH";
    case 0x1e: return "BCRD_STOP";    case 0x1f: return "CONNECT";
    case 0x20: return "ISCONNECTED";  case 0x21: return "CONCOMPL";
    case 0x24: return "SEND_DATA";    case 0x25: return "SEND_DATAW";
    case 0x26: return "RECV_DATA";    case 0x27: return "WAIT";
    case 0x30: return "DISCONNECT";   case 0x37: return "RTX_WAIT";
  }
  return "?";
}

static void rfu_log_command(u32 cmd, u32 plen, u32 arg0, s32 ret) {
  static u32 last_linkpwr = 0xFFFFFFFF;
  link_ctr.rfu_cmds++;
  if (ret < 0)
    link_ctr.rfu_errors++;
  if (!link_log_active)
    return;
  if (ret >= 0 && !RFU_TRACE_TRANSFERS) {
    // Data path commands run every frame: only counted, not logged.
    if (cmd == RFU_CMD_SEND_DATA || cmd == RFU_CMD_SEND_DATAW ||
        cmd == RFU_CMD_RECV_DATA || cmd == RFU_CMD_WAIT || cmd == RFU_CMD_RTX_WAIT)
      return;
    // Link strength: log only changes.
    if (cmd == RFU_CMD_LINKPWR) {
      u32 v = ret > 0 ? rfu_buf[0] : 0;
      if (v == last_linkpwr)
        return;
      last_linkpwr = v;
    }
  }
  if (ret < 0)
    link_log("RFU cmd %02X %-11s plen=%u a0=%08X -> ERROR %d (state=%u)",
             cmd, rfu_cmd_name(cmd), plen, arg0, (int)-ret, rfu_state);
  else
    link_log("RFU cmd %02X %-11s plen=%u a0=%08X -> %d words r0=%08X (state=%u)",
             cmd, rfu_cmd_name(cmd), plen, arg0, (int)ret,
             ret > 0 ? rfu_buf[0] : 0, rfu_state);
}

static struct {
  u32 buf[23];
  u8 blen;
} rfu_tx_buf;

static struct {
  u16 devid;         // Device ID assigned to the host
  u8 tx_ttl;         // Internal counter for broadcast transmission
  u32 bdata[6];      // Data to broadcast other devices
  struct {
    u16 client_id;   // libretro client ID
    u16 devid;       // Host-assigned device ID
    u16 clttl;       // Client TTL (to check for disconnects)
    u32 h_tx;        // FrogGBA: data packets sent to this client
    u32 c_consumed;  // FrogGBA: packets the client game has read (reported)
    u8 report_valid; // FrogGBA: the client has sent a report
    u8 throttle_off; // FrogGBA: gave up stalling for this client
    u8 qhead, qcount;  // Ring buffer of received packets
    struct {
      u32 datalen;   // Byte count of data waiting to be polled.
      u8  data[16];  // Data received from client.
    } pkts[RFU_PKT_QUEUE];
  } clients[4];      // Connected clients IDs (zero means empty slot).
  u8 recv_block_mask;  // FrogGBA: slots not ACKed in the last event (RECV skips them)
  u16 stall_run;       // FrogGBA: consecutive "no ACK" stalls
} rfu_host;

static struct {
  u16 devid;         // Device ID assigned to the client (by the host?)
  u16 clnum;         // Client number (0 to 3)
  u16 host_id;       // Client ID for the host device.
  u32 consumed;      // FrogGBA: host packets read by the game (RECV_DATA)
  u32 reported;      // FrogGBA: last value sent to the host
  u8 credits;        // FrogGBA: packets that may be handed to the game now
  u8 report_ttl;     // FrogGBA: frames since the last report
  u8 qhead, qcount;  // Ring buffer of packets received from the host
  struct {
    u16 hblen;       // Bytes received from the host.
    u8 hdata[128];   // Data received from the host (accumulated).
  } pkts[RFU_PKT_QUEUE];
} rfu_client;

typedef struct {
  u8 valid;
  u8 ttl;            // When it reaches zero, entry is invalidated
  u16 device_id;     // Random ID generated by the RFU for each new session
  u32 data[6];       // Broadcast data (game+user data)
} t_client_broadcast;

// The table is indexed by client_id
static t_client_broadcast rfu_peer_bcst[MAX_RFU_PEERS];

// Constants used for the network protocol.

#define NET_RFU_HEADER          0x52465531       // RFU1

#define NET_RFU_BROADCAST       0x00    // Host to everyone broadcast packet
#define NET_RFU_CONNECT_REQ     0x01    // Client connection request
#define NET_RFU_CONNECT_ACK     0x02    // Host connection response (accept)
#define NET_RFU_CONNECT_NACK    0x03    // Host connection response (reject)
#define NET_RFU_DISCONNECT      0x04    // Client/Host disconnect notice
#define NET_RFU_HOST_SEND       0x05    // Host to client data send
#define NET_RFU_CLIENT_SEND     0x06    // Client to host data send
#define NET_RFU_CLIENT_ACK      0x07    // Client report: word 3 = host packets read
                                        // by the game (FrogGBA, link protocol 3).


static void rfu_net_send_cmd2(int client_id, u32 ptype, u32 h, u32 w3) {
  u32 pkt[4] = {
    netorder32(NET_RFU_HEADER),  // RFU1 header
    netorder32(ptype),           // Message type
    netorder32(h),               // Header word
    netorder32(w3)
  };
  netpacket_send(client_id, pkt, 16);
}

static void rfu_net_send_cmd(int client_id, u32 ptype, u32 h) {
  rfu_net_send_cmd2(client_id, ptype, h, 0);
}

// Client: tell the host how many of its packets the game has read.
static void rfu_client_report(void) {
  rfu_net_send_cmd2(rfu_client.host_id, NET_RFU_CLIENT_ACK,
                    rfu_client.devid | (rfu_client.clnum << 16), rfu_client.consumed);
  rfu_client.reported = rfu_client.consumed;
  rfu_client.report_ttl = 0;
}

// Host: a client reported how many host packets its game has read.
static void rfu_host_note_consumed(u32 clid, u32 consumed) {
  if (!rfu_host.clients[clid].report_valid ||
      (s32)(consumed - rfu_host.clients[clid].c_consumed) > 0)
    rfu_host.clients[clid].c_consumed = consumed;
  rfu_host.clients[clid].report_valid = 1;
}

// Host: host packets a client has not read yet (0 if unknown).
static u32 rfu_host_lag(u32 clid) {
  s32 lag;
  if (!rfu_host.clients[clid].devid || !rfu_host.clients[clid].report_valid)
    return 0;
  lag = (s32)(rfu_host.clients[clid].h_tx - rfu_host.clients[clid].c_consumed);
  return lag > 0 ? (u32)lag : 0;
}

static void rfu_net_send_bcast(u32 ptype, u32 h, const u32 *pload) {
  u32 i;
  u32 pkt[9] = {
    netorder32(NET_RFU_HEADER),   // RFU1 header
    netorder32(ptype),            // Message type
    netorder32(h),                // Header word
  };
  for (i = 0; i < 6; i++)
    pkt[i + 3] = netorder32(pload[i]);

  // Broadcast to all clients (ID=0xffff)
  netpacket_send(LINK_BROADCAST_ID, pkt, sizeof(pkt));
}

static void rfu_net_send_data(int client_id, u32 ptype, u32 h, const u32 *pload, unsigned plen) {
  u32 i;
  struct {
    u32 header[3];
    u8  data8[92];
  } pkt = {
    {
      netorder32(NET_RFU_HEADER),   // RFU1 header
      netorder32(ptype),            // Message type
      netorder32(h),                // Header word
    },
  };
  // Data is sent in little endian format over the RF link (presumably)
  for (i = 0; i < plen; i++)
    pkt.data8[i] = pload[i / 4] >> (8 * (i & 3));
  memset(&pkt.data8[plen], 0, 92 - plen);

  // FrogGBA: client packets carry the "host packets read" counter, sent
  // after the data it answers (the host relies on that order).
  if (ptype == NET_RFU_CLIENT_SEND && plen <= RFU_REPORT_OFFSET) {
    u32 v[2] = { netorder32(RFU_REPORT_MAGIC), netorder32(rfu_client.consumed) };
    memcpy(&pkt.data8[RFU_REPORT_OFFSET], v, sizeof(v));
    rfu_client.reported = rfu_client.consumed;
    rfu_client.report_ttl = 0;
  }

  link_ctr.rfu_pkt_tx++;
  netpacket_send(client_id, &pkt, 104);
}

// This is called whenever the game uses the GPIO (pin D) to perform a reset
// pin flip in the external reset pin. We reset the device to a known state.
void rfu_reset(void) {
  RFU_DEBUG_LOG("RFU reset!\n");
  // Reset FSMs to a known state
  rfu_prev_data = 0;
  rfu_cnt = 0;
  rfu_state = RFU_STATE_IDLE;
  rfu_comstate = RFU_COMSTATE_RESET;
  rfu_timeout_cycles = 0;
  rfu_resp_timeout = 0;
  rfu_timeout = RFU_DEF_TIMEOUT;
  rfu_rtx_max = RFU_DEF_RTXMAX;
  memset(&rfu_host, 0, sizeof(rfu_host));

  // Clear all the received broadcasts.
  memset(&rfu_peer_bcst, 0, sizeof(rfu_peer_bcst));

  // Re-seed random gen.  cpu_ticks is determined by the GBA program and
  // is therefore replay-reproducible; time(NULL) is not, and would break
  // record/replay or netplay rollback if used here.  The seed only needs
  // to vary across resets within a session, which cpu_ticks already
  // satisfies (rfu_reset is called from a GPIO pulse driven by the game).
  rand_seed(cpu_ticks);
}

static u16 new_devid(void) {
  while (1) {
    /* Mix in cpu_ticks (deterministic) rather than time(NULL) so the
     * generated device ID is reproducible across record/replay. */
    u16 n = rand_gen() ^ (u16)cpu_ticks;
    if (n)
      return n;
  }
}

// We have received a command in full (with a potential payload), process it
// and return the return command code (plus some payload too?).
static s32 rfu_process_command(void) {
  u32 i, j, cnt = 0;
  // (FrogGBA: each command is logged with its result by rfu_log_command)

  switch (rfu_cmd) {
  // These are not 100% supported, but they are OK as long as we ACK them.
  case RFU_CMD_INIT1:
  case RFU_CMD_INIT2:
    return 0;

  case RFU_CMD_SYSCFG:
    // Contains the slave timeout and retransmit count.
    rfu_timeout = rfu_buf[0];
    rfu_rtx_max = rfu_buf[0] >> 8;
    return 0;

  case RFU_CMD_SYSVER:
    rfu_buf[0] = 0x00830117;   // Likely some sort of firmware/hw version.
    return 1;

  case RFU_CMD_SYSSTAT:
    // Lower bits contain the DEVID, along with slot bits (if any) and some status code.
    if (rfu_state == RFU_STATE_HOST)
      rfu_buf[0] = (1 << 24) | rfu_host.devid;
    else if (rfu_state == RFU_STATE_CLIENT)
      rfu_buf[0] = (5 << 24) | ((1 << rfu_client.clnum) << 16) | rfu_client.devid;
    else
      rfu_buf[0] = 0;

    return 1;

  case RFU_CMD_SLOTSTAT:
    if (rfu_state == RFU_STATE_HOST) {
      // Just a list of connected devices it seems
      u32 cnt = 0;
      rfu_buf[cnt++] = 0;
      for (i = 0; i < 4; i++)
        if (rfu_host.clients[i].devid) {
          rfu_buf[0]++;
          rfu_buf[cnt++] = rfu_host.clients[i].devid | (i << 16);
        }
      return cnt;
    }
    return 0;

  case RFU_CMD_LINKPWR:
    // TODO: Return something better (ie. latency?)
    if (rfu_state == RFU_STATE_HOST)
      rfu_buf[0] = (rfu_host.clients[0].devid ? 0x000000ff : 0) |
                   (rfu_host.clients[1].devid ? 0x0000ff00 : 0) |
                   (rfu_host.clients[2].devid ? 0x00ff0000 : 0) |
                   (rfu_host.clients[3].devid ? 0xff000000 : 0);
    else if (rfu_state == RFU_STATE_CLIENT)
      rfu_buf[0] = 0xffffffff;
    else
      rfu_buf[0] = 0;
    return 1;

  // Process broadcast read sessions.
  // TODO return errors if outside of a session.
  case RFU_CMD_BCRD_START:
    return 0;   // Return an ACK immediately.

  case RFU_CMD_BCRD_STOP:
  case RFU_CMD_BCRD_FETCH:
    // We pick up to four random broadcasting peers.
    // This is not randomly fair but whatever :D
    i = rand_gen() % MAX_RFU_PEERS;
    for (j = 0, cnt = 0; cnt < 4*7 && j < MAX_RFU_PEERS; j++) {
      u32 entry = (i + j) % MAX_RFU_PEERS;
      if (rfu_peer_bcst[entry].valid) {
        // Header is just the device ID
        rfu_buf[cnt++] = rfu_peer_bcst[entry].device_id;
        memcpy(&rfu_buf[cnt], rfu_peer_bcst[entry].data,
               sizeof(rfu_peer_bcst[entry].data));
        cnt += 6;
      }
    }
    return cnt;

  // Sets the broadcast data to use on Host mode
  case RFU_CMD_BCST_DATA:
    if (rfu_plen == 6)
      memcpy(rfu_host.bdata, rfu_buf, sizeof(rfu_host.bdata));
    return 0;

  case RFU_CMD_HOST_START:
    if (rfu_state == RFU_STATE_CLIENT) {
      RFU_DEBUG_LOG("RFU error: Cannot start host, we are a client already\n");
      return -1;   // Return error if we are connected (state client)
    }

    if (rfu_state == RFU_STATE_IDLE) {
      // Generate a new ID if we don't have one already.
      rfu_host.devid = new_devid();
      memset(rfu_host.clients, 0, sizeof(rfu_host.clients));
      rfu_state = RFU_STATE_HOST;    // Host mode active
      RFU_DEBUG_LOG("Start hosting with device ID %02x\n", rfu_host.devid);
    }
    // Start broadcasting immediately.
    rfu_host.tx_ttl = 0xff;
    return 0;

  case RFU_CMD_HOST_STOP:
    if (rfu_state == RFU_STATE_IDLE)
      return -1;  // Return error if host mode is not active

    // This just "stops" accepting new clients, however if the host has no
    // clients, it will return to idle state (I think!).
    if (rfu_state == RFU_STATE_HOST) {
      for (i = 0; i < 4; i++)
        if (rfu_host.clients[i].devid)
          return 0;     // We stay in the host mode.

      rfu_state = RFU_STATE_IDLE;
    }

    return 0;

  case RFU_CMD_HOST_ACCEPT:
    if (rfu_state == RFU_STATE_IDLE)
      return -1;  // Return error if host mode is not active

    // This is actually a "list connected devices", not actually accept().
    // Return a list of IDs and a client number (slot number).
    for (i = 0; i < 4; i++)
      if (rfu_host.clients[i].devid)
        rfu_buf[cnt++] = rfu_host.clients[i].devid | (i << 16);
    return cnt;

  case RFU_CMD_CONNECT:
    if (rfu_state == RFU_STATE_HOST)
      return -1;  // Return error if host mode is active

    // The game specified a device ID, find the host.
    {
      u16 reqid = rfu_buf[0] & 0xffff;
      for (i = 0, cnt = 0; i < MAX_RFU_PEERS; i++) {
        if (rfu_peer_bcst[i].valid &&
            rfu_peer_bcst[i].device_id == reqid) {

          // Send a request to the host to connect
          rfu_net_send_cmd(i, NET_RFU_CONNECT_REQ, reqid);
          rfu_state = RFU_STATE_CONNECTING;
          RFU_DEBUG_LOG("Requesting connection to client %d (%x)\n", i, reqid);
          return 0;
        }
      }
    }
    // If the ID cannot be found, just ACK for now, ISCONNECTED will fail.
    return 0;

  case RFU_CMD_ISCONNECTED:
    if (rfu_state == RFU_STATE_HOST)
      return -1;  // Return error if host mode is active

    if (rfu_state == RFU_STATE_CONNECTING)
      rfu_buf[0] = RFU_CONN_INPROGRESS;
    else if (rfu_state == RFU_STATE_IDLE)
      rfu_buf[0] = RFU_CONN_FAILED;
    else
      rfu_buf[0] = rfu_client.devid | (rfu_client.clnum << 16);

    return 1;

  case RFU_CMD_CONCOMPL:
    // Seems that this is also called even when no connection happened!
    if (rfu_state == RFU_STATE_HOST)
      return -1;

    // Returns ID, with slot number and can also indicate failure.
    if (rfu_state == RFU_STATE_CLIENT) {
      rfu_buf[0] = rfu_client.devid | (rfu_client.clnum << 16);
    } else {
      rfu_buf[0] = RFU_CONN_COMP_FAIL;
      rfu_state = RFU_STATE_IDLE;
    }

    return 1;

  case RFU_CMD_SEND_DATAW:
  case RFU_CMD_SEND_DATA:
    if (!rfu_plen)
      return 0;

    if (rfu_state == RFU_STATE_HOST) {
      // Read data to be sent into the TX buffer
      rfu_tx_buf.blen = rfu_buf[0] & 0x7F;
      memcpy(rfu_tx_buf.buf, &rfu_buf[1], (rfu_plen - 1)*sizeof(u32));
    }
    else if (rfu_state == RFU_STATE_CLIENT) {
      // Same as above, but the header encoding is funny
      rfu_tx_buf.blen = (rfu_buf[0] >> (8 + rfu_client.clnum * 5)) & 0x1F;
      memcpy(rfu_tx_buf.buf, &rfu_buf[1], (rfu_plen - 1)*sizeof(u32));
    }

    if (rfu_state == RFU_STATE_HOST) {
      // Host sends a package to all clients.
      RFU_TRACE("Host sending %d bytes / %d words to clients\n",
                    rfu_tx_buf.blen, rfu_plen - 1);
      if (rfu_tx_buf.blen <= 90) {
        for (i = 0; i < 4; i++)
          if (rfu_host.clients[i].devid) {
            rfu_net_send_data(rfu_host.clients[i].client_id,
              NET_RFU_HOST_SEND, rfu_tx_buf.blen, rfu_tx_buf.buf, rfu_tx_buf.blen);
            if (rfu_tx_buf.blen)   // Empty packets are not queued by the client
              rfu_host.clients[i].h_tx++;
          }
      }
    }
    else if (rfu_state == RFU_STATE_CLIENT) {
      // Schedule data to be sent
      RFU_TRACE("Client sending %d bytes / %d words to host\n",
                    rfu_tx_buf.blen, rfu_plen - 1);
      if (rfu_tx_buf.blen <= 16) {
        // Send it immediately! This is not really accurate.
        rfu_net_send_data(rfu_client.host_id, NET_RFU_CLIENT_SEND,
          (rfu_tx_buf.blen << 24) | (rfu_client.clnum << 16) | rfu_client.devid,
          rfu_tx_buf.buf, rfu_tx_buf.blen);
      }
    }
    else
      return -1;   // We are not connected nor a host
    break;

  case RFU_CMD_RTX_WAIT:
    // "Resume retransmission": the game did not get an ACK from some client
    // (only happens when we stall the host, see rfu_update). The original
    // code sent the last packet again, but the PSP link is reliable and the
    // packet is already on its way: a copy would be a duplicate for the game.
    if (rfu_state != RFU_STATE_HOST && rfu_state != RFU_STATE_CLIENT)
      return -1;
    link_ctr.rfu_rtx++;
    return 0;

  case RFU_CMD_RECV_DATA:
    if (rfu_state == RFU_STATE_HOST) {
      // Receive data from clients
      u32 cnt = 0, bufbytes = 0;
      u8 tmpbuf[16*4] = {0};
      rfu_buf[cnt++] = 0;   // Header contains byte counts as a bitfield.
      for (i = 0; i < 4; i++) {
        // Slots we just reported as "no ACK": the game ignores what it reads
        // for them (and would overwrite it later), so keep it queued.
        if (rfu_host.recv_block_mask & (1 << i))
          continue;
        if (rfu_host.clients[i].devid && rfu_host.clients[i].qcount) {
          u32 head = rfu_host.clients[i].qhead;
          u32 dlen = MIN(16, rfu_host.clients[i].pkts[head].datalen);
          RFU_TRACE("Host reads data from client buffer (%d bytes)\n", dlen);
          // Accumulate into temp buffer
          memcpy(&tmpbuf[bufbytes], &rfu_host.clients[i].pkts[head].data[0], dlen);
          bufbytes += dlen;
          // Update byte count header for this client
          rfu_buf[0] |= dlen << (8 + i * 5);
          // Discard front packet
          rfu_host.clients[i].qhead = (head + 1) & RFU_PKT_MASK;
          rfu_host.clients[i].qcount--;
        }
      }
      // Copy data into words into the RFU buffer.
      for (i = 0; i < (bufbytes + 3) / 4; i++)
        rfu_buf[cnt++] = leupack32(&tmpbuf[i*4]);
      rfu_host.recv_block_mask = 0;
      return cnt;
    }
    else if (rfu_state == RFU_STATE_CLIENT) {
      u32 cnt = 0;
      u32 head = rfu_client.qhead;
      u32 dlen = rfu_client.qcount ? rfu_client.pkts[head].hblen : 0;
      RFU_TRACE("Client reads data from host buffer (%d bytes)\n", dlen);
      rfu_buf[cnt++] = dlen;   // Header contains byte count.
      for (j = 0; j < (dlen + 3) / 4; j++)
        rfu_buf[cnt++] = leupack32(&rfu_client.pkts[head].hdata[j*4]);

      // Move to the next packet
      if (rfu_client.qcount) {
        rfu_client.qhead = (head + 1) & RFU_PKT_MASK;
        rfu_client.qcount--;
        rfu_client.consumed++;   // Reported to the host (flow control)
      }
      return cnt;
    }
    break;

  case RFU_CMD_WAIT:
    // Do nothing, return no data (just ack).
    // The handler will deal with clock reversing.
    return 0;

  case RFU_CMD_DISCONNECT:
    if (rfu_state == RFU_STATE_CLIENT) {
      // Assuming self-disconnect?
      rfu_net_send_cmd(rfu_client.host_id, NET_RFU_DISCONNECT,
                       rfu_client.devid | (rfu_client.clnum << 16));
      rfu_state = RFU_STATE_IDLE;
    } else if (rfu_state == RFU_STATE_HOST) {
      // We are disconnecing some client(s).
      for (i = 0; i < 4; i++)
        if (rfu_buf[0] & (1 << i)) {
          // Send disconnect notice, clear slot!
          rfu_net_send_cmd(rfu_host.clients[i].client_id, NET_RFU_DISCONNECT,
                   rfu_host.clients[i].devid | (i << 16));
          memset(&rfu_host.clients[i], 0, sizeof(rfu_host.clients[i]));
        }
    }
    return 0;

  default:
    RFU_DEBUG_LOG("Unknown RFU command %02x\n", rfu_cmd);
  };

  return 0;
}

// Returns true if a Wait event can finish due to new data being available.
static bool rfu_data_avail(void) {
  if (rfu_state == RFU_STATE_CLIENT) {
    if (rfu_client.qcount != 0)
      return true;
  }
  else if (rfu_state == RFU_STATE_HOST) {
    // Returns true if any data from any client is available.
    u32 i;
    for (i = 0; i < 4; i++)
      if (rfu_host.clients[i].devid && rfu_host.clients[i].qcount)
        return true;
  }
  return false;
}

// Called whenever the game flips the SIOCNT:Start bit to transmit a value.
// We simulate the reception and response of said value (over the SPI bus).
u32 rfu_transfer(u32 sent_value) {
  u32 retval = 0x80000000;

  switch (rfu_comstate) {
  case RFU_COMSTATE_RESET:
    // Start of sequence (check the low 16 bits)
    retval = 0;
    if ((sent_value & 0xFFFF) == 0x494E)
      rfu_comstate = RFU_COMSTATE_HANDSHAKE;

    break;

  case RFU_COMSTATE_HANDSHAKE:
    // Check for the last step of the sequence
    if (sent_value == 0xB0BB8001)
      rfu_comstate = RFU_COMSTATE_WAITCMD;

    retval = (sent_value << 16) | ((~rfu_prev_data) & 0xFFFF);
    break;

  case RFU_COMSTATE_WAITCMD:
    // Wait for a new command, verify its header.
    if ((sent_value >> 16) == 0x9966) {
      rfu_plen = (u8)(sent_value >> 8);
      rfu_cmd  = (u8)(sent_value);
      rfu_cnt = 0;
      if (!rfu_plen) {
        // Returns error code or response length
        s32 ret = rfu_process_command();
        rfu_log_command(rfu_cmd, 0, 0, ret);
        if (ret < 0) {
          rfu_comstate = RFU_COMSTATE_RESPERR;
          rfu_cmd = (u32)(-ret); // Err code
          rfu_plen = 1;
        }
        else {
          rfu_comstate = RFU_COMSTATE_RESPCMD;
          rfu_plen = (u32)ret;
        }
      }
      else
        rfu_comstate = RFU_COMSTATE_WAITDAT;
    }
    break;

  case RFU_COMSTATE_WAITDAT:
    rfu_buf[rfu_cnt++] = sent_value;
    if (rfu_cnt == rfu_plen) {
      u32 arg0 = rfu_buf[0];
      s32 ret = rfu_process_command();
      rfu_log_command(rfu_cmd, rfu_plen, arg0, ret);
      if (ret < 0) {
        rfu_comstate = RFU_COMSTATE_RESPERR;
        rfu_cmd = (u32)(-ret); // Err code
        rfu_plen = 1;
      }
      else {
        rfu_comstate = RFU_COMSTATE_RESPCMD;
        rfu_plen = (u32)ret;
      }
      rfu_cnt = 0;
    }
    break;

  case RFU_COMSTATE_RESPCMD:
    // Disregard the input value and respond with the ack'ed command
    retval = 0x99660080 | rfu_cmd | (rfu_plen << 8);

    // These commands are special: they do not have any response data
    // and reverse the clock roles (the RFU becomes master).
    if (rfu_cmd == RFU_CMD_WAIT || rfu_cmd == RFU_CMD_RTX_WAIT || rfu_cmd == RFU_CMD_SEND_DATAW) {
      rfu_comstate = RFU_COMSTATE_WAITEVENT;
      rfu_timeout_cycles = rfu_timeout * (16777216 / 60);  // Frames to cycles
      rfu_resp_timeout = rfu_rtx_max * (16777216 / 60 / 6);  // RFU "frame" to cycles
      rfu_wait_start_ticks = cpu_ticks;
      rfu_wait_paced = 0;
      link_ctr.rfu_waits++;
      if (rfu_state == RFU_STATE_HOST)
        rfu_host.recv_block_mask = 0;
      // A client that read packets without answering (plain WAIT) still has
      // to tell the host, or the host would stall waiting for it.
      else if (rfu_state == RFU_STATE_CLIENT && rfu_client.consumed != rfu_client.reported)
        rfu_client_report();
      RFU_TRACE("wait start (%s) timeout=%u frames rtx=%u state=%u",
                rfu_cmd_name(rfu_cmd), rfu_timeout, rfu_rtx_max, rfu_state);
    } else {
      rfu_comstate = rfu_plen ? RFU_COMSTATE_RESPDAT : RFU_COMSTATE_WAITCMD;
    }
    break;

  case RFU_COMSTATE_RESPDAT:
    // Keep pushing data to the GBA
    retval = rfu_buf[rfu_cnt++];
    if (rfu_cnt == rfu_plen)
      rfu_comstate = RFU_COMSTATE_WAITCMD;
    break;

  case RFU_COMSTATE_WAITEVENT:
  case RFU_COMSTATE_WAITRESP:
    // TODO: this should not happen? Since we are master?
    break;

  case RFU_COMSTATE_RESPERR:
    // Return the error code command. Includes one error value
    retval = 0x996601ee;
    rfu_comstate = RFU_COMSTATE_RESPERR2;
    break;

  case RFU_COMSTATE_RESPERR2:
    retval = rfu_cmd;   // Some error code, not understood yet.
    rfu_comstate = RFU_COMSTATE_WAITCMD;
    break;
  };

  rfu_prev_data = sent_value;
  return retval;
}

// Gets called every frame for basic device updates.
void rfu_frame_update(void) {
  // If device is in reset state, do nothing really.
  if (rfu_comstate != RFU_COMSTATE_RESET) {
    u32 i;
    // Account for peer expiration.
    for (i = 0; i < MAX_RFU_PEERS; i++) {
      if (rfu_peer_bcst[i].valid) {
        if (--rfu_peer_bcst[i].ttl == 0)
          rfu_peer_bcst[i].valid = 0;
      }
    }

    // Broadcast host session periodically
    if (rfu_state == RFU_STATE_HOST) {
      if (rfu_host.tx_ttl++ >= BCST_ANNOUNCE_VB) {
        rfu_host.tx_ttl = 0;
        rfu_net_send_bcast(NET_RFU_BROADCAST, rfu_host.devid, rfu_host.bdata);
      }
    }

    // FrogGBA flow control: one more packet may be handed to the client
    // game this frame, and the host learns how far behind the game is.
    if (rfu_state == RFU_STATE_CLIENT) {
      if (rfu_client.credits < RFU_CLIENT_BURST)
        rfu_client.credits++;
      if (rfu_client.consumed != rfu_client.reported ||
          ++rfu_client.report_ttl >= RFU_REPORT_KEEPALIVE)
        rfu_client_report();
    }
    else if (rfu_state == RFU_STATE_HOST) {
      for (i = 0; i < 4; i++) {
        u32 lag = rfu_host_lag(i);
        if (lag > link_ctr.rfu_lag_max)
          link_ctr.rfu_lag_max = lag;
        if (rfu_host.clients[i].throttle_off && lag <= RFU_HOST_MAX_LAG) {
          rfu_host.clients[i].throttle_off = 0;
          LLOG("RFU client slot %u caught up (lag %u): flow control back on", (unsigned)i, (unsigned)lag);
        }
      }
    }

    // Account client TTL (to timeout clients)
    if (rfu_state == RFU_STATE_HOST) {
      for (i = 0; i < 4; i++) {
        if (rfu_host.clients[i].devid) {
          if (++rfu_host.clients[i].clttl >= 240 /* 4s */) {
            // The client hasn't sent stuff for a while, disconnect.
            RFU_DEBUG_LOG("Disconnect client slot %d due to timeout\n", i);
            memset(&rfu_host.clients[i], 0, sizeof(rfu_host.clients[i]));
          }
        }
      }
    }
  }
}

// Host: queue a packet received from a client until the game reads it.
static void rfu_host_queue_client_pkt(u32 clid, const u8 *payl, u32 blen) {
  if (rfu_host.clients[clid].qcount < RFU_PKT_QUEUE) {
    u32 slot = (rfu_host.clients[clid].qhead + rfu_host.clients[clid].qcount) & RFU_PKT_MASK;
    memcpy(rfu_host.clients[clid].pkts[slot].data, payl, blen);
    rfu_host.clients[clid].pkts[slot].datalen = blen;
    rfu_host.clients[clid].qcount++;
    if (rfu_host.clients[clid].qcount > link_ctr.rfu_q_max)
      link_ctr.rfu_q_max = rfu_host.clients[clid].qcount;
    RFU_TRACE("Recv client packet (%d bytes) Q[#%d]\n", blen, rfu_host.clients[clid].qcount);
    return;
  }
  link_ctr.rfu_q_drops++;
  RFU_DEBUG_LOG("Host dropped a client packet (queue full)\n");
}

void rfu_net_receive(const void* buf, size_t len, uint16_t client_id) {
  // RFU1 header, just some minor sanity check really.
  if (len >= 12 && upack32((const u8*)buf) == NET_RFU_HEADER) {
    u32 i, j;
    const u8 *buf8 = (const u8*)buf;
    const u8 *payl = &buf8[12];
    u32 ptype = upack32(&buf8[4]);
    u32 hdata = upack32(&buf8[8]);

    switch (ptype) {
    case NET_RFU_BROADCAST:
      // Fill the broadcast slot for the peer
      RFU_DEBUG_LOG("Got broadcast (client: #%d devID: %02x)\n", client_id, hdata);
      if (client_id < MAX_RFU_PEERS) {
        rfu_peer_bcst[client_id].device_id = hdata;
        rfu_peer_bcst[client_id].valid = 1;
        rfu_peer_bcst[client_id].ttl = 0xff;
        for (j = 0; j < 6; j++)
          rfu_peer_bcst[client_id].data[j] = upack32(&payl[j*4]);
      }
      break;

    case NET_RFU_CONNECT_REQ:
      RFU_DEBUG_LOG("Received Conn Req (client ID: %d)\n", client_id);
      if (rfu_state == RFU_STATE_HOST) {
        // Ensure this client is not already connected!
        for (i = 0; i < 4; i++)
          if (rfu_host.clients[i].devid &&
              rfu_host.clients[i].client_id == client_id) {

            RFU_DEBUG_LOG("Connection request ignored: already connected!\n");
            return;
          }

        // Find an empty connection slot
        for (i = 0; i < 4; i++)
          if (!rfu_host.clients[i].devid) {
            u16 newid = new_devid();
            memset(&rfu_host.clients[i], 0, sizeof(rfu_host.clients[i]));
            rfu_host.clients[i].devid = newid;
            rfu_host.clients[i].client_id = client_id;
            RFU_DEBUG_LOG("Connected client: assigned new devID %02x\n", newid);

            // Respond with ACK
            rfu_net_send_cmd(client_id, NET_RFU_CONNECT_ACK, newid | (i << 16));
            return;
          }
        
        // Not enough slots, NACK
        rfu_net_send_cmd(client_id, NET_RFU_CONNECT_NACK, 0);
      } else {
        RFU_DEBUG_LOG("Conn Req ignored, device is not in Host mode\n");
        rfu_net_send_cmd(client_id, NET_RFU_CONNECT_NACK, 0);
      }
      break;
      
    case NET_RFU_CONNECT_ACK:
      RFU_DEBUG_LOG("Received connection ACK from client ID: %d\n", client_id);
      // Only ok if we are not connected (not hosting)
      if (rfu_state == RFU_STATE_CONNECTING) {
        // Clear state and install device ID and slot number.
        memset(&rfu_client, 0, sizeof(rfu_client));
        rfu_client.devid = hdata & 0xffff;
        rfu_client.clnum = hdata >> 16;
        rfu_client.host_id = client_id;
        rfu_state = RFU_STATE_CLIENT;
        RFU_DEBUG_LOG("Client connected with slot ID %d\n", rfu_client.clnum);
      }
      break;

    case NET_RFU_CONNECT_NACK:
      // When receiving a NACK just return to Idle state.
      if (rfu_state == RFU_STATE_CONNECTING)
        rfu_state = RFU_STATE_IDLE;
      RFU_DEBUG_LOG("Received CONN NACK\n");
      break;

    case NET_RFU_DISCONNECT:
      if (rfu_state == RFU_STATE_HOST) {
        // Clear the client from the list
        u32 clnum = (hdata >> 16) & 0x3;
        u16 cldid = hdata & 0xffff;
        if (rfu_host.clients[clnum].devid == cldid)
          memset(&rfu_host.clients[clnum], 0, sizeof(rfu_host.clients[clnum]));
      }
      else if (rfu_state == RFU_STATE_CLIENT) {
        // Go back to idle state!
        memset(&rfu_client, 0, sizeof(rfu_client));
        rfu_state = RFU_STATE_IDLE;
      }
      break;

    case NET_RFU_HOST_SEND:
      // Only possible if we are a client
      if (rfu_state == RFU_STATE_CLIENT) {
        u32 blen = hdata & 0x7f;
        if (len >= blen + 12) {
          u32 i;
          // FrogGBA: no per-packet ACK any more (it doubled the traffic);
          // the client reports what the game has read instead (see
          // rfu_client_report), which also keeps the host's TTL alive.
          // Receive data from the host. Queue que packet if possible
          (void)i;
          link_ctr.rfu_pkt_rx++;
          if (blen == 0)
            return;
          if (rfu_client.qcount < RFU_PKT_QUEUE) {
            u32 slot = (rfu_client.qhead + rfu_client.qcount) & RFU_PKT_MASK;
            memcpy(&rfu_client.pkts[slot].hdata, payl, blen);
            rfu_client.pkts[slot].hblen = blen;
            rfu_client.qcount++;
            if (rfu_client.qcount > link_ctr.rfu_q_max)
              link_ctr.rfu_q_max = rfu_client.qcount;
            RFU_TRACE("Recv host packet (%d bytes) Q[#%d]\n", blen, rfu_client.qcount);
            return;
          }
          link_ctr.rfu_q_drops++;
          rfu_client.consumed++;   // Gone for good: don't let the host wait for it
          RFU_DEBUG_LOG("Client dropped a host packet (queue full)\n");
        }
      }
      break;

    case NET_RFU_CLIENT_SEND:
      // Only available when we are hosting
      if (rfu_state == RFU_STATE_HOST) {
        u32 i;
        u16 cdevid = hdata & 0xffff;
        u32 clid = (hdata >> 16) & 0x3;
        u32 blen = hdata >> 24;

        // Validate the slot with device ID
        if (rfu_host.clients[clid].devid == cdevid) {
          rfu_host.clients[clid].clttl = 0;   // Account for packet reception
          (void)i;
          link_ctr.rfu_pkt_rx++;
          // FrogGBA: the packet also says how many host packets the client
          // game has read (it was sent after the data it answers).
          if (len >= 12 + RFU_REPORT_OFFSET + 8 &&
              upack32(&payl[RFU_REPORT_OFFSET]) == RFU_REPORT_MAGIC) {
            if (blen > 0 && blen <= 16)
              rfu_host_queue_client_pkt(clid, payl, blen);
            rfu_host_note_consumed(clid, upack32(&payl[RFU_REPORT_OFFSET + 4]));
            return;
          }
          if (blen == 0 || blen > 16)
            return;
          rfu_host_queue_client_pkt(clid, payl, blen);
        }
      }
      break;

    case NET_RFU_CLIENT_ACK:
      // Should only happen when hosting
      if (rfu_state == RFU_STATE_HOST) {
        u32 devid = hdata & 0xffff;
        u32 clid = (hdata >> 16) & 0x3;

        if (rfu_host.clients[clid].devid == devid) {
          rfu_host.clients[clid].clttl = 0;   // Account for packet reception
          if (len >= 16)   // FrogGBA: host packets read by the client game
            rfu_host_note_consumed(clid, upack32(payl));
        }
      }
    };
  } else {
    RFU_DEBUG_LOG("Drop malformed packet\n");
  }
}

// Host: the stall (if any) is over.
static void rfu_host_stall_end(void) {
  if (rfu_host.stall_run >= 10)
    LLOG("RFU host waited %u frames for the client game to catch up",
         (unsigned)rfu_host.stall_run);
  rfu_host.stall_run = 0;
}

// Host: slots whose client game is too far behind (flow control).
static u32 rfu_host_lag_mask(void) {
  u32 i, mask = 0, maxlag = 0;
  for (i = 0; i < 4; i++) {
    u32 lag = rfu_host_lag(i);
    if (lag > RFU_HOST_MAX_LAG && !rfu_host.clients[i].throttle_off) {
      mask |= 1 << i;
      if (lag > maxlag)
        maxlag = lag;
    }
  }
  if (mask && rfu_host.stall_run >= RFU_HOST_MAX_STALL) {
    // Something is wrong (a client that reads nothing?): never freeze the
    // host game for good. Back to the original behaviour until it catches up.
    LLOG("RFU host: client slots %X still %u packets behind after %u stalls: flow control off for them",
         (unsigned)mask, (unsigned)maxlag, (unsigned)rfu_host.stall_run);
    for (i = 0; i < 4; i++)
      if (mask & (1 << i))
        rfu_host.clients[i].throttle_off = 1;
    rfu_host.stall_run = 0;
    mask = 0;
  }
  return mask;
}

// Account for consumed cycles and return if a serial IRQ should be raised.
bool rfu_update(unsigned cycles) {
  if (rfu_comstate == RFU_COMSTATE_WAITEVENT) {
    /* Force receive packets so that we can perhaps abort the wait.
     * This helps minimize latency (otherwise we need to wait a full
     * frame!).
     *
     * REENTRANCY: netpacket_poll_receive synchronously dispatches to
     * the frontend's netplay code, which may then call back into our
     * netpacket_receive -> rfu_net_receive on the same thread.
     * rfu_net_receive freely mutates rfu_state, rfu_host, rfu_client,
     * rfu_peer_bcst[] and other rfu_* globals.  All reads below must
     * re-fetch these globals after this point - do NOT cache rfu_state
     * etc. into a local across this call. */
    netpacket_poll_receive();

    // Check if we are running our of time to respond.
    rfu_timeout_cycles -= MIN(cycles, rfu_timeout_cycles);
    rfu_resp_timeout   -= MIN(cycles, rfu_resp_timeout);

    // Wait for GBA to go into slave mode before finishing the wait!
    if ((read_ioreg(REG_SIOCNT) & 0x1) == 0) {
      if (rfu_state == RFU_STATE_IDLE) {
        // We are disconnected (most likely)
        rfu_buf[0] = 0x99660000 | (1 << 8) | RFU_CMD_RESP_DISC;
        rfu_buf[1] = 0xF;  // Reason disconnect (0), all slots disconnected?
        rfu_buf[2] = 0x80000000;
        rfu_cnt = 0;
        rfu_plen = 3;
        rfu_comstate = RFU_COMSTATE_WAITRESP;
        link_ctr.rfu_ev_disc++;
        LLOG("RFU wait end: DISCONNECTED after %u cycles", (unsigned)(cpu_ticks - rfu_wait_start_ticks));
      }
      else if (rfu_data_avail() &&
               (rfu_state != RFU_STATE_CLIENT || rfu_client.credits)) {
        // Some event is available!
        if (rfu_state == RFU_STATE_CLIENT)
          rfu_client.credits--;     // Pacing: see RFU_CLIENT_BURST
        else
          rfu_host_stall_end();
        rfu_buf[0] = 0x99660000 | RFU_CMD_RESP_DATA;
        rfu_buf[1] = 0x80000000;
        rfu_cnt = 0;
        rfu_plen = 2;
        rfu_comstate = RFU_COMSTATE_WAITRESP;
        link_ctr.rfu_ev_data++;
        RFU_TRACE("wait end: DATA after %u cycles", (unsigned)(cpu_ticks - rfu_wait_start_ticks));
      }
      else if (rfu_state == RFU_STATE_HOST && !rfu_resp_timeout) {
        // We "retransmitted" the message N times (not really, but the equivalent
        // time has elapsed) and we simulate a lack of client response.
        // The low byte of the parameter is the set of slots that ACKed the
        // host data (librfu: rfu_UNI_PARENT_getDRAC_ACK).
        u32 lagmask = rfu_host_lag_mask();
        rfu_buf[0] = 0x99660000 | RFU_CMD_RESP_DATA | (1 << 8);
        if (lagmask) {
          // FrogGBA flow control: the client game is too far behind. Report
          // "no ACK" for it: the game skips this frame and retransmits
          // (RTX_WAIT) instead of sending one more packet.
          rfu_buf[1] = 0x00000F00 | (0x0F & ~lagmask);
          rfu_host.recv_block_mask = lagmask;
          rfu_host.stall_run++;
          link_ctr.rfu_ev_stall++;
        } else {
          rfu_host_stall_end();
          rfu_buf[1] = 0x00000F0F;  // TODO: just using 4 slots for now
          link_ctr.rfu_ev_noresp++;
        }
        rfu_buf[2] = 0x80000000;
        rfu_cnt = 0;
        rfu_plen = 3;
        rfu_comstate = RFU_COMSTATE_WAITRESP;
        RFU_TRACE("wait end: NO CLIENT ANSWER after %u cycles", (unsigned)(cpu_ticks - rfu_wait_start_ticks));
      }
      else if (rfu_state == RFU_STATE_CLIENT && rfu_client.qcount && !rfu_wait_paced) {
        rfu_wait_paced = 1;           // Data held back until the next frame
        link_ctr.rfu_paced++;
      }
      else if (!rfu_timeout_cycles) {
        // We ran out of time, just return an "error" code
        rfu_buf[0] = 0x99660000 | RFU_CMD_RESP_TIMEO;
        rfu_buf[1] = 0x80000000;
        rfu_cnt = 0;
        rfu_plen = 2;
        rfu_comstate = RFU_COMSTATE_WAITRESP;
        link_ctr.rfu_ev_timeout++;
        LLOG("RFU wait end: TIMEOUT after %u cycles", (unsigned)(cpu_ticks - rfu_wait_start_ticks));
      }
    }
  }

  if (rfu_comstate == RFU_COMSTATE_WAITRESP) {
    // Pushes command and data as an RFU-master back to the GBA.
    // Only if SO/SI are low and the device is active to receive stuff
    if ((read_ioreg(REG_SIOCNT) & 0xC) == 0 && (read_ioreg(REG_SIOCNT) & 0x80)) {
      link_ctr.rfu_words_out++;
      rfu_push_blocked_since = 0;
      rfu_push_blocked_logged = 0;
      // Write data into the "received" register and clear active bit.
      write_ioreg(REG_SIODATA32_H, rfu_buf[rfu_cnt] >> 16);
      write_ioreg(REG_SIODATA32_L, rfu_buf[rfu_cnt] & 0xffff);
      write_ioreg(REG_SIOCNT, (read_ioreg(REG_SIOCNT) & ~0x80));
      rfu_cnt++;
      // Go back to slave mode
      if (rfu_cnt == rfu_plen) {
        rfu_comstate = RFU_COMSTATE_WAITCMD;
        RFU_TRACE("event %08X delivered (%u words)", rfu_buf[0], rfu_plen);
      }
      return read_ioreg(REG_SIOCNT) & 0x4000;
    }
    else {
      // The GBA is not ready yet (SO/SI busy or no START bit).
      link_ctr.rfu_push_block++;
      if (!rfu_push_blocked_since)
        rfu_push_blocked_since = cpu_ticks ? cpu_ticks : 1;
      else if (!rfu_push_blocked_logged &&
               (u32)(cpu_ticks - rfu_push_blocked_since) > 16777216 / 60) {
        rfu_push_blocked_logged = 1;
        LLOG("RFU push of word %u/%u blocked for >1 frame: SIOCNT=%04X (need SO=0 SI=0 START=1)",
             rfu_cnt, rfu_plen, read_ioreg(REG_SIOCNT));
      }
    }
  }

  return false;
}


// FrogGBA diagnostics: state snapshot for the per-second log summary.
void rfu_debug_info(u32 *comstate, u32 *state, u32 *clients, u32 *bcst) {
  u32 i;
  *comstate = rfu_comstate;
  *state = rfu_state;
  *clients = 0;
  *bcst = 0;
  for (i = 0; i < 4; i++)
    if (rfu_host.clients[i].devid)
      (*clients)++;
  for (i = 0; i < MAX_RFU_PEERS; i++)
    if (rfu_peer_bcst[i].valid)
      (*bcst)++;
}
