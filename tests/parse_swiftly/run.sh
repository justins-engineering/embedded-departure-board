#!/usr/bin/env bash
# Build and run the Swiftly parser tests on the host. No board, no Zephyr
# build, no network. Runnable from anywhere:  ./tests/parse_swiftly/run.sh
set -uo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$HERE/../.." && pwd)"
SRC="$REPO/app/src"
CONF="$REPO/app/sign.conf"
FIXTURE="$HERE/fixtures/stop1670-20260814T024135Z.json"
BUILD="$HERE/.build"

# Read the sizing knobs out of sign.conf rather than repeating them here.
# They have to agree with the firmware or these tests exercise a differently
# shaped parser, and a copy in this file would drift the first time anyone
# retuned the real one. Absent means stop: a silently-missing value would
# leave the compiler default in place and quietly change the layout.
conf_val() {
  local key="$1" val
  val="$(sed -n "s/^${key}=\([0-9][0-9]*\).*/\1/p" "$CONF" | head -1)"
  if [ -z "$val" ]; then
    echo "FATAL: $key not found in $CONF" >&2
    exit 2
  fi
  printf '%s' "$val"
}

STOP_MAX_ROUTES="$(conf_val CONFIG_STOP_MAX_ROUTES)" || exit 2
ROUTE_MAX_DEPARTURES="$(conf_val CONFIG_ROUTE_MAX_DEPARTURES)" || exit 2
STOP_JSON_BUF_SIZE="$(conf_val CONFIG_STOP_JSON_BUF_SIZE)" || exit 2

echo "config from app/sign.conf: STOP_MAX_ROUTES=$STOP_MAX_ROUTES"\
" ROUTE_MAX_DEPARTURES=$ROUTE_MAX_DEPARTURES STOP_JSON_BUF_SIZE=$STOP_JSON_BUF_SIZE"

# -fshort-enums is load-bearing, not tidiness: it makes jsmntok_t 8 bytes on
# the host exactly as it is on ARM, which keeps parse_swiftly.c's own
# BUILD_ASSERT about that size meaningful. Without it the assert fails, and
# the tempting fix -- stubbing BUILD_ASSERT out -- would leave these tests
# passing against a parser laid out differently from the shipped one.

# -Wall but not -Wextra. The app sources carry a set of pre-existing -Wextra
# findings (signed/unsigned loop comparisons, qualifiers on return types)
# that the firmware build does not surface either; emitting them on every run
# buries the pass/fail line this script exists to produce. Raising the app to
# -Wextra clean is worth doing and is not this harness's job.
CFLAGS=(
  -std=gnu11 -Wall -fshort-enums
  -I"$HERE" -I"$SRC" -I"$SRC/json"
  -DCONFIG_STOP_MAX_ROUTES="$STOP_MAX_ROUTES"
  -DCONFIG_ROUTE_MAX_DEPARTURES="$ROUTE_MAX_DEPARTURES"
  -DCONFIG_STOP_JSON_BUF_SIZE="$STOP_JSON_BUF_SIZE"
)
PARSER=("$SRC/json/parse_swiftly.c" "$SRC/json/json_helpers.c" "$SRC/json/jsmn.c")

mkdir -p "$BUILD"
rc=0

# Compiler output is captured rather than streamed so the pass/fail line at
# the end is the first thing a reader sees. It is printed in full if a build
# fails, which is the only time it matters.
build() {  # build <output> <source> [extra flags...]
  local out="$1" src="$2"; shift 2
  if ! gcc "${CFLAGS[@]}" "$@" -o "$BUILD/$out" "$HERE/$src" "${PARSER[@]}" \
      > "$BUILD/$out.buildlog" 2>&1; then
    echo "FATAL: failed to build $out" >&2
    cat "$BUILD/$out.buildlog" >&2
    exit 2
  fi
}

build classify      test_parse.c -DFIXED
build classify_pre  test_parse.c            # caller-side behaviour before the fix
build match         test_match.c
build real          test_real.c

echo
echo "== classification and stale-state cases =="
"$BUILD/classify" || rc=1

echo
echo "== either-spelling matching, and the collision it makes reachable =="
"$BUILD/match" || rc=1

echo
echo "== the captured real end-of-service payload =="
"$BUILD/real" "$FIXTURE" || rc=1

echo
if [ "$rc" -eq 0 ]; then
  echo "PASS — all parse_swiftly tests green"
else
  echo "FAIL — see above"
fi
exit "$rc"
