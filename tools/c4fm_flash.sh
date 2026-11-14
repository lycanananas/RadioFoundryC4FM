#!/bin/sh
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The RadioFoundry Authors
# Build the C4FM image for a DM-32UV, merge the radio's own codec, check it, and flash it through the radio's update
# loader (docs/testing/dm32-c4fm-bench.md 0). Run from the checkout's root.
#
#   tools/c4fm_flash.sh build              build + merge the codec, nothing touched
#   tools/c4fm_flash.sh flash [PORT]       the above, then flash (radio in update mode: PTT + SK1 at power-on)
#
# STOCK: the codec source, a stock firmware file or the program-memory.bin of a backup made on stock firmware; unset,
# the build's own merge from codecs/ is used. PORT default $RFOUNDRY_PORT, else /dev/ttyUSB0.
# CSKY: the toolchain's bin directory (default ~/toolchains/csky-elf/bin).
set -eu

STOCK=${STOCK:-}
CSKY=${CSKY:-$HOME/toolchains/csky-elf/bin}
OUT=build/dm32-c4fm
PATH="$CSKY:$PATH"
export PATH

[ -f Makefile ] && [ -d core/c4fm ] || { echo "run from the C4FM checkout's root" >&2; exit 2; }
[ -z "$STOCK" ] || [ -f "$STOCK" ] || { echo "codec source $STOCK not found" >&2; exit 2; }

make -j"$(nproc)" BOARD=dm32 VARIANT=c4fm >"$OUT.log" 2>&1 || { tail -30 "$OUT.log"; echo "build failed: $OUT.log" >&2; exit 1; }
grep -E "^ *(FLASH|RAM):" "$OUT.log" || true
if grep -q ": warning:" "$OUT.log"; then
	grep ": warning:" "$OUT.log"
	echo "build has warnings" >&2
fi
if [ -n "$STOCK" ]; then
	python3 tools/hrc7000_codec.py --board dm32 --stock "$STOCK" --image "$OUT/rfoundry_dm32.bin" -o "$OUT/rfoundry_dm32_codec.bin"
fi
[ -f "$OUT/rfoundry_dm32_codec.bin" ] || { echo "no codec merged: put the stock file in codecs/ or set STOCK=" >&2; exit 2; }
git log -1 --format='firmware: %h %s' 2>/dev/null || true
[ -z "$(git status --porcelain -- core drivers platform boards hal 2>/dev/null)" ] || echo "firmware: with uncommitted changes"

PORT=${2:-${RFOUNDRY_PORT:-/dev/ttyUSB0}}
case "${1:-build}" in
build)
	python3 tools/serial_loader.py --model dm32 "$OUT/rfoundry_dm32_codec.bin" --port "$PORT" --dry-run
	;;
flash)
	echo "Radio off, hold PTT + SK1, switch on (green LED), then press Enter."
	python3 tools/serial_loader.py --model dm32 "$OUT/rfoundry_dm32_codec.bin" --port "$PORT"
	;;
*)
	echo "usage: tools/c4fm_flash.sh build|flash [PORT]" >&2
	exit 2
	;;
esac
