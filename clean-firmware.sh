#!/usr/bin/env bash
set -eu

ROOT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
BUILD_DIR="$ROOT_DIR/firmware/build"
DIST_DIR="$ROOT_DIR/dist"

removed=0
if [ -d "$BUILD_DIR" ]; then
    rm -rf -- "$BUILD_DIR"
    printf 'Removed %s\n' "$BUILD_DIR"
    removed=1
fi
if [ -f "$DIST_DIR/trinket_sd.uf2" ]; then
    rm -f -- "$DIST_DIR/trinket_sd.uf2"
    printf 'Removed %s\n' "$DIST_DIR/trinket_sd.uf2"
    removed=1
fi
if [ "$removed" -eq 0 ]; then
    printf 'Nothing to clean.\n'
fi
