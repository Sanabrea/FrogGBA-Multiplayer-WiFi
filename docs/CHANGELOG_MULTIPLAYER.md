# Multiplayer changelog

The multiplayer WiFi feature was developed through a series of test builds, each checked on two real PSPs with Pokémon FireRed and with debug logs from both consoles. This is what each build changed and why.

## v6: cable mode latency and buffer

*Link protocol version 3. Compatible with v4 and v5.*

- **Cable mode now gives the PSP network stack CPU time every frame**, as wireless mode already did (a 100 µs yield). In logs from a working v4 cable session, round-trip times in cable mode were about 4 times higher than in wireless mode (median about 220 ms vs 53 ms, peaks of 1.5 s) whenever the emulator had no idle time. The network helper threads were being starved.
- **The Pokémon cable receive queue is twice as deep**: 256 blocks (about 4 s) instead of 128. In the same session, a WiFi stall delivered a burst that overflowed the old queue, and 4 blocks were dropped. They happened to be room movement keys (`CAFE` commands), but a dropped block can desync the players' positions or corrupt data. In the simulator with 2.8 s outages, v5 lost data 11 times and v6 none.

## v5: background saving during link sessions

- While a link session is open, the save is written to the Memory Stick by a **background thread** from a snapshot, instead of freezing the emulator for one to several seconds.
- Why: after a wireless battle, both games saved. One PSP finished and left the room, while the other was frozen writing the Memory Stick, fell behind, and was taken out of the room by the game. The same freeze was visible in cable logs (more than 1 s of silence on both PSPs right after saving).
- Synchronous saves (exit, suspend, loading another game) wait for any background write to finish first.

## v4: Wireless Adapter flow control: Union Room works

*Link protocol version 3. Incompatible with older builds.*

- **Flow control between the emulated adapters.** The guest's game receives at most one data packet per frame (burst of 2). The guest reports how many packets its game has consumed. The host answers "no ACK" to its game when the guest falls more than 12 packets behind, so librfu re-sends later. That's a situation the game already handles.
- **No network re-send for `RTX_WAIT`**: the transport is already reliable, so duplicates would only fill the guest's queue.
- Wireless mode sends about half as much WiFi traffic as v3.
- Why: in v3, the session broke in the trade room right after both players were ready. Pokémon's wireless library keeps a 20-slot receive queue on the child console. After a WiFi hiccup, up to 22 packets arrived at once, the game raised a link error and reset the adapter.
- **Result on real PSPs:** wireless trades, Union Room chat and a wireless battle worked (FireRed). A cable trade session (English FireRed) also worked.

## v3: Wireless Adapter groundwork

- The PSP network threads run at a higher priority than the emulator, and in wireless mode the emulator yields briefly every frame. When the game spins waiting for the adapter, the network had no CPU time, so packets arrived in bursts.
- Wireless packet queues 4 times larger.
- A lighter debug log: buffered writes, a rate limit, collapsed repeated lines.
- Result: the Union Room was reached and the trade room entered, but the session broke when both players were ready (fixed in v4).

## v2: new transport: the cable works

*Link protocol version 2.*

- New reliable channel: several messages packed per datagram (records), selective acknowledgements, adaptive retransmission timeout with Karn's rule and exponential back-off.
- Result: Pokémon cable battles worked on real PSPs.

## v1: first ad-hoc link

- First implementation: PSP ad-hoc transport (group `FROGLNK1`), HOST/GUEST pairing with mode negotiation, link port and protocol emulation ported from gpSP for libretro (Pokémon and Advance Wars cable, Wireless Adapter), the *Link cable (ad-hoc)* menu and the debug log.

Also done in the early builds: FrogGBA's timer prescaler speed hack is disabled during link sessions, because the games time their link handshakes with those timers.
