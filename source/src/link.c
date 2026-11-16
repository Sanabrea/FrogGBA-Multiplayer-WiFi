/* FrogGBA - Link over ad-hoc
 *
 * Transport for the GBA link port emulation between two PSPs.
 *
 *  - Uses the PSP ad-hoc WiFi (sceNetAdhocctl + PDP datagrams).
 *  - Both PSPs join the same ad-hoc group. The one that chose "Host"
 *    announces itself, the "Guest" answers, and the host confirms. The host
 *    also decides which serial protocol both sides will emulate.
 *  - Game data travels over a small reliable ordered channel (link_rel.c),
 *    because the Pokemon link protocol cannot survive lost packets.
 *  - The serial protocols are emulated by serial.c / serial_proto.c / rfu.c
 *    (ported from gpSP for libretro). They see the host as client 0 and the
 *    guest as client 1, exactly like RetroArch netplay does.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 2 of
 * the License, or (at your option) any later version.
 */

#include "common.h"

#include <pspnet.h>
#include <pspnet_adhoc.h>
#include <pspnet_adhocctl.h>
#include <psputility.h>
#include <pspwlan.h>

#include "link_rel.h"

// ---------------------------------------------------------------------------
// Configuration

#define LINK_GROUP_NAME        "FROGLNK1"   // Ad-hoc group (max 8 chars)
#define LINK_PRODUCT_CODE      "FROG00001"  // Ad-hoc product id (9 chars)
#define LINK_PDP_PORT          0x4C4B       // "LK"
#define LINK_PDP_BUFSIZE       0x2000

#define LINK_MAGIC             0x46474C31   // "FGL1"
#define LINK_PROTO_VERSION     3            // 2: records, adaptive RTO; 3: RFU flow control

#define LINK_HDR_SIZE          20
// Payload of a data datagram: several messages packed as records.
#define LINK_DGRAM_PAYLOAD     1024
#define LINK_MAX_DGRAM         (LINK_HDR_SIZE + LINK_DGRAM_PAYLOAD)

// Data datagrams per network service call (once per frame). The PSP
// ad-hoc link only sustains a few hundred small packets per second.
#define LINK_MAX_DGRAMS_TICK   2

#define LINK_KEEPALIVE_US      250000       // Ping when idle
#define LINK_PEER_TIMEOUT_US   6000000      // Peer considered lost
#define LINK_ADHOC_TIMEOUT_US  20000000     // Joining the ad-hoc group
#define LINK_PAIR_TIMEOUT_US   90000000     // Waiting for the other PSP
#define LINK_PAIR_RESEND_US    200000       // Pairing message period

// Poll the network at most this often while the CPU is emulating
// (expressed in GBA cycles, ~1ms).
#define LINK_POLL_MIN_CYCLES   16384

// Priority of the network stack threads (lower number = higher priority).
// The emulator thread runs at 32 and has no idle time when the game spins
// on the serial port (Wireless Adapter), so the network threads must be
// above it or received packets pile up and arrive in bursts.
#define LINK_NET_THREAD_PRIO   0x18
#define LINK_NET_THREAD_PRIO_FALLBACK 42

// Sleep this long each frame (any link mode) so that the lower priority
// helper threads of the PSP network stack also get to run. Without it, cable
// mode showed ~4x the round trip time of wireless mode (p50 220 ms vs 53 ms,
// peaks of 1.5 s) whenever the emulator had no idle time.
#define LINK_YIELD_US          100

// sceUtilityLoadNetModule() when the module is already loaded
#define LINK_ERR_ALREADY_LOADED  ((int)0x80111102)

// Peer list entry as returned by sceNetAdhocctlGetPeerList (152 bytes on
// real hardware; the PSPSDK struct is 4 bytes short).
typedef struct
{
  u32 next;
  char nickname[128];
  unsigned char mac[6];
  u16 padding;
  u32 flags;
  u32 last_recv_lo;
  u32 last_recv_hi;
} __attribute__((packed)) link_peer_info;

// Datagram types
#define PKT_HELLO      1   // Host -> broadcast: "I'm hosting"
#define PKT_JOIN       2   // Guest -> host: "I want to join"
#define PKT_WELCOME    3   // Host -> guest: "You're in" (carries final mode)
#define PKT_DATA       4   // Reliable payload (serial protocol message)
#define PKT_ACK        5   // Pure acknowledgement
#define PKT_PING       6   // Keepalive
#define PKT_BYE        7   // Session closed

// Pairing payload: game code + title, so each side can show what the
// other one is running.
#define PAIR_PAYLOAD_SIZE      16

// ---------------------------------------------------------------------------
// State

typedef enum
{
  LINK_STATE_IDLE = 0,       // No session
  LINK_STATE_CONNECTED,      // Session open, peer alive
  LINK_STATE_LOST            // Session open, peer silent for a while
} LINK_STATE_TYPE;

u32 option_link_mode = LINK_MODE_AUTO;
u32 link_request_menu_exit = 0;

u32 netplay_client_id = 0;
u32 netplay_num_clients = 0;

