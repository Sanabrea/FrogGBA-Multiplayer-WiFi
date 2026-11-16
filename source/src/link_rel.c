/* FrogGBA - Link over ad-hoc
 *
 * Reliable ordered message channel (selective repeat, adaptive timeout,
 * several messages per datagram). See link_rel.h.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 2 of
 * the License, or (at your option) any later version.
 */

#include <string.h>
#include "link_rel.h"

#define REL_MASK    (REL_WINDOW - 1)
#define RX_MASK     (REL_RXWIN - 1)

// Holes reported by the SACK are re-sent after half the timeout, but not
// faster than this.
#define REL_FAST_RETX_MIN   40000

static void put16(uint8_t *p, uint16_t v) { p[0] = v & 0xFF; p[1] = v >> 8; }
static uint16_t get16(const uint8_t *p)   { return p[0] | (p[1] << 8); }

void rel_init(rel_chan_t *c)
{
  memset(c, 0, sizeof(*c));
  c->rto = REL_RTO_INIT;
}

int rel_unacked(const rel_chan_t *c)
{
  return (uint16_t)(c->next_seq - c->base_seq);
}

int rel_has_unsent(const rel_chan_t *c)
{
  // Sent messages are always a prefix of the window: checking the newest
  // queued message is enough.
  if (c->next_seq == c->base_seq)
    return 0;

  return !c->slots[(uint16_t)(c->next_seq - 1) & REL_MASK].sent;
}

int rel_push(rel_chan_t *c, const void *data, uint16_t len)
{
  rel_slot_t *slot;

  if (len > REL_MAX_PAYLOAD)
    return -1;

  if (rel_unacked(c) >= REL_WINDOW)
  {
    c->stat_full++;
    return -1;
  }

  slot = &c->slots[c->next_seq & REL_MASK];
  memcpy(slot->data, data, len);
  slot->len = len;
  slot->sent = 0;
  slot->sacked = 0;
  slot->nacked = 0;
  slot->tx_count = 0;
  slot->last_tx = 0;

  c->next_seq++;
  c->stat_sent++;

  return 0;
}

static uint32_t slot_timeout(const rel_chan_t *c, const rel_slot_t *slot)
{
  uint32_t t;

  if (slot->nacked)
  {
    t = c->rto / 2;
    return (t < REL_FAST_RETX_MIN) ? REL_FAST_RETX_MIN : t;
  }

  t = c->rto << c->backoff;
  return (t > REL_RTO_MAX) ? REL_RTO_MAX : t;
}

static uint32_t write_record(uint8_t *out, uint16_t seq, const rel_slot_t *slot)
{
  put16(out, seq);
  put16(out + 2, slot->len);
  memcpy(out + REL_REC_HDR, slot->data, slot->len);
  return REL_REC_HDR + slot->len;
}

uint32_t rel_collect(rel_chan_t *c, uint32_t now, uint8_t *buf,
                     uint32_t max_bytes, int new_only)
{
  uint32_t used = 0;
  uint16_t seq;

  // 1) Re-sends whose timer expired, oldest first.
  if (!new_only)
  {
    for (seq = c->base_seq; seq != c->next_seq; seq++)
    {
      rel_slot_t *slot = &c->slots[seq & REL_MASK];

      if (!slot->sent)
        break;      // Only never sent messages from here on

      if ((uint16_t)(seq - c->base_seq) >= REL_RXWIN)
        break;      // Beyond what the receiver can hold

      if (slot->sacked)
        continue;

      if ((uint32_t)(now - slot->last_tx) < slot_timeout(c, slot))
        continue;

      if (used + REL_REC_HDR + slot->len > max_bytes)
        break;

      // The oldest message timed out (not a SACK hole): back off.
      if (seq == c->base_seq && !slot->nacked && c->backoff < REL_MAX_BACKOFF)
        c->backoff++;

      used += write_record(buf + used, seq, slot);
      slot->last_tx = now;
      slot->nacked = 0;
      if (slot->tx_count < 255)
        slot->tx_count++;
      c->stat_retx++;
    }
  }

  // 2) Messages never sent. Flow control: the receiver only buffers
  // REL_RXWIN messages from its next expected one, anything further would
  // be thrown away and re-sent later, wasting air time.
  for (seq = c->base_seq; seq != c->next_seq; seq++)
  {
    rel_slot_t *slot = &c->slots[seq & REL_MASK];

    if ((uint16_t)(seq - c->base_seq) >= REL_RXWIN)
      break;

    if (slot->sent)
      continue;

    if (used + REL_REC_HDR + slot->len > max_bytes)
      break;

    used += write_record(buf + used, seq, slot);
    slot->sent = 1;
    slot->last_tx = now;
    slot->tx_count = 1;
  }

  return used;
}

uint32_t rel_sack_mask(const rel_chan_t *c)
{
  uint32_t mask = 0;
  uint32_t i;

  // Slot for seq (expected + 1 + i) is rx[(expected + 1 + i) % RXWIN].
  // The slot of "expected" itself is always empty (else it was delivered).
  for (i = 0; i < REL_RXWIN - 1; i++)
  {
    if (c->rx[(uint16_t)(c->expected_seq + 1 + i) & RX_MASK].valid)
      mask |= 1u << i;
  }

  return mask;
}

