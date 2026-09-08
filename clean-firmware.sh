#!/usr/bin/env bash
set -eu

ROOT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
BUILD_DIR="$ROOT_DIR/firmware/build"
DIST_DIR="$ROOT_DIR/dist"

if [ -d "$BUILD_DIR" ]; then
    rm -rf -- "$BUILD_DIR"
    cd "$ROOT_DIR"
    rm -f -- "$DIST_DIR/trinket_sd.uf2"
    printf '\nCleaned build artifacts.\n'
    printf 'Removed %s\n' "$BUILD_DIR"
    printf 'Removed %s\n' "$DIST_DIR/trinket_sd.uf2"
    printf '(DOS client tool artifacts are clean-client.sh'"'"'s job now -- see TOOLCHAIN.md.)\n'
else
    printf 'Nothing to clean.\n'
fi
