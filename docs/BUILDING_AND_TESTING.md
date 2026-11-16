# Building and testing

## Building for PSP

The multiplayer code builds exactly like upstream FrogGBA. No extra dependencies are needed beyond a PSP toolchain (pspdev/PSPSDK), which already ships the network libraries used.

### With Docker (as upstream)

See [BUILD_INSTRUCTIONS.md](../BUILD_INSTRUCTIONS.md):

```bash
docker-compose up --build
# EBOOT.PBP and FrogGBA.prx end up in build/
```

### With a local pspdev toolchain

```bash
export PSPDEV=/path/to/pspdev
export PATH=$PATH:$PSPDEV/bin
cd source
make -j4
# -> source/EBOOT.PBP
```

### What the multiplayer port changes in the build

`source/Makefile` adds six objects and five PSP libraries:

```make
OBJS += serial.o serial_proto.o rfu.o link.o link_rel.o link_log.o
LIBS += -lpspnet_adhocctl -lpspnet_adhoc -lpspnet -lpsputility -lpspwlan
```

### Packaging a release

The release ZIP has the same layout as upstream:

```
PSP/GAME/FrogGBA/EBOOT.PBP
PSP/GAME/FrogGBA/ku_bridge.prx
PSP/GAME/FrogGBA/exception.prx
PSP/GAME/FrogGBA/dir.ini
PSP/GAME/FrogGBA/{roms,save,state,cfg}/
```

Users only need to replace `EBOOT.PBP` to update. Both PSPs must run builds with the same `LINK_PROTO_VERSION` (`source/src/link.c`). Bump it whenever the wire format or the protocol behavior changes, so that incompatible builds ignore each other instead of misbehaving.

## Testing without PSPs

`tests/link/` contains PC tools that compile the real link sources against a mock of the PSP ad-hoc API. See [tests/link/README.md](../tests/link/README.md). In short:

```bash
bash tests/link/unit/run.sh                                     # reliable channel
bash tests/link/sim/run_cable.sh BPRE "POKEMON FIRE" BPRE "POKEMON FIRE" 20 0.05 5000 3000
bash tests/link/sim/run_rfu.sh BPRE "POKEMON FIRE" BPRE "POKEMON FIRE" 20 0.02 4000 2000
bash tests/link/sim/run_pokemon_wireless.sh 20 0.02 4000 2000
```

They model packet loss, latency, jitter, limited airtime and WiFi outages, and check that the game-level data arrives complete and in order. The Pokémon models follow the pret decompilations of FireRed and Emerald (`link.c`, `link_rfu_2.c`, librfu).

## Testing on real hardware

The simulators can't reproduce everything: PSP thread scheduling, real WiFi behavior and Memory Stick timing all matter. The reference test is two PSPs:

1. Install the same build on both and enable **Debug log**.
2. Cable: connect with Mode *Auto*, then trade and battle at the Cable Club.
3. Wireless: Mode *Wireless (beta)*, connect before entering the Pokémon Center, then use the Direct Corner and the Union Room (chat, trade, battle).
4. After each activity, leave the room and check that **both** consoles save and return to the overworld.
5. Compare both `link_log.txt` files (see [DEBUG_LOG.md](DEBUG_LOG.md)): look for `timed out`, `queue full`, long `last_rx` gaps and high `rtt`.
