#!/usr/bin/env bash
# Build and run the stop_id gate tests on the host. No board, no Zephyr
# build, no network. Runnable from anywhere:  ./tests/stop_id/run.sh
set -uo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$HERE/../.." && pwd)"
SRC="$REPO/app/src"
CONF="$REPO/app/sign.conf"
BUILD="$HERE/.build"

# CONFIG_STOP_ID comes out of sign.conf for the same reason the parser
# harness reads its sizing knobs from there: a copy in this file would
# drift, and the seed-vs-synced distinction under test is about the real
# seed the firmware ships with, not a made-up one.
STOP_ID="$(sed -n 's/^CONFIG_STOP_ID="\(.*\)"$/\1/p' "$CONF" | head -1)"
if [ -z "$STOP_ID" ]; then
  echo "FATAL: CONFIG_STOP_ID not found in $CONF" >&2
  exit 2
fi

echo "config from app/sign.conf: STOP_ID=\"$STOP_ID\""

mkdir -p "$BUILD"

if ! gcc -std=gnu11 -Wall -I"$HERE" -I"$SRC" -DCONFIG_STOP_ID="\"$STOP_ID\"" \
    -o "$BUILD/sync" "$HERE/test_sync.c" "$SRC/stop_id.c" \
    > "$BUILD/sync.buildlog" 2>&1; then
  echo "FATAL: failed to build the sync tests" >&2
  cat "$BUILD/sync.buildlog" >&2
  exit 2
fi

# The display mapping carries the same seed-vs-synced distinction as the
# stop id and gates the same boot behaviour, so it is exercised here rather
# than in a harness of its own.
if ! gcc -std=gnu11 -Wall -I"$HERE" -I"$SRC" \
    -o "$BUILD/map" "$HERE/test_map_sync.c" "$SRC/display_map.c" \
    > "$BUILD/map.buildlog" 2>&1; then
  echo "FATAL: failed to build the display map tests" >&2
  cat "$BUILD/map.buildlog" >&2
  exit 2
fi

rc=0

echo
echo "== seed-vs-synced gate cases =="
"$BUILD/sync" || rc=1

echo
echo "== display map seed-vs-synced cases =="
"$BUILD/map" || rc=1

echo
if [ "$rc" -eq 0 ]; then
  echo "PASS: all stop_id tests green"
else
  echo "FAIL: see above"
  exit 1
fi
