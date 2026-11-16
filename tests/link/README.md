# Link tests (run on a PC)

These tools test the multiplayer link code **without PSPs**. They compile FrogGBA's real link sources (`source/src/link.c`, `link_rel.c`, `link_log.c`, `serial.c`, `serial_proto.c`, `rfu.c`) for Linux, against a small mock of the PSP network API. Two processes play the two PSPs.

Requirements: Linux (or WSL), `gcc` with AddressSanitizer/UBSan, `bash`. Run the commands below from this `tests/link` folder.

## Unit test: reliable channel

```bash
bash unit/run.sh
```

`test_link_rel.c` runs two endpoints of `link_rel.c` over a simulated datagram link with limited capacity, latency, jitter (reordering), random loss and duplicates. It checks exactly-once, in-order delivery with intact content, and reports latency percentiles, re-sends and packet rate. It ends with `ALL PASS`.

## Two-process simulators

The mock (`sim/psp_mock.c`) emulates ad-hoc PDP sockets over UDP on `127.0.0.1`, and can degrade the "air":

| Argument / variable | Meaning |
|---|---|
| `loss` (arg) / `LINK_LOSS` | Packet loss probability, 0.0–1.0 |
| `delay_us` (arg) / `LINK_DELAY_US` | One-way delay (default 3000) |
| `jitter_us` (arg) / `LINK_JITTER_US` | Random extra delay, which reorders packets |
| `portbase` (arg) / `LINK_PORTBASE` | Base UDP port. Use different ports to run tests in parallel. |
| `LINK_RATE` | Datagrams per second the air carries per direction (0 = unlimited). A 24-datagram queue drops the excess. |
| `LINK_STALL_MS`, `LINK_STALL_EVERY_MS` | Total outage of `LINK_STALL_MS` every `LINK_STALL_EVERY_MS` (WiFi hiccups) |

Each script prints a `PASS`/`FAIL` line per side, and saves both `link_log.txt` files in `sim/logs_out/`.

### Pokémon cable

```bash
bash sim/run_cable.sh BPRE "POKEMON FIRE" BPRE "POKEMON FIRE" 20 0.05 5000 3000
```

`gba_sim.c` is a minimal GBA model running a C port of the Gen3 Pokémon serial link driver (handshake, send/receive queues, checksums, timer-paced multiplayer transfers, serial IRQ), on top of the real link code. Each side sends one numbered 8-word block per frame and verifies that the other side's blocks arrive complete, uncorrupted and in sequence, with no bad checksum. The first four arguments are the game code and title of each side, which also tests mode negotiation. `LINK_MODE` forces the menu's *Mode* option on both sides (0 Auto, 1 Wireless, 2 Cable Pokémon, 3/4 Advance Wars 1/2).

Example with WiFi outages:

```bash
LINK_STALL_MS=1500 LINK_STALL_EVERY_MS=5000 bash sim/run_cable.sh BPRE "POKEMON FIRE" BPRE "POKEMON FIRE" 20 0.05 5000 3000 47030
```

### Wireless Adapter: basic

```bash
bash sim/run_rfu.sh BPRE "POKEMON FIRE" BPRE "POKEMON FIRE" 20 0.02 4000 2000
```

`rfu_sim.c` drives the real `rfu.c` through its command interface like a game would. The host broadcasts and accepts, the guest searches and connects, and then both exchange one numbered packet per frame.

### Wireless Adapter: Pokémon trade room model

```bash
bash sim/run_pokemon_wireless.sh 20 0.02 4000 2000
SIM_FPS_GUEST=55 bash sim/run_pokemon_wireless.sh 20 0.02 4000 2000 47150   # guest slower than host
```

`poke_rfu_sim.c` models what Pokémon FireRed/Emerald do in the wireless trade room (the "MSC" mode of `link_rfu_2.c` and librfu, from the pret decompilations). The parent sends with `SEND_DATAW`/`RTX_WAIT` once per frame and skips the frame when not acknowledged. The child answers every adapter event and keeps the game's **20-slot receive queue**, which is fatal when full, like in the real game. Extra variables: `SIM_FPS_HOST`, `SIM_FPS_GUEST` (emulated frame rates), `RFU_SRC` (path to an alternative `rfu.c`, to compare versions), `OUTNAME` (log name prefix).
