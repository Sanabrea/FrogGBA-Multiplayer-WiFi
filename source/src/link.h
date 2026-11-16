/* FrogGBA - Link over ad-hoc
 *
 * Emulates the GBA link port between two PSPs using the PSP ad-hoc WiFi.
 * The serial protocols themselves (Wireless Adapter and the latency
 * tolerant Pokemon / Advance Wars link cable modes) come from gpSP for
 * libretro by David Guillen Fandos; this module provides the transport.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 2 of
 * the License, or (at your option) any later version.
 */

#ifndef LINK_H
#define LINK_H

#include <stddef.h>
#include <stdint.h>

// Values for option_link_mode (menu order)
#define LINK_MODE_AUTO       0
#define LINK_MODE_RFU        1   // GBA Wireless Adapter
#define LINK_MODE_POKE       2   // Link cable, Pokemon Gen3 protocol
#define LINK_MODE_AW1        3   // Link cable, Advance Wars 1 protocol
#define LINK_MODE_AW2        4   // Link cable, Advance Wars 2 protocol
#define LINK_MODE_COUNT      5

extern u32 option_link_mode;

// Set by the link module when the menu should return to the game
// (for example right after a successful connection).
extern u32 link_request_menu_exit;

// Emulated "netplay" identity used by the serial protocols:
// the ad-hoc host is client 0, the guest is client 1.
extern u32 netplay_client_id;
extern u32 netplay_num_clients;

// Called after a ROM is loaded (used by the Auto mode).
void link_game_loaded(const char *game_code, const char *game_title);

// Called on GBA reset / savestate load to reset the serial state machines.
void link_reset_serial(void);

// Blocking connection screen (runs inside the menu). Returns 1 if connected.
int link_connect(int as_host);

// Closes the session (notifies the peer).
void link_disconnect(void);

// Once per emulated frame.
void link_frame_update(void);

// Once per menu refresh, keeps the session alive while the menu is open.
void link_menu_tick(void);

// True while a session is open (serial emulation routed to the network).
int link_is_active(void);

// Short human readable status lines for the menu.
const char *link_status_line1(void);
const char *link_status_line2(void);

// Diagnostics (link_log.c)
void link_debug_info(u32 *state, u32 *retx, u32 *unacked, u32 *full,
                     u32 *dgram_tx, u32 *dgram_rx, u32 *ms_since_rx,
                     u32 *srtt_ms, u32 *rto_ms, u32 *send_err);

// Emulator shutdown: closes the session and unloads the network modules.
void link_shutdown(void);

// Transport used by the serial protocol emulation (serial*.c, rfu.c).
#define LINK_BROADCAST_ID    0xFFFF
void netpacket_send(uint16_t client_id, const void *buf, size_t len);
void netpacket_poll_receive(void);

#endif
