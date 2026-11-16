#!/bin/bash
# Unit test of the reliable channel (source/src/link_rel.c) over a simulated
# lossy, reordering, rate-limited datagram link. Usage: ./run.sh
set -e
cd "$(dirname "$0")"
SRC="$(cd ../../../source/src && pwd)"
B=$(mktemp -d)
gcc -O1 -g -w -I"$SRC" -fsanitize=address,undefined -o "$B/test_link_rel" test_link_rel.c "$SRC/link_rel.c"
"$B/test_link_rel"
