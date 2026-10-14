#!/usr/bin/env bash
# Builds ClaudeBoy's engine into dist/claudeboy-engine: mGBA's Game Boy core,
# fetched once at a pinned version and checked against its checksum, with
# the engine (engine/*.c) on top. Needs curl (the first time), cmake, make, tar
# and a C compiler; everything it makes stays in this folder.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
MGBA_VERSION=0.10.5
MGBA_SHA256=91d6fbd32abcbdf030d58d3f562de25ebbc9d56040d513ff8e5c19bee9dacf14
BUILD="$ROOT/.build"
SOURCE="$BUILD/mgba-$MGBA_VERSION"
LIBRARY="$BUILD/mgba-lib"

fail() {
  echo "$1" >&2
  exit 1
}

for tool in cmake make cc tar; do
  command -v "$tool" >/dev/null || fail "ClaudeBoy's build needs $tool"
done

sha256() {
  if command -v sha256sum >/dev/null; then sha256sum "$1" | cut -d' ' -f1; else shasum -a 256 "$1" | cut -d' ' -f1; fi
}

mkdir -p "$BUILD" "$ROOT/dist"
# Unpacked whole, or not at all: a stamp written last
if [ ! -f "$SOURCE/.unpacked" ]; then
  archive="$BUILD/mgba-$MGBA_VERSION.tar.gz"
  if [ ! -f "$archive" ]; then
    command -v curl >/dev/null || fail "ClaudeBoy's build needs curl to fetch mGBA"
    echo "Fetching mGBA $MGBA_VERSION…"
    curl -fsSL --retry 2 -o "$archive.part" "https://github.com/mgba-emu/mgba/archive/refs/tags/$MGBA_VERSION.tar.gz"
    mv "$archive.part" "$archive"
  fi
  if [ "$(sha256 "$archive")" != "$MGBA_SHA256" ]; then
    rm -f "$archive"
    fail "mGBA's archive doesn't match its checksum; run this again to fetch it anew"
  fi
  rm -rf "$SOURCE"
  tar -xzf "$archive" -C "$BUILD"
  touch "$SOURCE/.unpacked"
fi

jobs="$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)"

echo "Building mGBA's Game Boy core…"
log="$BUILD/mgba-build.log"
if ! { cmake -S "$SOURCE" -B "$LIBRARY" \
  -DLIBMGBA_ONLY=ON -DM_CORE_GBA=OFF -DM_CORE_GB=ON -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_POLICY_VERSION_MINIMUM=3.5 \
  -DUSE_DEBUGGERS=OFF -DENABLE_SCRIPTING=OFF -DUSE_GDB_STUB=OFF -DBUILD_LTO=OFF &&
  cmake --build "$LIBRARY" -j "$jobs"; } >"$log" 2>&1; then
  tail -n 20 "$log" >&2
  fail "mGBA's core didn't build; the whole log is in $log"
fi

echo "Building the engine…"
extra=()
# glibc before 2.34 keeps shm_open in librt, and dlopen in libdl
if [ "$(uname)" = Linux ]; then extra+=(-lrt -ldl); fi
cc -O2 -std=gnu11 -Wall -Wno-format-truncation \
  -I"$LIBRARY/include" -I"$SOURCE/include" \
  "$ROOT/engine/claudeboy-engine.c" "$ROOT/engine/window.c" "$ROOT/engine/console.c" "$ROOT/engine/frontmost.c" "$LIBRARY/libmgba.a" -lm -lpthread "${extra[@]}" \
  -o "$ROOT/dist/claudeboy-engine.part"
mv "$ROOT/dist/claudeboy-engine.part" "$ROOT/dist/claudeboy-engine"
echo "Built $ROOT/dist/claudeboy-engine"
