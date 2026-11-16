# The debug log (`link_log.txt`)

The debug log records what happens during a link session: connection steps, link protocol events, and a status block every second. It's the main tool for finding out why a session failed. **Logs from both PSPs**, read side by side, tell almost the whole story.

## Recording a log

1. On **both** PSPs: menu → **Link cable (ad-hoc)** → **Debug log: On (link_log.txt)**. Do this before connecting.
2. Connect and reproduce the problem.
3. Choose **Disconnect** (or exit FrogGBA) so the log is flushed and closed. The connection screen shows *Debug log: link_log.txt* when the file was opened, or *Debug log FAILED (code)* if not.
4. The file is `PSP/GAME/FrogGBA/link_log.txt` on each PSP.

Details:

- Each session is **appended** to the file, starting with a header. When the file is already larger than 3 MB, the next session overwrites it.
- The log is kept in RAM and written in large chunks, so it doesn't slow the game down. If the PSP crashes, the last few seconds may be missing.
- To protect performance, at most 1500 lines per second are written. The rest are counted in `dropped_log_lines`.
- Identical consecutive lines are collapsed into `    ^ repeated N more times`.

## Line format

```
==== FrogGBA link log  2026-11-16 14:48:19 ====
role=GUEST mode=Cable (Pokemon) game=[BPRE] POKEMON FIRE
[f000000 +000000] LINK session D3AC started. local=00:11:22:33:44:55 peer=66:77:88:99:AA:BB
[f001292 +280896] POKE peer 0 state 0 -> 1
```

- `fNNNNNN`: frame number since the session started (60 frames ≈ 1 second when the game runs at full speed).
- `+CCCCCC`: GBA CPU cycles into that frame (a frame has 280,896 cycles). Useful for ordering events within a frame.
- Then a **category** and the message.

Frame numbers are local to each PSP. To line up both logs, use events that both see, such as the session start, handshakes or a BYE.

## Categories

### `LINK`: transport and session

| Message | Meaning |
|---|---|
| `session XXXX started. local=… peer=…` | Pairing finished. Session id and both MAC addresses. |
| `peer game [CODE] TITLE, option mode=N, final mode=…, client_id=N` | The other PSP's game, this PSP's menu choice, the negotiated mode, and the protocol client id (0 host, 1 guest). |
| `registers at start: SIOCNT=… RCNT=…` | Link port registers when the session began. |
| `no packets from the other PSP for 6000 ms` | The peer went silent: out of range, paused, frozen or turned off. |
| `peer is answering again` | Packets resumed after the above. |
| `the other PSP closed the session (BYE)` | The other side disconnected or exited. |
| `send window full: message dropped` | More than 256 messages waiting for acknowledgement. The link was down for a long time. This should never appear in a healthy session. |
| `malformed data datagram` | A corrupted or incompatible datagram was ignored. |
| `log closed: …` | End of the session log, with the reason. |

### `SIO`: link port hardware mode

`SIO mode NORMAL -> MULTI (SIOCNT write 2000, RCNT=0000)` means the game switched the link port mode (NORMAL, MULTI, UART, GPIO). Games switch modes when they probe for a cable or an adapter. Pokémon cable sessions use **MULTI**, and the Wireless Adapter uses **NORMAL** (32-bit).

### `POKE`: Pokémon cable protocol

| Message | Meaning |
|---|---|
| `Reset serial-proto state` | The protocol state was cleared (session start, mode change or save state load). |
| `peer N state A -> B` | The other console's protocol state changed: 0 = PREINIT, 1 = HANDSHAKE, 2 = CONNECTED. |
| `Master handshake detected, moving to connected mode!` | HOST side: the game finished the handshake. |
| `Triggering IRQ for master handshake (handshake->connected).` | GUEST side: the emulated master completes the handshake. |
| `Detected handshake, switching state!` | The game started a new handshake (re-initialized the link, e.g. entering a room). A few are normal. Dozens in a row mean the two sides are struggling to agree, usually because of WiFi delay. |
| `POKE blk M=… S=…` | A completed block as the game saw it: 8 master words and 8 slave words. `(slave data not here yet)` / `(master data not here yet)` means the other console's block hadn't arrived, so zeros were used. This is normal at the start of a transfer. |
| `peer N timed out (state was S)` | No protocol message from the other console for 240 frames (4 s). The game will see the link as lost. Expected when leaving a link room, but not in the middle of a trade or battle. |
| `Packet dropped! (queue full)` | More than 256 blocks queued from the other console: data was lost. Should never happen. It means a long WiFi stall. |

