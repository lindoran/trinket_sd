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
cd "$ROOT_DIR"
mkdir -p "$DIST_DIR"
cp "$BUILD_DIR/trinket_sd.uf2" "$DIST_DIR/trinket_sd.uf2"

printf '\nBuilt firmware: %s\n' "$DIST_DIR/trinket_sd.uf2"

# The DOS-side diagnostic clients (SDTEST.EXE/IFTEST.EXE, firmware/client/)
# used to build here too, via ia16-elf-gcc. As of 2026-09-08 they build
# via Borland C++ 3.1 under dosbox instead -- see build-client.sh and
# TOOLCHAIN.md for why (ia16-elf-gcc has no viable fresh-sandbox install
# path; BC31 is already vendored, proven, and now this project's one DOS
# toolchain). Run build-client.sh (or build.sh, which runs both) to get
# them.