static LINK_STATE_TYPE link_state = LINK_STATE_IDLE;

// Network stack bring-up flags
static int net_modules_loaded = 0;
static int net_inited = 0;
static int adhoc_inited = 0;
static int adhocctl_inited = 0;
static int adhocctl_connected = 0;
static int pdp_id = -1;

static unsigned char local_mac[6];
static unsigned char peer_mac[6];
static const unsigned char broadcast_mac[6] =
  { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };

static int link_is_host = 0;
static u16 link_session = 0;
static u8 link_peer_mode = 0;        // Serial mode reported by the peer

static rel_chan_t link_rel;
static u32 last_rx_time = 0;
static u32 last_tx_time = 0;
static u32 last_poll_ticks = 0;
static u32 dgram_tx_count = 0;
static u32 dgram_rx_count = 0;
static u32 send_errors = 0;

// Game currently loaded (for Auto mode and the pairing info)
static char local_game_code[5] = "";
static char local_game_title[13] = "";
static char peer_game_code[5] = "";
static char peer_game_title[13] = "";

static char status_line1[64] = "Not connected";
static char status_line2[64] = "";

static u8 rx_buf[LINK_MAX_DGRAM + 16] ALIGN_DATA;
static u8 tx_buf[LINK_MAX_DGRAM + 16] ALIGN_DATA;
static u8 pay_buf[LINK_DGRAM_PAYLOAD + 16] ALIGN_DATA;

static void link_teardown(int send_bye);

// ---------------------------------------------------------------------------
// Helpers

static u32 now_us(void)
{
  return sceKernelGetSystemTimeLow();
}

static void put16(u8 *p, u16 v) { p[0] = v & 0xFF; p[1] = v >> 8; }
static void put32(u8 *p, u32 v) { put16(p, v & 0xFFFF); put16(p + 2, v >> 16); }
static u16 get16(const u8 *p)   { return p[0] | (p[1] << 8); }
static u32 get32(const u8 *p)   { return get16(p) | (get16(p + 2) << 16); }

static const char *serial_mode_name(int mode)
{
  switch (mode)
  {
    case SERIAL_MODE_RFU:          return "Wireless Adapter (beta)";
    case SERIAL_MODE_SERIAL_POKE:  return "Cable (Pokemon)";
    case SERIAL_MODE_SERIAL_AW1:   return "Cable (Adv. Wars 1)";
    case SERIAL_MODE_SERIAL_AW2:   return "Cable (Adv. Wars 2)";
  }
  return "None";
}

// ---------------------------------------------------------------------------
// Serial mode selection

typedef struct
{
  const char *code;   // Game code prefix (3 chars matches every region)
  u8 mode;
} link_game_entry;

// Defaults taken from gpSP for libretro (gba_over.h / auto detection).
static const link_game_entry link_game_table[] =
{
  // Pokemon Gen3: the link cable mode (Cable Club trades and battles). It
  // works for every Gen3 game and between them, and has been verified on
  // real PSPs. Emerald / FireRed / LeafGreen can also use the Wireless
  // Adapter (Union Room), selectable by hand while it is in beta.
  { "AXV", SERIAL_MODE_SERIAL_POKE },
  { "AXP", SERIAL_MODE_SERIAL_POKE },
  { "BPE", SERIAL_MODE_SERIAL_POKE },
  { "BPR", SERIAL_MODE_SERIAL_POKE },
  { "BPG", SERIAL_MODE_SERIAL_POKE },
  // Advance Wars
  { "AWR", SERIAL_MODE_SERIAL_AW1 },
  { "AW2", SERIAL_MODE_SERIAL_AW2 },
  // Wireless Adapter games
  { "BKRJ", SERIAL_MODE_RFU },  // No No No Puzzle Chailien
  { "BDG",  SERIAL_MODE_RFU },  // Digimon Racing
  { "BG3E", SERIAL_MODE_RFU },  // Dragon Ball Z - Buu's Fury
  { "BMG",  SERIAL_MODE_RFU },  // Mario Golf: Advance Tour
  { "BTM",  SERIAL_MODE_RFU },  // Mario Tennis: Power Tour
  { "BRBE", SERIAL_MODE_RFU },  // Mega Man Battle Network 5 Team Protoman
  { "BRKE", SERIAL_MODE_RFU },  // Mega Man Battle Network 5 Team Colonel
  { "BR5E", SERIAL_MODE_RFU },  // Mega Man Battle Network 6 Cybeast Gregar
  { "BR6E", SERIAL_MODE_RFU },  // Mega Man Battle Network 6 Cybeast Falzar
  { "B2WE", SERIAL_MODE_RFU },  // Narnia Chronicles
  { "B4U",  SERIAL_MODE_RFU },  // Shrek Super Slam
  { "B3AE", SERIAL_MODE_RFU },  // Lord of the Rings: the third Age
  { "B85",  SERIAL_MODE_RFU },  // Hamtaro Ham Ham Games
};

