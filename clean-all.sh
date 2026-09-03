#!/usr/bin/env bash
# Runs both of this repo's clean scripts. See build.sh for the build-side
# equivalent.
set -eu

ROOT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)

"$ROOT_DIR/clean-firmware.sh"
"$ROOT_DIR/clean-driver.sh"
