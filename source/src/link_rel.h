/* FrogGBA - Link over ad-hoc
 *
 * link_rel: a tiny reliable, ordered message channel built on top of an
 * unreliable datagram transport (PSP ad-hoc PDP).
 *
 *  - Selective repeat: every datagram carries a cumulative ACK (next
 *    sequence number we expect) plus a 32 bit SACK mask of the following
 *    messages already held in the reorder buffer.
 *  - Messages are queued and then packed several per datagram (records),
 *    both new ones and re-sends, so the packet rate stays low. The PSP
 *    ad-hoc link saturates at a few hundred small packets per second.
 *  - The retransmission timeout adapts to the measured round trip time
 *    (RFC 6298 style, Karn's rule) and backs off exponentially while the
 *    oldest message keeps timing out, so a slow link never turns into a
 *    retransmission storm.
 *
 * This file has no PSP dependencies so it can be unit tested on a PC.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 2 of
 * the License, or (at your option) any later version.
 */

#ifndef LINK_REL_H
#define LINK_REL_H

#include <stdint.h>

#define REL_MAX_PAYLOAD   576      // Largest message we accept (bytes)
#define REL_WINDOW        256      // Max unacknowledged messages (power of 2)
#define REL_RXWIN         32       // Reorder buffer (matches the SACK mask)
#define REL_REC_HDR       4        // Record header: seq (2) + length (2)

#define REL_RTO_INIT      250000   // us, before the first RTT sample
#define REL_RTO_MIN       120000   // us
#define REL_RTO_MAX       2000000  // us
#define REL_MAX_BACKOFF   4        // RTO doubles up to 16x

typedef struct
{
  uint16_t len;
  uint8_t  sent;                   // Transmitted at least once
  uint8_t  sacked;                 // Peer reported it holds this message
  uint8_t  nacked;                 // Peer holds later messages: likely lost
  uint8_t  tx_count;
  uint32_t last_tx;                // Timestamp (us) of the last transmission
  uint8_t  data[REL_MAX_PAYLOAD];
} rel_slot_t;

typedef struct
{
  uint16_t len;
  uint8_t  valid;
  uint8_t  pad;
  uint8_t  data[REL_MAX_PAYLOAD];
} rel_rxslot_t;

// Callback used to deliver a message to the upper layer (in order).
typedef void (*rel_rx_fn)(void *ctx, const uint8_t *data, uint16_t len);

typedef struct
{
  // Sender side
  uint16_t next_seq;               // Sequence number for the next new message
  uint16_t base_seq;               // Oldest message not yet acknowledged
  rel_slot_t slots[REL_WINDOW];    // Indexed by seq % REL_WINDOW

  // Receiver side
  uint16_t expected_seq;           // Next in-order sequence we expect
  uint8_t  ack_pending;            // We owe the peer an ACK
  rel_rxslot_t rx[REL_RXWIN];      // Reorder buffer, indexed by seq % REL_RXWIN

  // Round trip time estimation
  uint32_t srtt;                   // Smoothed RTT (us), 0 = no sample yet
  uint32_t rttvar;                 // RTT variation (us)
  uint32_t rto;                    // Current base timeout (us)
  uint32_t backoff;                // Exponential backoff shift

  // Statistics (for the status screen / debugging)
  uint32_t stat_sent;
  uint32_t stat_retx;
  uint32_t stat_full;
  uint32_t stat_dup;
  uint32_t stat_ooo;
  uint32_t stat_delivered;
} rel_chan_t;

void rel_init(rel_chan_t *c);

// Number of messages queued but not acknowledged yet.
int rel_unacked(const rel_chan_t *c);

// True if some queued message was never transmitted.
int rel_has_unsent(const rel_chan_t *c);

// Queues a new message (it is transmitted by rel_collect). Returns 0, or -1
// if the message is too big or the window is full (peer not acknowledging).
int rel_push(rel_chan_t *c, const void *data, uint16_t len);

// Packs messages to transmit into buf as records [seq][len][data]:
// first the re-sends whose timeout expired (oldest first), then never sent
// messages (unless new_only, which only takes never sent ones). Returns the
// number of bytes written (0 = nothing to send).
uint32_t rel_collect(rel_chan_t *c, uint32_t now, uint8_t *buf,
                     uint32_t max_bytes, int new_only);

// SACK mask to send along with the cumulative ACK (expected_seq):
// bit i set means message expected_seq + 1 + i is already buffered.
uint32_t rel_sack_mask(const rel_chan_t *c);

// Processes the cumulative ACK + SACK mask received from the peer.
void rel_on_ack(rel_chan_t *c, uint16_t ack, uint32_t sack, uint32_t now);

// Processes a received data message. Delivers it (and any buffered
// messages that become in-order) through the callback.
void rel_on_data(rel_chan_t *c, uint16_t seq, const uint8_t *data,
                 uint16_t len, rel_rx_fn deliver, void *ctx);

// Parses a datagram payload made of records and feeds rel_on_data.
// Returns -1 if the payload is malformed.
int rel_on_records(rel_chan_t *c, const uint8_t *buf, uint32_t len,
                   rel_rx_fn deliver, void *ctx);

#endif
