#!/usr/bin/env bash
set -eu

ROOT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
FIRMWARE_DIR="$ROOT_DIR/firmware"
BUILD_DIR="$FIRMWARE_DIR/build"
DIST_DIR="$ROOT_DIR/dist"
SDK_PATH=${PICO_SDK_PATH:-"$HOME/.pico-sdk/sdk/2.3.0"}
BOARD=${PICO_BOARD:-pico}

"$ROOT_DIR/setup-firmware.sh"

cmake -S "$FIRMWARE_DIR" -B "$BUILD_DIR" \
    -DPICO_SDK_PATH="$SDK_PATH" \
    -DPICO_BOARD="$BOARD"
cmake --build "$BUILD_DIR" -j"$(getconf _NPROCESSORS_ONLN 2>/dev/null || printf '1')"
cd "./firmware/client" && make
cd "$ROOT_DIR"
cp "$BUILD_DIR/trinket_sd.uf2" "$DIST_DIR/trinket_sd.uf2"
cp "$FIRMWARE_DIR/client/SDTEST.EXE" "$DIST_DIR/"
cp "$FIRMWARE_DIR/client/IFTEST.EXE" "$DIST_DIR/"

printf '\nBuilt firmware: %s\n' "$DIST_DIR/trinket_sd.uf2"
printf 'Built client: %s\n' "$DIST_DIR/SDTEST.EXE"
printf 'Built client: %s\n' "$DIST_DIR/IFTEST.EXE"