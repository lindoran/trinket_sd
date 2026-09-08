#!/usr/bin/env bash
# Read-only dependency check for every build and packaging target.
set -u

ROOT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
SDK_ROOT=${PICO_SDK_HOME:-"$HOME/.pico-sdk"}
SDK_PATH=${PICO_SDK_PATH:-"$SDK_ROOT/sdk/2.3.0"}
TOOLCHAIN_PATH="$SDK_ROOT/toolchain/15_2_Rel1"
PICOTOOL_PATH="$SDK_ROOT/picotool/2.3.0/picotool"
BC31_DIR="$ROOT_DIR/legacy/bc31"

missing=0

command_check() {
    if command -v "$1" >/dev/null 2>&1; then
        printf '  [ok]      %s\n' "$1"
    else
        printf '  [missing] %s\n' "$1"
        missing=$((missing + 1))
    fi
}

file_check() {
    if [ -e "$1" ]; then
        printf '  [ok]      %s\n' "$2"
    else
        printf '  [missing] %s (%s)\n' "$2" "$1"
        missing=$((missing + 1))
    fi
}

printf 'Common build and packaging tools:\n'
for tool in bash make cmake git getconf mktemp cp rm grep timeout tar 7z zip; do
    command_check "$tool"
done

printf '\nRP2040 firmware toolchain:\n'
command_check arm-none-eabi-gcc
file_check "$SDK_PATH/pico_sdk_init.cmake" "Pico SDK 2.3.0"
file_check "$PICOTOOL_PATH" "picotool 2.3.0"

printf '\nDOS toolchain (driver + diagnostic clients, Borland C++ 3.1 under dosbox):\n'
command_check dosbox
file_check "$BC31_DIR" "Borland C++ 3.1 directory"
for tool in BCC.EXE MAKE.EXE TLINK.EXE TASM.EXE; do
    file_check "$BC31_DIR/BIN/$tool" "Borland $tool"
done
file_check "$BC31_DIR/INCLUDE" "Borland INCLUDE directory"
file_check "$BC31_DIR/LIB" "Borland LIB directory"

printf '\n'
if [ "$missing" -eq 0 ]; then
    printf 'All dependencies required by the build and package targets are available.\n'
    exit 0
fi

printf '%d dependency check(s) failed. Suggested next steps:\n' "$missing"
printf '  Firmware: install gcc-arm-none-eabi, cmake, git, and picotool; or set PICO_SDK_PATH to a Pico SDK 2.3.0 checkout.\n'
printf '  DOS toolchain (driver + clients): install dosbox and provide a legally obtained Borland C++ 3.1 copy under legacy/bc31/.\n'
printf '  Packaging: install tar, 7z (p7zip), and zip.\n'
exit 1
