#!/usr/bin/env bash
set -eu

ROOT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
BUILD_DIR="$ROOT_DIR/firmware/build"

if [ -d "$BUILD_DIR" ]; then
    rm -rf -- "$BUILD_DIR"
    printf 'Removed %s\n' "$BUILD_DIR"
else
    printf 'Nothing to clean.\n'
fi