static int link_auto_mode(void)
{
  u32 i;

  for (i = 0; i < sizeof(link_game_table) / sizeof(link_game_table[0]); i++)
  {
    const char *code = link_game_table[i].code;
    if (strncmp(local_game_code, code, strlen(code)) == 0)
      return link_game_table[i].mode;
  }

  // Pokemon ROM hacks usually keep the "POKEMON" title: use the cable mode,
  // which does not depend on the Wireless Adapter firmware quirks.
  if (strncmp(local_game_title, "POKEMON", 7) == 0)
    return SERIAL_MODE_SERIAL_POKE;

  return SERIAL_MODE_RFU;
}

static int link_local_mode(void)
{
  switch (option_link_mode)
  {
    case LINK_MODE_RFU:  return SERIAL_MODE_RFU;
    case LINK_MODE_POKE: return SERIAL_MODE_SERIAL_POKE;
    case LINK_MODE_AW1:  return SERIAL_MODE_SERIAL_AW1;
    case LINK_MODE_AW2:  return SERIAL_MODE_SERIAL_AW2;
  }
  return link_auto_mode();
}

static int link_local_mode_flexible(void)
{
  return option_link_mode == LINK_MODE_AUTO;
}

// The host decides the protocol for the session.
static int link_negotiate_mode(int host_mode, int host_flex,
                               int guest_mode, int guest_flex)
{
  if (host_mode == guest_mode)
    return host_mode;

  // A manual choice wins over an automatic one.
  if (host_flex && !guest_flex)
    return guest_mode;
  if (guest_flex && !host_flex)
    return host_mode;

  // Both automatic (e.g. FireRed + Ruby): the cable works for every Gen3
  // Pokemon game, the Wireless Adapter does not.
  if (host_mode == SERIAL_MODE_SERIAL_POKE || guest_mode == SERIAL_MODE_SERIAL_POKE)
    return SERIAL_MODE_SERIAL_POKE;

  return host_mode;
}

void link_game_loaded(const char *game_code, const char *game_title)
{
  strncpy(local_game_code, game_code, 4);
  local_game_code[4] = 0;
  strncpy(local_game_title, game_title, 12);
  local_game_title[12] = 0;
}

void link_reset_serial(void)
{
  if (serial_mode != SERIAL_MODE_DISABLED)
    serial_reset();
}

// ---------------------------------------------------------------------------
// Network stack

static int link_net_init(char *err, size_t errlen)
{
  int ret;

  if (!net_modules_loaded)
  {
    ret = sceUtilityLoadNetModule(PSP_NET_MODULE_COMMON);
    if (ret < 0 && ret != LINK_ERR_ALREADY_LOADED)   // already loaded is fine
    {
      snprintf(err, errlen, "Net module load failed (%08X)", ret);
      return -1;
    }

    ret = sceUtilityLoadNetModule(PSP_NET_MODULE_ADHOC);
    if (ret < 0 && ret != LINK_ERR_ALREADY_LOADED)
    {
      sceUtilityUnloadNetModule(PSP_NET_MODULE_COMMON);
      snprintf(err, errlen, "Ad-hoc module load failed (%08X)", ret);
      return -1;
    }

    net_modules_loaded = 1;
  }

  if (!net_inited)
  {
    ret = sceNetInit(128 * 1024, LINK_NET_THREAD_PRIO, 4 * 1024,
                     LINK_NET_THREAD_PRIO, 4 * 1024);
    if (ret < 0)
      ret = sceNetInit(128 * 1024, LINK_NET_THREAD_PRIO_FALLBACK, 4 * 1024,
                       LINK_NET_THREAD_PRIO_FALLBACK, 4 * 1024);
    if (ret < 0)
    {
      snprintf(err, errlen, "sceNetInit failed (%08X)", ret);
      return -1;
    }
    net_inited = 1;
  }

  if (!adhoc_inited)
  {
    ret = sceNetAdhocInit();
    if (ret < 0)
    {
      snprintf(err, errlen, "sceNetAdhocInit failed (%08X)", ret);
      return -1;
    }
    adhoc_inited = 1;
  }

  if (!adhocctl_inited)
  {
    struct productStruct product;

    memset(&product, 0, sizeof(product));
    product.unknown = 0;
    memcpy(product.product, LINK_PRODUCT_CODE, 9);

    ret = sceNetAdhocctlInit(32 * 1024, LINK_NET_THREAD_PRIO, &product);
    if (ret < 0)
      ret = sceNetAdhocctlInit(32 * 1024, 0x20, &product);
    if (ret < 0)
    {
      snprintf(err, errlen, "sceNetAdhocctlInit failed (%08X)", ret);
      return -1;
    }
    adhocctl_inited = 1;
  }

  return 0;
}

static void link_net_term(void)
{
  if (adhocctl_inited)
  {
    sceNetAdhocctlTerm();
    adhocctl_inited = 0;
  }

  if (adhoc_inited)
  {
    sceNetAdhocTerm();
    adhoc_inited = 0;
  }

  if (net_inited)
  {
    sceNetTerm();
    net_inited = 0;
  }

  if (net_modules_loaded)
  {
    sceUtilityUnloadNetModule(PSP_NET_MODULE_ADHOC);
    sceUtilityUnloadNetModule(PSP_NET_MODULE_COMMON);
    net_modules_loaded = 0;
  }
}

