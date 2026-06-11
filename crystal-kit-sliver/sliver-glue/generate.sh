#!/usr/bin/env bash
#
# generate.sh — wrap a post-ex DLL through Crystal Palace into a PICO .bin
#
# Required env:
#   CRYSTAL_PALACE_HOME  directory containing crystalpalace.jar AND the 'link' wrapper
#
# Usage:
#   ./generate.sh <input.dll> [args-file] [output.bin]
#
# Defaults:
#   args-file → empty file
#   output.bin → ./build/crystal-postex.x64.bin
#
# Verified CLI:
#   ./link <loader.spec> <file.dll|file.o> <out.bin> [A=hex] [%B=val] [@config.spec]

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
[[ -f "$SCRIPT_DIR/.crystalenv" ]] && source "$SCRIPT_DIR/.crystalenv"

: "${CRYSTAL_PALACE_HOME:?Set CRYSTAL_PALACE_HOME or create sliver-glue/.crystalenv with CRYSTAL_PALACE_HOME=<path>}"

INPUT_DLL="${1:?Usage: generate.sh <input.dll> [args-string-or-file] [output.bin]}"
ARGS_INPUT="${2:-}"
OUTPUT="${3:-./build/$(basename "$INPUT_DLL" .dll).pico.bin}"

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
POSTEX_DIR="$REPO_ROOT/postex-loader"
BUILD_DIR="$(dirname "$OUTPUT")"

mkdir -p "$BUILD_DIR"

# Resolve OUTPUT to absolute path BEFORE we cd into postex-loader
OUTPUT="$(cd "$BUILD_DIR" && pwd)/$(basename "$OUTPUT")"

if [[ -z "$ARGS_INPUT" ]]; then
    ARGS_FILE="$BUILD_DIR/empty.args"
    : > "$ARGS_FILE"
elif [[ -f "$ARGS_INPUT" ]]; then
    ARGS_FILE="$ARGS_INPUT"
else
    ARGS_FILE="$BUILD_DIR/args.tmp"
    printf '%s' "$ARGS_INPUT" > "$ARGS_FILE"
fi
ARGS_FILE_ABS="$(cd "$(dirname "$ARGS_FILE")" && pwd)/$(basename "$ARGS_FILE")"

INPUT_DLL_ABS="$(cd "$(dirname "$INPUT_DLL")" && pwd)/$(basename "$INPUT_DLL")"

if [[ ! -f "$INPUT_DLL_ABS" ]]; then
    echo "error: input DLL not found: $INPUT_DLL" >&2
    exit 2
fi

if [[ ! -x "$CRYSTAL_PALACE_HOME/link" ]]; then
    echo "error: 'link' wrapper not found or not executable at $CRYSTAL_PALACE_HOME/link" >&2
    exit 2
fi

if [[ ! -f "$REPO_ROOT/libtcg.x64.zip" ]]; then
    echo "error: libtcg.x64.zip missing at $REPO_ROOT/libtcg.x64.zip" >&2
    echo "       Copy it from the upstream Crystal-Kit repo. See docs/TOOLCHAIN.md." >&2
    exit 2
fi

echo "[*] Building postex-loader objects"
make -C "$POSTEX_DIR" all

echo "[*] Running Crystal Palace linker (./link) on postex-loader/loader.spec"
# Spec references 'bin/*.o' and '../libtcg.x64.zip' relative to its own dir,
# so we cd into postex-loader before invoking link.
cd "$POSTEX_DIR"
"$CRYSTAL_PALACE_HOME/link" \
    loader.spec \
    "$INPUT_DLL_ABS" \
    "$OUTPUT" \
    %ARGFILE="$ARGS_FILE_ABS"

echo "[+] Crystal-wrapped PICO at: $OUTPUT"
echo "[i] Next: feed this .bin to the crystal-loader Sliver Extension via 'crystal payload=$OUTPUT'"
