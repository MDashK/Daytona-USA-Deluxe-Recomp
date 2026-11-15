#!/bin/bash
# Build the PS Vita homebrew. Run inside WSL.
# Uses the softfp VitaSDK (the one with VitaGL installed).
set -e

export VITASDK="${VITASDK:-/home/mdashk/vitasdk-softfp}"
cd "$(dirname "$0")"

cmake -DCMAKE_TOOLCHAIN_FILE="$VITASDK/share/vita.toolchain.cmake" -B build .
cmake --build build

echo
echo "Built build/daytona_vita.elf"
