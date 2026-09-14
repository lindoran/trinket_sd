#!/usr/bin/env bash
set -eu

ROOT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
SDK_ROOT=${PICO_SDK_HOME:-"$HOME/.pico-sdk"}
SDK_VERSION=2.3.0
TOOLCHAIN_VERSION=15_2_Rel1
PICOTOOL_VERSION=2.3.0
SDK_PATH=${PICO_SDK_PATH:-"$SDK_ROOT/sdk/$SDK_VERSION"}
TOOLCHAIN_PATH="$SDK_ROOT/toolchain/$TOOLCHAIN_VERSION"
PICOTOOL_PATH="$SDK_ROOT/picotool/$PICOTOOL_VERSION/picotool"

mkdir -p "$SDK_ROOT/sdk" "$SDK_ROOT/toolchain" "$SDK_ROOT/picotool/$PICOTOOL_VERSION"

if [ ! -f "$SDK_PATH/pico_sdk_init.cmake" ]; then
    if [ -n "${PICO_SDK_PATH:-}" ]; then
        printf 'Configured Pico SDK is missing: %s\n' "$SDK_PATH" >&2
        exit 1
    fi
    printf 'Installing Pico SDK %s...\n' "$SDK_VERSION"
    git clone --depth 1 --branch "$SDK_VERSION" --recurse-submodules \
        https://github.com/raspberrypi/pico-sdk.git "$SDK_PATH"
fi

if [ ! -x "$TOOLCHAIN_PATH/bin/arm-none-eabi-gcc" ]; then
    compiler=$(command -v arm-none-eabi-gcc || true)
    if [ -z "$compiler" ]; then
        printf 'ARM toolchain is missing (arm-none-eabi-gcc).\n' >&2
        exit 1
    fi
    printf 'Linking the installed ARM toolchain into %s...\n' "$TOOLCHAIN_PATH"
    mkdir -p "$TOOLCHAIN_PATH/bin"
    for tool in /usr/bin/arm-none-eabi-*; do
        [ -e "$tool" ] || continue
        ln -sfn "$tool" "$TOOLCHAIN_PATH/bin/$(basename "$tool")"
    done
fi

if [ ! -x "$PICOTOOL_PATH" ]; then
    picotool=$(command -v picotool || true)
    if [ -n "$picotool" ]; then
        ln -sfn "$picotool" "$PICOTOOL_PATH"
    else
        # Not fatal: picotool isn't apt-installable on a stock Ubuntu
        # sandbox (confirmed 2026-09-09 -- no such package), but the
        # Pico SDK's own CMakeLists.txt (tools/CMakeLists.txt's
        # pico_init_picotool) auto-fetches and builds picotool from
        # source via FetchContent the first time `cmake -S firmware -B
        # firmware/build ...` runs, if it can't find one already
        # installed. Confirmed working end-to-end this way. Hard-
        # failing the whole setup script here over this was blocking
        # every firmware build in a fresh sandbox for no real reason.
        printf 'picotool not found on PATH -- cmake will fetch and build it from source on first configure (this is expected and fine).\n'
    fi
fi

printf 'Firmware tools are ready under %s\n' "$SDK_ROOT"
