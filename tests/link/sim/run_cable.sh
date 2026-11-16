#!/bin/bash
# Builds the real FrogGBA link sources against the mocks and runs a host and a
# guest process. Usage: ./run_cable.sh <host_code> <host_title> <guest_code> <guest_title> <seconds> [loss] [delay_us] [jitter_us] [portbase]
set -e
cd "$(dirname "$0")"
SRC="$(cd ../../../source/src && pwd)"
B=$(mktemp -d)
cp $SRC/link.c $SRC/link.h $SRC/link_rel.c $SRC/link_rel.h $SRC/link_log.c $SRC/link_log.h $SRC/serial.c $SRC/serial.h $SRC/serial_proto.c $SRC/rfu.c $B/
cp mock/*.h psp_mock.c gba_sim.c $B/
gcc -O1 -g -w -I$B -fsanitize=address,undefined -o $B/gba_sim $B/gba_sim.c $B/psp_mock.c $B/link.c $B/link_rel.c $B/link_log.c $B/serial.c $B/serial_proto.c $B/rfu.c
set +e
export LINK_LOSS=${6:-0} LINK_DELAY_US=${7:-3000} LINK_JITTER_US=${8:-0} LINK_PORTBASE=${9:-47000} LINK_RATE=${LINK_RATE:-0} LINK_STALL_MS=${LINK_STALL_MS:-0} LINK_STALL_EVERY_MS=${LINK_STALL_EVERY_MS:-0}
mkdir -p $B/host $B/guest
LINK_LOGDIR=$B/host LINK_ROLE=host  $B/gba_sim "$1" "$2" "$5" > $B/host.log 2>&1 &
HP=$!
sleep 0.5
LINK_LOGDIR=$B/guest LINK_ROLE=guest $B/gba_sim "$3" "$4" "$5" > $B/guest.log 2>&1 &
GP=$!
wait $HP; HR=$?
wait $GP; GR=$?
cat $B/host.log $B/guest.log
mkdir -p logs_out && cp $B/host/link_log.txt logs_out/${OUTNAME:-run}_host.txt 2>/dev/null; cp $B/guest/link_log.txt logs_out/${OUTNAME:-run}_guest.txt 2>/dev/null
rm -rf $B
exit $(( HR | GR ))
