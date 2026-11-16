/* gameplaySP / FrogGBA
 *
 * Serial port (link port) emulation.
 * Ported from gpSP for libretro, Copyright (C) 2023-2025 David Guillen Fandos
 * <david@davidgf.net>. Adapted to FrogGBA and the PSP ad-hoc transport.
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
 */

#ifndef SERIAL_H
#define SERIAL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define SERIAL_MODE_DISABLED      0  // Original FrogGBA behaviour (no link)
#define SERIAL_MODE_GBP           1  // Not used in FrogGBA
#define SERIAL_MODE_RFU           2  // Wifi Adapter
#define SERIAL_MODE_SERIAL_POKE   3  // Connected to serial cable (Pokemon emulation mode)
#define SERIAL_MODE_SERIAL_AW1    4  // Connected to serial cable (AdvWars emulation mode)
#define SERIAL_MODE_SERIAL_AW2    5  // Connected to serial cable (AdvWars emulation mode)

// Active serial device. Anything other than DISABLED means a link session
// is open and the link port is routed through the network.
extern int serial_mode;

// Emulated CPU cycle counter (main.c).
extern u32 cpu_ticks;

// IO register helpers used by the ported code (the PSP is little endian,
// like the GBA, so no byte swapping is needed).
#define read_ioreg(regnum)        (io_registers[(regnum)])
#define write_ioreg(regnum, val)  io_registers[(regnum)] = (u16)(val)
#define read_ioreg32(regnum)      (read_ioreg(regnum) | (read_ioreg((regnum) + 1) << 16))

#define netorder32(value)         __builtin_bswap32(value)

#ifndef MIN
#define MIN(a, b) (((a) < (b)) ? (a) : (b))
#endif

// Register writes
CPU_ALERT_TYPE write_siocnt(u16 value);
CPU_ALERT_TYPE write_rcnt(u16 value);

// Serial IRQ interface
u32 serial_next_event(void);
bool update_serial(unsigned cycles);

// Resets every serial state machine (used on connect/reset/state load).
void serial_reset(void);
void serial_frame_update(void);
void serial_net_receive(const void *buf, size_t len, uint16_t client_id);

// RFU interface
void rfu_reset(void);
bool rfu_update(unsigned cycles);
u32 rfu_transfer(u32 value);
void rfu_frame_update(void);
void rfu_net_receive(const void* buf, size_t len, uint16_t client_id);

// Serial implementations
void serialproto_reset(void);

void serialpoke_frame_update(void);
void serialpoke_master_send(void);
bool serialpoke_update(unsigned cycles);
void serialpoke_net_receive(const void* buf, size_t len, uint16_t client_id);

void serialaw_master_send(void);
bool serialaw_update(unsigned cycles);
void serialaw_net_receive(const void* buf, size_t len, uint16_t client_id);

// Diagnostics (link_log.c)
void rfu_debug_info(u32 *comstate, u32 *state, u32 *clients, u32 *bcst);
void serialpoke_debug_info(u32 *state, u32 *count);

// Small PRNG used by the RFU emulation.
u16 rand_gen(void);
void rand_seed(u32 data);

#endif
