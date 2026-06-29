#!/usr/bin/env bash
# ─────────────────────────────────────────────────────────────────────────────
#  Flash the Cardputer firmware with the CORRECT flash size.
#
#  WHY THIS SCRIPT EXISTS:
#  m5stack esp32 core 3.3.7 with FQBN ...:PartitionScheme=default_8MB builds the
#  bootloader header for a 4MB flash (elf2image --flash-size 4MB, merge
#  --pad-to-size 4MB) but writes an 8MB *partition table*. Plain
#  `arduino-cli ... --upload` flashes with `--flash-size keep`, leaving the 4MB
#  header in place → the ROM loader sees an 8MB partition table on a "4MB" flash
#  → "partition X invalid ... exceeds flash chip size 0x400000" → boot loop →
#  black screen (the chip is fine, it just never runs the app).
#
#  FIX: compile, then flash via esptool with `--flash-size detect` so the
#  bootloader header is patched to the real 8MB → partition table is valid.
#
#  Usage:  cardputer/flash.sh [PORT]      (PORT auto-detected if omitted)
# ─────────────────────────────────────────────────────────────────────────────
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
export PATH="$PATH:$ROOT/bin"
# FQBN override via env, e.g.:  FQBN=m5stack:esp32:m5stack_atoms3r cardputer/flash.sh helmet/atoms3r_hud
FQBN="${FQBN:-m5stack:esp32:m5stack_cardputer:PartitionScheme=default_8MB}"
LIBS="$HOME/Documents/Arduino/libraries"
# $1 = sketch dir (default: the dashboard). e.g. cardputer/flash.sh cardputer/ble_relay
SKETCH="${1:-$ROOT/cardputer/vesc_dashboard}"
NAME="$(basename "$SKETCH")"
OUT="/tmp/vesc_build_$NAME"
PORT="${2:-$(ls /dev/cu.usbmodem* 2>/dev/null | head -1)}"
ET="$(ls "$HOME"/Library/Arduino15/packages/m5stack/tools/esptool_py/*/esptool 2>/dev/null | head -1)"
BA="$(ls "$HOME"/Library/Arduino15/packages/m5stack/hardware/esp32/*/tools/partitions/boot_app0.bin 2>/dev/null | head -1)"

[ -n "$PORT" ] || { echo "No /dev/cu.usbmodem* port found — is the Cardputer plugged in?"; exit 1; }
echo "Port: $PORT   Sketch: $NAME"
echo "Compiling…"
arduino-cli compile --fqbn "$FQBN" --libraries "$LIBS" --output-dir "$OUT" "$SKETCH"

B="$OUT/$NAME.ino"
echo "Flashing via esptool (--flash-size detect)…"
"$ET" --chip esp32s3 --port "$PORT" --before default_reset --after hard_reset write-flash \
  --flash-mode dio --flash-freq 80m --flash-size detect \
  0x0     "$B.bootloader.bin" \
  0x8000  "$B.partitions.bin" \
  0xe000  "$BA" \
  0x10000 "$B.bin"
echo "Done. Flashed with the real 8MB flash size — should boot the app, not loop."
