#!/usr/bin/env bash
#
# pack-extension.sh — package the crystal-loader Sliver Extension as a tarball
# installable via 'extensions install' in the Sliver client.
#
# Usage:
#   ./pack-extension.sh [output.tar.gz]
#
# Expects crystal-loader.x64.dll (the runtime DLL wrapper around the PICO loader)
# to be present in this directory. That DLL is the step-6 deliverable and is NOT
# produced by generate.sh — generate.sh produces the PICO .bin that the wrapper
# consumes at runtime.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
OUTPUT="${1:-$SCRIPT_DIR/build/crystal-loader-0.1.0.tar.gz}"
BUILD_DIR="$(dirname "$OUTPUT")"

mkdir -p "$BUILD_DIR"

LOADER_DLL="$SCRIPT_DIR/crystal-loader.x64.dll"
EXEC_DLL="$SCRIPT_DIR/crystal-exec.x64.dll"
MANIFEST="$SCRIPT_DIR/extension.json"

if [[ ! -f "$LOADER_DLL" ]]; then
    echo "error: $LOADER_DLL not found." >&2
    exit 2
fi

if [[ ! -f "$EXEC_DLL" ]]; then
    echo "error: $EXEC_DLL not found." >&2
    echo "       Build it with:  cd crystal-exec && make" >&2
    exit 2
fi

if [[ ! -f "$MANIFEST" ]]; then
    echo "error: $MANIFEST not found." >&2
    exit 2
fi

echo "[*] Creating Sliver Extension tarball: $OUTPUT"
# Sliver's tarball reader expects entries with a leading ./ prefix
tar -C "$SCRIPT_DIR" -czf "$OUTPUT" \
    ./extension.json \
    ./crystal-loader.x64.dll \
    ./crystal-exec.x64.dll

echo "[+] Done. Install on Sliver client with:"
echo "    extensions install $OUTPUT"
