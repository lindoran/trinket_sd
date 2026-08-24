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
    if [ -z "$picotool" ]; then
        printf 'picotool is missing. Install it or put it on PATH.\n' >&2
        exit 1
    fi
    ln -sfn "$picotool" "$PICOTOOL_PATH"
fi

printf 'Firmware tools are ready under %s\n' "$SDK_ROOT"