static void rtt_sample(rel_chan_t *c, uint32_t sample)
{
  uint32_t var;

  if (!c->srtt)
  {
    c->srtt = sample;
    c->rttvar = sample / 2;
  }
  else
  {
    uint32_t delta = (c->srtt > sample) ? (c->srtt - sample) : (sample - c->srtt);
    c->rttvar = (3 * c->rttvar + delta) / 4;
    c->srtt = (7 * c->srtt + sample) / 8;
  }

  var = 4 * c->rttvar;
  if (var < 20000)
    var = 20000;

  c->rto = c->srtt + var;
  if (c->rto < REL_RTO_MIN)
    c->rto = REL_RTO_MIN;
  if (c->rto > REL_RTO_MAX)
    c->rto = REL_RTO_MAX;
}

void rel_on_ack(rel_chan_t *c, uint16_t ack, uint32_t sack, uint32_t now)
{
  uint16_t in_flight = (uint16_t)(c->next_seq - c->base_seq);
  uint16_t acked     = (uint16_t)(ack - c->base_seq);
  uint32_t i;
  // RTT sample: the newest message acknowledged by THIS ack (cumulatively or
  // by SACK) that was sent only once (Karn's rule) and was not already
  // sacked. A message that waited at the receiver behind a hole was sacked
  // earlier, so it does not inflate the estimate when the hole is filled.
  int have_sample = 0;
  uint32_t sample = 0;

  // Ignore stale or bogus ACKs (they would point outside the window).
  if (acked > in_flight)
    return;

  if (acked)
  {
    uint16_t seq = ack;
    do
    {
      rel_slot_t *slot;
      seq--;
      slot = &c->slots[seq & REL_MASK];
      if (slot->sent && slot->tx_count == 1 && !slot->sacked)
      {
        sample = now - slot->last_tx;
        have_sample = 1;
        break;
      }
    }
    while (seq != c->base_seq);

    c->base_seq = ack;
    c->backoff = 0;   // The link moves again
  }

  // Mark selectively acknowledged messages so they are not re-sent, and
  // flag the holes below the highest sacked message as probably lost.
  if (sack && (c->base_seq != c->next_seq))
  {
    uint32_t highest = 0;

    in_flight = (uint16_t)(c->next_seq - c->base_seq);

    for (i = 0; i < 32; i++)
    {
      uint16_t off = (uint16_t)(1 + i);

      if (off >= in_flight)
        break;

      if (sack & (1u << i))
      {
        rel_slot_t *slot = &c->slots[(uint16_t)(c->base_seq + off) & REL_MASK];

        // Newer than anything acked cumulatively: preferred as sample.
        if (!slot->sacked && slot->sent && slot->tx_count == 1)
        {
          sample = now - slot->last_tx;
          have_sample = 1;
        }

        slot->sacked = 1;
        highest = off;
      }
    }

    // The message at base_seq itself is always missing when a SACK exists.
    for (i = 0; i < highest; i++)
    {
      rel_slot_t *slot = &c->slots[(uint16_t)(c->base_seq + i) & REL_MASK];
      if (!slot->sacked && slot->sent)
        slot->nacked = 1;
    }
  }

  if (have_sample)
    rtt_sample(c, sample);
}

void rel_on_data(rel_chan_t *c, uint16_t seq, const uint8_t *data,
                 uint16_t len, rel_rx_fn deliver, void *ctx)
{
  int16_t diff = (int16_t)(seq - c->expected_seq);

  c->ack_pending = 1;

  if (diff < 0)
  {
    c->stat_dup++;      // Already delivered, our ACK was probably lost.
    return;
  }

  if (diff >= REL_RXWIN || len > REL_MAX_PAYLOAD)
  {
    c->stat_ooo++;      // Too far ahead, the sender will resend it later.
    return;
  }

  if (diff > 0)
  {
    rel_rxslot_t *slot = &c->rx[seq & RX_MASK];

    if (slot->valid)
    {
      c->stat_dup++;
      return;
    }

    memcpy(slot->data, data, len);
    slot->len = len;
    slot->valid = 1;
    c->stat_ooo++;
    return;
  }

  // In order: deliver it, then everything that was waiting behind it.
  deliver(ctx, data, len);
  c->expected_seq++;
  c->stat_delivered++;

  while (c->rx[c->expected_seq & RX_MASK].valid)
  {
    rel_rxslot_t *slot = &c->rx[c->expected_seq & RX_MASK];

    slot->valid = 0;
    deliver(ctx, slot->data, slot->len);
    c->expected_seq++;
    c->stat_delivered++;
  }
}

int rel_on_records(rel_chan_t *c, const uint8_t *buf, uint32_t len,
                   rel_rx_fn deliver, void *ctx)
{
  while (len >= REL_REC_HDR)
  {
    uint16_t seq  = get16(buf);
    uint16_t mlen = get16(buf + 2);

    if ((uint32_t)mlen > len - REL_REC_HDR)
      return -1;

    rel_on_data(c, seq, buf + REL_REC_HDR, mlen, deliver, ctx);

    buf += REL_REC_HDR + mlen;
    len -= REL_REC_HDR + mlen;
  }

  return len ? -1 : 0;
}