static int adhocctl_state(void)
{
  int state = 0;

  if (sceNetAdhocctlGetState(&state) < 0)
    return -1;

  return state;
}

static void adhoc_leave_group(void)
{
  if (adhocctl_connected)
  {
    u32 start = now_us();

    sceNetAdhocctlDisconnect();

    // Wait (briefly) until the group is really left.
    while ((adhocctl_state() > 0) && ((u32)(now_us() - start) < 2000000))
      sceKernelDelayThread(20 * 1000);

    adhocctl_connected = 0;
  }
}

// ---------------------------------------------------------------------------
// Datagrams

static void build_header(u8 *p, u8 type, u16 seq, u16 len)
{
  put32(p + 0, LINK_MAGIC);
  p[4] = type;
  p[5] = LINK_PROTO_VERSION;
  put16(p + 6, link_session);
  put16(p + 8, seq);
  put16(p + 10, link_rel.expected_seq);   // Cumulative ACK, always present
  put16(p + 12, len);
  p[14] = (u8)serial_mode;
  p[15] = link_is_host ? 0 : 1;
  put32(p + 16, rel_sack_mask(&link_rel));  // Selective ACK
}

static void send_dgram(const unsigned char *dest, u8 type, u16 seq,
                       const void *payload, u16 len)
{
  if (pdp_id < 0)
    return;

  build_header(tx_buf, type, seq, len);

  if (len)
    memcpy(tx_buf + LINK_HDR_SIZE, payload, len);

  if (sceNetAdhocPdpSend(pdp_id, (unsigned char *)dest, LINK_PDP_PORT, tx_buf,
                         LINK_HDR_SIZE + len, 0, 1) < 0)
    send_errors++;
  dgram_tx_count++;

  last_tx_time = now_us();

  // Every datagram carries the ACK.
  link_rel.ack_pending = 0;
}

// Packs queued messages (re-sends first, then new ones) into up to
// max_dgrams datagrams. Returns how many datagrams were sent.
static int link_send_data(int new_only, int max_dgrams)
{
  int sent = 0;

  while (sent < max_dgrams)
  {
    u32 n = rel_collect(&link_rel, now_us(), pay_buf, LINK_DGRAM_PAYLOAD, new_only);
    if (!n)
      break;

    send_dgram(peer_mac, PKT_DATA, 0, pay_buf, (u16)n);
    sent++;
  }

  return sent;
}

// Wireless Adapter mode is latency sensitive: send each message right away.
// The cable modes tolerate latency, so their messages are packed and sent
// once per frame (fewer packets on the air).
static int link_send_immediately(void)
{
  return serial_mode == SERIAL_MODE_RFU;
}

static void build_pair_payload(u8 *p, u8 mode, u8 flexible)
{
  memset(p, 0, PAIR_PAYLOAD_SIZE);
  memcpy(p, local_game_code, 4);
  memcpy(p + 4, local_game_title, 10);
  p[14] = mode;
  p[15] = flexible;
}

static void parse_pair_payload(const u8 *p, u16 len)
{
  if (len < PAIR_PAYLOAD_SIZE)
    return;

  memcpy(peer_game_code, p, 4);
  peer_game_code[4] = 0;
  memcpy(peer_game_title, p + 4, 10);
  peer_game_title[10] = 0;
}

// Receives one datagram. Returns its type (and fills the out params), 0 if
// there is nothing to read or the datagram is not ours.
static int recv_dgram(unsigned char *src_mac, u16 *session, u16 *seq,
                      u16 *ack, u32 *sack, u8 **payload, u16 *plen)
{
  unsigned short port = 0;
  int len = sizeof(rx_buf);
  int ret;

  if (pdp_id < 0)
    return -1;

  ret = sceNetAdhocPdpRecv(pdp_id, src_mac, &port, rx_buf, &len, 0, 1);
  if (ret < 0)
    return -1;    // Would block (empty) or error

  dgram_rx_count++;

  if ((len < LINK_HDR_SIZE) || (get32(rx_buf) != LINK_MAGIC) ||
      (rx_buf[5] != LINK_PROTO_VERSION))
    return 0;

  // Ignore our own broadcasts, should they ever loop back.
  if (memcmp(src_mac, local_mac, 6) == 0)
    return 0;

  *session = get16(rx_buf + 6);
  *seq     = get16(rx_buf + 8);
  *ack     = get16(rx_buf + 10);
  *plen    = get16(rx_buf + 12);
  *sack    = get32(rx_buf + 16);
  *payload = rx_buf + LINK_HDR_SIZE;

  if (*plen > (u16)(len - LINK_HDR_SIZE))
    return 0;

  link_peer_mode = rx_buf[14];

  return rx_buf[4];
}

// ---------------------------------------------------------------------------
// Session traffic (called while connected)

static void link_deliver(void *ctx, const uint8_t *data, uint16_t len)
{
  (void)ctx;
  link_ctr.net_msg_rx++;
  serial_net_receive(data, len, link_is_host ? 1 : 0);
}

