#!/usr/bin/env bash
set -eu

ROOT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
OUT="$ROOT_DIR/dist/TRINKET.SYS"

if [ -f "$OUT" ]; then
    rm -f -- "$OUT"
    printf 'Removed %s\n' "$OUT"
else
    printf 'Nothing to clean.\n'
fi
