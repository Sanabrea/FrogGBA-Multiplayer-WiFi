#!/bin/bash
# Pokemon-like wireless test. Usage: ./run_pokemon_wireless.sh <seconds> [loss] [delay_us] [jitter_us] [portbase]
# Env: SIM_FPS_HOST / SIM_FPS_GUEST, LINK_RATE, LINK_STALL_MS, LINK_STALL_EVERY_MS, RFU_SRC (alt rfu.c), OUTNAME
set -e
cd "$(dirname "$0")"
SRC="$(cd ../../../source/src && pwd)"
B=$(mktemp -d)
cp $SRC/link.c $SRC/link.h $SRC/link_rel.c $SRC/link_rel.h $SRC/link_log.c $SRC/link_log.h $SRC/serial.c $SRC/serial.h $SRC/serial_proto.c $SRC/rfu.c $B/
[ -n "$RFU_SRC" ] && cp "$RFU_SRC" $B/rfu.c
cp mock/*.h psp_mock.c poke_rfu_sim.c $B/
gcc -O1 -g -w -I$B -fsanitize=address,undefined -o $B/sim $B/poke_rfu_sim.c $B/psp_mock.c $B/link.c $B/link_rel.c $B/link_log.c $B/serial.c $B/serial_proto.c $B/rfu.c
set +e
export LINK_LOSS=${2:-0} LINK_DELAY_US=${3:-3000} LINK_JITTER_US=${4:-0} LINK_PORTBASE=${5:-47100} LINK_RATE=${LINK_RATE:-0} LINK_STALL_MS=${LINK_STALL_MS:-0} LINK_STALL_EVERY_MS=${LINK_STALL_EVERY_MS:-0}
mkdir -p $B/host $B/guest
SIM_FPS=${SIM_FPS_HOST:-60} LINK_LOGDIR=$B/host LINK_ROLE=host  $B/sim "$1" > $B/host.log 2>&1 &
HP=$!
sleep 0.5
SIM_FPS=${SIM_FPS_GUEST:-60} LINK_LOGDIR=$B/guest LINK_ROLE=guest $B/sim "$1" > $B/guest.log 2>&1 &
GP=$!
wait $HP; HR=$?
wait $GP; GR=$?
cat $B/host.log $B/guest.log
mkdir -p logs_out && cp $B/host/link_log.txt logs_out/${OUTNAME:-poke}_host.txt 2>/dev/null; cp $B/guest/link_log.txt logs_out/${OUTNAME:-poke}_guest.txt 2>/dev/null
exit $(( HR | GR ))