static void link_process_incoming(void)
{
  unsigned char src[6];
  u16 session, seq, ack, plen;
  u32 sack;
  u8 *payload;
  int type;
  int guard = 64;   // Do not starve the emulator if flooded

  while (guard-- > 0)
  {
    type = recv_dgram(src, &session, &seq, &ack, &sack, &payload, &plen);

    if (type < 0)
      break;

    if (type == 0 || session != link_session || memcmp(src, peer_mac, 6) != 0)
      continue;

    last_rx_time = now_us();

    if (link_state == LINK_STATE_LOST)
    {
      link_state = LINK_STATE_CONNECTED;
      LLOG("LINK peer is answering again");
    }

    rel_on_ack(&link_rel, ack, sack, now_us());

    switch (type)
    {
      case PKT_DATA:
        if (rel_on_records(&link_rel, payload, plen, link_deliver, NULL) < 0)
          LLOG("LINK malformed data datagram (%u bytes)", (unsigned)plen);
        if (link_state == LINK_STATE_IDLE)
          return;
        break;

      case PKT_JOIN:
        // The guest did not get our WELCOME yet: repeat it.
        if (link_is_host)
        {
          u8 pl[PAIR_PAYLOAD_SIZE];
          build_pair_payload(pl, (u8)serial_mode, 0);
          send_dgram(peer_mac, PKT_WELCOME, 0, pl, PAIR_PAYLOAD_SIZE);
        }
        break;

      case PKT_BYE:
        LLOG("LINK the other PSP closed the session (BYE)");
        link_teardown(0);
        snprintf(status_line1, sizeof(status_line1), "The other PSP disconnected");
        status_line2[0] = 0;
        return;

      default:
        // PING / ACK / WELCOME / HELLO: the ACK was already processed.
        break;
    }
  }
}

static void link_service_outgoing(void)
{
  u32 now = now_us();

  // Data (re-sends and queued new messages). Data datagrams carry the ACK.
  if (link_send_data(0, LINK_MAX_DGRAMS_TICK))
    return;

  if (link_rel.ack_pending || ((u32)(now - last_tx_time) >= LINK_KEEPALIVE_US))
    send_dgram(peer_mac, link_rel.ack_pending ? PKT_ACK : PKT_PING, 0, NULL, 0);
}

static void link_check_timeout(void)
{
  if ((link_state == LINK_STATE_CONNECTED) &&
      ((u32)(now_us() - last_rx_time) >= LINK_PEER_TIMEOUT_US))
  {
    link_state = LINK_STATE_LOST;
    LLOG("LINK no packets from the other PSP for %u ms", LINK_PEER_TIMEOUT_US / 1000);
  }
}

void netpacket_send(uint16_t client_id, const void *buf, size_t len)
{
  u16 peer_id = link_is_host ? 1 : 0;

  if (link_state == LINK_STATE_IDLE)
    return;

  if ((client_id != LINK_BROADCAST_ID) && (client_id != peer_id))
    return;

  link_ctr.net_msg_tx++;

  if (rel_push(&link_rel, buf, (u16)len) == 0)
  {
    if (link_send_immediately())
      link_send_data(1, 1);
  }
  else
    LLOG("LINK send window full: message of %u bytes dropped (%u unacked)",
         (unsigned)len, (unsigned)rel_unacked(&link_rel));
}

void netpacket_poll_receive(void)
{
  if (link_state == LINK_STATE_IDLE)
    return;

  // This is called from the emulation loop (RFU waits), keep it cheap.
  if ((u32)(cpu_ticks - last_poll_ticks) < LINK_POLL_MIN_CYCLES)
    return;

  last_poll_ticks = cpu_ticks;
  link_process_incoming();
}

void link_frame_update(void)
{
  if (link_state == LINK_STATE_IDLE)
    return;

  last_poll_ticks = cpu_ticks;
  link_process_incoming();

  if (link_state == LINK_STATE_IDLE)   // BYE received
    return;

  link_service_outgoing();
  link_check_timeout();

  serial_frame_update();
  link_log_frame();

  sceKernelDelayThread(LINK_YIELD_US);
}

void link_menu_tick(void)
{
  if (link_state == LINK_STATE_IDLE)
    return;

  // Keep the session alive: answer the peer and resend pending data, but
  // do not advance the emulated serial devices (the game is paused).
  link_process_incoming();

  if (link_state == LINK_STATE_IDLE)
    return;

  link_service_outgoing();
  link_check_timeout();
}

int link_is_active(void)
{
  return link_state != LINK_STATE_IDLE;
}

const char *link_status_line1(void)
{
  switch (link_state)
  {
    case LINK_STATE_CONNECTED:
      snprintf(status_line1, sizeof(status_line1), "Connected as %s - %s",
               link_is_host ? "HOST" : "GUEST", serial_mode_name(serial_mode));
      break;

    case LINK_STATE_LOST:
      snprintf(status_line1, sizeof(status_line1), "No answer from the other PSP...");
      break;

    default:
      break;
  }

  return status_line1;
}