Some first words you will recognise in Pokémon blocks (names from the [pret](https://github.com/pret/pokefirered) decompilation): `2222` send link type, `BBBB` start of a data block, `8888` data block continuation, `CAFE` held keys in the Cable Club rooms (the second word is the key code: `0011` none, `0012`–`0015` D-pad, `001A` idle…), `2FFE` ready to exit standby, `5FFF` ready to close the link.

### `RFU`: Wireless Adapter

Commands the game sends to the adapter, and the adapter's events, using gpSP's debug messages (e.g. `RFU reset!`, connection, broadcast and data events). Used for debugging the Union Room.

## Status block (every second)

Every 60 frames, four lines starting with `S`. **Counters are per period** (since the previous block), unless noted.

### `S cpu`

```
S cpu pc=081B3D3C/080F4A06/081B30D4/081B5EC4 T halt=0 ime=1 ie=0085 if=0000
```

- `pc=`: program counter sampled 4 times during the second. It shows where the game is (e.g. the same address 4 times means it is waiting in a loop).
- `T`/`A`: Thumb or ARM state. `halt`: CPU halted. `ime`/`ie`/`if`: interrupt master enable, enabled interrupts, pending interrupts. Bit 7 (`0x80`) of `ie` is the serial interrupt.

### `S sio`

```
S sio cnt=600B rcnt=0000 d32=00000000 mul=0000/0000/0000/0000 send=0000 tm2=0000:FFBF tm3=00C1:FF8D
```

Link registers: `SIOCNT`, `RCNT`, `SIODATA32`, `SIOMULTI0-3`, `SIOMLT_SEND`, and timers 2 and 3 (control:counter), which the games use to pace link transfers.

### `S ctr`

```
S ctr nrm=0 mul=540 irq=540 clk=0 | rfu cs=0 st=0 cl=0 bc=0 cmd=0 err=0 wait=0 ev(d/n/t/x)=0/0/0/0 out=0 blk=0 pkt tx/rx=0/0 qmax=0 qdrop=0 stall=0 paced=0 lag=0 rtx=0 | poke st=2/2 q=0/12 blk=… nopeer=… tx=… rx=… drop=0 sirq=…
```

| Field | Meaning |
|---|---|
| `nrm`, `mul` | Normal-mode and multiplayer transfers started |
| `irq` | Serial interrupts raised |
| `clk` | Clock direction switches |
| `rfu cs st cl bc` | Wireless Adapter: communication state, state, connected clients, broadcasting (current values) |
| `cmd`, `err` | Adapter commands processed / answered with an error |
| `wait`, `ev(d/n/t/x)` | Adapter waits started, and how they ended: data, no response, timeout, disconnect |
| `out`, `blk` | Words pushed to the game in the adapter's reverse-clock mode / pushes delayed |
| `pkt tx/rx` | Wireless data packets sent to / received from the other PSP |
| `qmax`, `qdrop` | Deepest wireless receive queue seen / packets dropped because it was full |
| `stall` | HOST: waits answered "no ACK" to slow the game down because the guest is behind (flow control) |
| `paced` | GUEST: waits where a packet was held back (one per frame) |
| `lag` | HOST: most host packets the guest's game hadn't consumed yet |
| `rtx` | Retransmit requests (`RTX_WAIT`) from the game |
| `poke st=A/B` | Pokémon cable protocol state of client 0 / client 1 (0–2, current values) |
| `q=A/B` | Blocks queued from client 0 / client 1 (current values) |
| `blk`, `nopeer` | Blocks completed / completed without the other side's data |
| `tx`, `rx` | Blocks sent / received |
| `drop` | Blocks dropped (queue full) |
| `sirq` | GUEST: fake master interrupts raised |

### `S net`

```
S net st=1 msg tx=60 rx=62 | dgram tx=60 rx=59 err=0 | retx=1 unacked=3 full=0 | rtt=49ms rto=120ms | last_rx=16ms ago | dropped_log_lines=0
```

| Field | Meaning |
|---|---|
| `st` | Transport state: 0 idle, 1 connected, 2 lost |
| `msg tx/rx` | Protocol messages sent/delivered in the period |
| `dgram tx/rx` | WiFi datagrams sent/received in the period |
| `err` | Datagram send errors |
| `retx` | Messages re-sent in the period |
| `unacked` | Messages waiting for acknowledgement (current) |
| `full` | Times the send window was full |
| `rtt`, `rto` | Smoothed round-trip time and current retransmission timeout |
| `last_rx` | Time since the last datagram from the other PSP |
| `dropped_log_lines` | Log lines skipped by the rate limit in the period |

## Reading a log: what healthy looks like

- **Idle session** (connected, nobody using the link): `msg tx=0 rx=0`, a few keepalive datagrams (`dgram tx≈4`), `last_rx` below 300 ms.
- **Active cable link** (Cable Club): about 60 messages per second each way, `unacked` small (under ~30), `rtt` mostly under ~100 ms, `retx` low, no `Packet dropped!`, no `timed out` until you leave the room.
- **Active wireless link:** `pkt tx/rx` flowing, `qdrop=0`. `stall`/`paced` above zero are fine: that is flow control doing its job.

## Common patterns

| You see | Likely cause |
|---|---|
| `last_rx` above 1000 ms on both PSPs at the same moment, right after a save | Older builds: both consoles froze while writing the save to the Memory Stick. Fixed in v5 (background saving). |
| `rtt` in the hundreds of ms or more, `unacked` near 100+, low `dgram` counts | WiFi interference or stalls. Try another ad-hoc channel and keep the PSPs closer. |
| `Packet dropped! (queue full)` / `qdrop>0` | A long WiFi stall overflowed a queue. Report it with both logs. |
| `peer N timed out` in the middle of a trade | The other console stopped talking for 4 s: menu opened, sleep, out of range, or a crash. |
| Many `Detected handshake, switching state!` in a row | The games kept restarting the link handshake because of WiFi delay. Usually recovers. |
| `LINK no packets from the other PSP for 6000 ms` | Out of range, the other PSP is in the menu or asleep, or it crashed. |
