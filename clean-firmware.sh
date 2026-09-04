#!/usr/bin/env bash
set -eu

ROOT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
BUILD_DIR="$ROOT_DIR/firmware/build"
DIST_DIR="$ROOT_DIR/dist"

if [ -d "$BUILD_DIR" ]; then
    rm -rf -- "$BUILD_DIR"
    cd "$ROOT_DIR/firmware/client" && make clean
    cd "$ROOT_DIR"
    rm -f -- "$DIST_DIR/trinket_sd.uf2" "$DIST_DIR/SDTEST.EXE" "$DIST_DIR/IFTEST.EXE"
    printf '\nCleaned build artifacts.\n'
    printf 'Removed %s\n' "$BUILD_DIR"
    printf 'Removed %s\n' "$DIST_DIR/trinket_sd.uf2"
    printf 'Removed %s\n' "$DIST_DIR/SDTEST.EXE"
    printf 'Removed %s\n' "$DIST_DIR/IFTEST.EXE"
else
    printf 'Nothing to clean.\n'
fi