const char *link_status_line2(void)
{
  if (link_state != LINK_STATE_IDLE)
  {
    snprintf(status_line2, sizeof(status_line2), "Peer: %s [%s]  resent: %u",
             peer_game_title, peer_game_code, (unsigned)link_rel.stat_retx);
  }

  return status_line2;
}

// ---------------------------------------------------------------------------
// Connection screen

#define LINK_COLOR_BG     COLOR15( 8, 15, 12)
#define LINK_COLOR_TEXT   COLOR15(31, 31, 29)
#define LINK_COLOR_HELP   COLOR15(20, 28, 22)
#define LINK_COLOR_WARN   COLOR15(28, 28, 12)

static void link_draw_screen(const char *title, const char *line1,
                             const char *line2, const char *hint)
{
  clear_screen(COLOR15_TO_32(LINK_COLOR_BG));

  print_string("     @ @", X_POS_CENTER, 50, COLOR15_GREEN, BG_NO_FILL);
  print_string("    (   )", X_POS_CENTER, 60, COLOR15_GREEN, BG_NO_FILL);
  print_string("   _) (_", X_POS_CENTER, 70, COLOR15_GREEN, BG_NO_FILL);

  print_string(title, X_POS_CENTER, 100, LINK_COLOR_TEXT, BG_NO_FILL);

  if (line1 && line1[0])
    print_string(line1, X_POS_CENTER, 130, LINK_COLOR_TEXT, BG_NO_FILL);

  if (line2 && line2[0])
    print_string(line2, X_POS_CENTER, 145, LINK_COLOR_WARN, BG_NO_FILL);

  if (hint && hint[0])
    print_string(hint, X_POS_CENTER, 240, LINK_COLOR_HELP, BG_NO_FILL);

  flip_screen(1);
}

static void link_show_message(const char *title, const char *line1,
                              const char *line2)
{
  GUI_ACTION_TYPE action;

  // Wait for a fresh button press (keeping an open session alive).
  do
  {
    link_menu_tick();
    link_draw_screen(title, line1, line2, "Press any button");
    action = get_gui_input();
  }
  while (action == CURSOR_NONE);
}

static int link_cancel_pressed(void)
{
  GUI_ACTION_TYPE action = get_gui_input();
  return (action == CURSOR_EXIT) || (action == CURSOR_BACK);
}

// Tears the session down. Keeps the network modules loaded so that a new
// connection does not need to load them again.
static void link_teardown(int send_bye)
{
  if ((pdp_id >= 0) && send_bye && (link_state != LINK_STATE_IDLE))
  {
    int i;
    for (i = 0; i < 3; i++)
    {
      send_dgram(peer_mac, PKT_BYE, 0, NULL, 0);
      sceKernelDelayThread(10 * 1000);
    }
  }

  if (link_state != LINK_STATE_IDLE)
    link_log_stop(send_bye ? "session closed here" : "session closed");

  link_state = LINK_STATE_IDLE;

  // Give the link port back to the original (unconnected) behaviour.
  serial_mode = SERIAL_MODE_DISABLED;
  netplay_client_id = 0;
  netplay_num_clients = 0;

  if (pdp_id >= 0)
  {
    sceNetAdhocPdpDelete(pdp_id, 0);
    pdp_id = -1;
  }

  adhoc_leave_group();
}

static void link_start_session(int final_mode)
{
  rel_init(&link_rel);

  netplay_client_id = link_is_host ? 0 : 1;
  netplay_num_clients = link_is_host ? 1 : 0;

  serial_mode = final_mode;

  link_log_start(link_is_host ? "HOST" : "GUEST", serial_mode_name(final_mode),
                 local_game_code, local_game_title);
  LLOG("LINK session %04X started. local=%02X:%02X:%02X:%02X:%02X:%02X peer=%02X:%02X:%02X:%02X:%02X:%02X",
       link_session, local_mac[0], local_mac[1], local_mac[2], local_mac[3], local_mac[4], local_mac[5],
       peer_mac[0], peer_mac[1], peer_mac[2], peer_mac[3], peer_mac[4], peer_mac[5]);
  LLOG("LINK peer game [%s] %s, option mode=%u, final mode=%s, client_id=%u",
       peer_game_code, peer_game_title, (unsigned)option_link_mode,
       serial_mode_name(final_mode), link_is_host ? 0 : 1);
  LLOG("LINK registers at start: SIOCNT=%04X RCNT=%04X", io_registers[REG_SIOCNT], io_registers[REG_RCNT]);

  serial_reset();

  last_rx_time = now_us();
  last_tx_time = 0;
  last_poll_ticks = cpu_ticks;

  link_state = LINK_STATE_CONNECTED;
}

