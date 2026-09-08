#!/usr/bin/env bash
set -eu

ROOT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)

removed=0
for f in "$ROOT_DIR/dist/SDTEST.EXE" "$ROOT_DIR/dist/IFTEST.EXE"; do
    if [ -f "$f" ]; then
        rm -f -- "$f"
        printf 'Removed %s\n' "$f"
        removed=1
    fi
done
if [ "$removed" -eq 0 ]; then
    printf 'Nothing to clean.\n'
fi