// Host side of the pairing. Returns 1 when a guest joined.
static int link_pair_host(char *line2, size_t line2len)
{
  u32 start = now_us();
  u32 last_hello = 0;
  u8 pl[PAIR_PAYLOAD_SIZE];
  int my_mode = link_local_mode();
  int my_flex = link_local_mode_flexible();

  line2[0] = 0;

  while ((u32)(now_us() - start) < LINK_PAIR_TIMEOUT_US)
  {
    unsigned char src[6];
    u16 session, seq, ack, plen;
    u32 sack;
    u8 *payload;
    int type;

    // Announce ourselves: broadcast plus a direct copy to every peer of the
    // ad-hoc group, in case broadcasts get filtered.
    if ((u32)(now_us() - last_hello) >= LINK_PAIR_RESEND_US)
    {
      link_peer_info peers[4];
      int buflen = sizeof(peers);

      build_pair_payload(pl, (u8)my_mode, (u8)my_flex);
      send_dgram(broadcast_mac, PKT_HELLO, 0, pl, PAIR_PAYLOAD_SIZE);

      memset(peers, 0, sizeof(peers));
      if (sceNetAdhocctlGetPeerList(&buflen, peers) >= 0)
      {
        int i, n = buflen / (int)sizeof(link_peer_info);
        for (i = 0; i < n && i < 4; i++)
          send_dgram(peers[i].mac, PKT_HELLO, 0, pl, PAIR_PAYLOAD_SIZE);
      }

      last_hello = now_us();
    }

    while ((type = recv_dgram(src, &session, &seq, &ack, &sack, &payload, &plen)) >= 0)
    {
      if (type == PKT_JOIN && session == link_session && plen >= PAIR_PAYLOAD_SIZE)
      {
        int final_mode;
        int i;

        memcpy(peer_mac, src, 6);
        parse_pair_payload(payload, plen);

        final_mode = link_negotiate_mode(my_mode, my_flex, payload[14], payload[15]);

        if (final_mode != payload[14] && !payload[15] && !my_flex)
          snprintf(line2, line2len, "Mode mismatch! Using %s", serial_mode_name(final_mode));

        link_start_session(final_mode);

        // The WELCOME carries the final mode (header byte 14).
        build_pair_payload(pl, (u8)final_mode, 0);
        for (i = 0; i < 3; i++)
          send_dgram(peer_mac, PKT_WELCOME, 0, pl, PAIR_PAYLOAD_SIZE);

        return 1;
      }

      if (type == PKT_HELLO)
        snprintf(line2, line2len, "The other PSP is also HOST: choose GUEST there");
    }

    link_draw_screen("Waiting for the other PSP (GUEST)...",
                     "Hosting the link session", line2,
                     "O / [] : cancel");

    if (link_cancel_pressed())
      return 0;
  }

  snprintf(line2, line2len, "Timed out waiting for the guest");
  return 0;
}

// Guest side of the pairing. Returns 1 when the host accepted us.
static int link_pair_guest(char *line2, size_t line2len)
{
  u32 start = now_us();
  u32 last_join = 0;
  int found_host = 0;
  u8 pl[PAIR_PAYLOAD_SIZE];
  int my_mode = link_local_mode();
  int my_flex = link_local_mode_flexible();

  line2[0] = 0;

  while ((u32)(now_us() - start) < LINK_PAIR_TIMEOUT_US)
  {
    unsigned char src[6];
    u16 session, seq, ack, plen;
    u32 sack;
    u8 *payload;
    int type;

    while ((type = recv_dgram(src, &session, &seq, &ack, &sack, &payload, &plen)) >= 0)
    {
      if (type == PKT_HELLO)
      {
        // Adopt the host session (a host that restarted gets a new one).
        if (!found_host || session != link_session ||
            memcmp(src, peer_mac, 6) != 0)
        {
          memcpy(peer_mac, src, 6);
          link_session = session;
          parse_pair_payload(payload, plen);
          found_host = 1;
          last_join = 0;
        }
      }
      else if (type == PKT_WELCOME && found_host && session == link_session &&
               memcmp(src, peer_mac, 6) == 0)
      {
        int final_mode = link_peer_mode;   // Header byte 14 = final mode

        if (final_mode < SERIAL_MODE_RFU || final_mode > SERIAL_MODE_SERIAL_AW2)
          final_mode = my_mode;

        if (final_mode != my_mode && !my_flex)
          snprintf(line2, line2len, "Mode mismatch! Using %s", serial_mode_name(final_mode));

        link_start_session(final_mode);
        return 1;
      }
    }

    if (found_host && ((u32)(now_us() - last_join) >= LINK_PAIR_RESEND_US))
    {
      build_pair_payload(pl, (u8)my_mode, (u8)my_flex);
      send_dgram(peer_mac, PKT_JOIN, 0, pl, PAIR_PAYLOAD_SIZE);
      last_join = now_us();
    }

    link_draw_screen(found_host ? "Joining the HOST..." :
                                  "Looking for the other PSP (HOST)...",
                     found_host ? peer_game_title : "Guest mode", line2,
                     "O / [] : cancel");

    if (link_cancel_pressed())
      return 0;
  }

  snprintf(line2, line2len, found_host ? "The host did not answer" :
                                         "No host found");
  return 0;
}

int link_connect(int as_host)
{
  char err[64];
  char line2[64];
  u32 start;
  int ret;

  if (link_state != LINK_STATE_IDLE)
    link_teardown(1);

  status_line1[0] = 0;
  status_line2[0] = 0;

  if (gamepak_filename[0] == 0)
  {
    link_show_message("Link", "Load a game first", "");
    snprintf(status_line1, sizeof(status_line1), "Not connected");
    return 0;
  }

  if (sceWlanGetSwitchState() == 0)
  {
    link_show_message("Link", "The WLAN switch is off", "Turn it on and try again");
    snprintf(status_line1, sizeof(status_line1), "WLAN switch is off");
    return 0;
  }

  link_draw_screen("Starting the network...", "", "", "");

  if (link_net_init(err, sizeof(err)) < 0)
  {
    link_show_message("Link error", err, "Try restarting FrogGBA");
    snprintf(status_line1, sizeof(status_line1), "%s", err);
    return 0;
  }

  // Join (or create) the ad-hoc group shared by both PSPs.
  ret = sceNetAdhocctlConnect(LINK_GROUP_NAME);
  if (ret < 0)
  {
    snprintf(err, sizeof(err), "Ad-hoc connect failed (%08X)", ret);
    link_show_message("Link error", err, "");
    snprintf(status_line1, sizeof(status_line1), "%s", err);
    return 0;
  }
  adhocctl_connected = 1;

  start = now_us();
  while (adhocctl_state() != 1)
  {
    link_draw_screen("Joining the ad-hoc network...", "", "", "O / [] : cancel");

    if (link_cancel_pressed() || ((u32)(now_us() - start) > LINK_ADHOC_TIMEOUT_US))
    {
      adhoc_leave_group();
      snprintf(status_line1, sizeof(status_line1), "Ad-hoc network not available");
      return 0;
    }

    sceKernelDelayThread(50 * 1000);
  }

  sceNetGetLocalEtherAddr(local_mac);

  pdp_id = sceNetAdhocPdpCreate(local_mac, LINK_PDP_PORT, LINK_PDP_BUFSIZE, 0);
  if (pdp_id < 0)
  {
    snprintf(err, sizeof(err), "PDP socket failed (%08X)", pdp_id);
    pdp_id = -1;
    adhoc_leave_group();
    link_show_message("Link error", err, "");
    snprintf(status_line1, sizeof(status_line1), "%s", err);
    return 0;
  }

  link_is_host = as_host;
  peer_game_code[0] = 0;
  peer_game_title[0] = 0;
  rel_init(&link_rel);

  if (as_host)
  {
    // Random session id, so stale packets from an old session are ignored.
    link_session = (u16)(now_us() ^ (now_us() >> 16) ^ local_mac[5]);
    if (link_session == 0)
      link_session = 1;

    ret = link_pair_host(line2, sizeof(line2));
  }
  else
  {
    link_session = 0;
    ret = link_pair_guest(line2, sizeof(line2));
  }

  if (!ret)
  {
    link_teardown(0);
    snprintf(status_line1, sizeof(status_line1), "%s",
             line2[0] ? line2 : "Connection cancelled");
    return 0;
  }

  {
    char l1[64];
    char l2[128];
    snprintf(l1, sizeof(l1), "Connected! Mode: %s", serial_mode_name(serial_mode));
    snprintf(l2, sizeof(l2), "%s%s%s", line2, line2[0] ? " / " : "", link_log_status());
    link_show_message(l1, peer_game_title[0] ? peer_game_title : "", l2);
  }

  // Go straight back to the game.
  link_request_menu_exit = 1;
  return 1;
}

void link_disconnect(void)
{
  if (link_state != LINK_STATE_IDLE)
  {
    link_teardown(1);
    snprintf(status_line1, sizeof(status_line1), "Disconnected");
    status_line2[0] = 0;
  }
}

void link_shutdown(void)
{
  link_teardown(1);
  link_net_term();
}

// Diagnostics: transport state for the per-second log summary.
void link_debug_info(u32 *state, u32 *retx, u32 *unacked, u32 *full,
                     u32 *dgram_tx, u32 *dgram_rx, u32 *ms_since_rx,
                     u32 *srtt_ms, u32 *rto_ms, u32 *send_err)
{
  static u32 prev_tx = 0, prev_rx = 0, prev_retx = 0, prev_full = 0, prev_err = 0;

  *srtt_ms = link_rel.srtt / 1000;
  *rto_ms = ((link_rel.rto << link_rel.backoff) > REL_RTO_MAX ? REL_RTO_MAX :
             (link_rel.rto << link_rel.backoff)) / 1000;
  *send_err = send_errors - prev_err;
  prev_err = send_errors;

  *state = link_state;
  *retx = link_rel.stat_retx - prev_retx;
  *full = link_rel.stat_full - prev_full;
  *unacked = rel_unacked(&link_rel);
  *dgram_tx = dgram_tx_count - prev_tx;
  *dgram_rx = dgram_rx_count - prev_rx;
  *ms_since_rx = (now_us() - last_rx_time) / 1000;

  prev_tx = dgram_tx_count;
  prev_rx = dgram_rx_count;
  prev_retx = link_rel.stat_retx;
  prev_full = link_rel.stat_full;
}
