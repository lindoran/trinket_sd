#!/usr/bin/env bash
# Builds the DOS-side diagnostic clients (SDTEST.EXE, IFTEST.EXE) via
# the vendored Borland C++ 3.1 toolchain (legacy/bc31) inside dosbox,
# headless -- same toolchain, same approach as build-driver.sh (see
# that script and TOOLCHAIN.md for why dosbox+BC31 is this project's
# one DOS-side toolchain now, client tools included as of 2026-09-08).
set -eu

ROOT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
CLIENT_DIR="$ROOT_DIR/firmware/client"
BC31_DIR="$ROOT_DIR/legacy/bc31"
DIST_DIR="$ROOT_DIR/dist"
WORK_DIR=$(mktemp -d)
trap 'rm -rf -- "$WORK_DIR"' EXIT

if ! command -v dosbox >/dev/null 2>&1; then
    printf 'error: dosbox not found on PATH.\n' >&2
    printf '       apt-get install dosbox (or your distro equivalent) -- see TOOLCHAIN.md.\n' >&2
    exit 1
fi

if [ ! -d "$BC31_DIR" ]; then
    printf 'error: vendored Borland C++ 3.1 not found at %s\n' "$BC31_DIR" >&2
    printf '       see TOOLCHAIN.md.\n' >&2
    exit 1
fi

if [ ! -f "$BC31_DIR/BIN/MAKE.EXE" ]; then
    printf 'error: %s exists but has no BIN/MAKE.EXE (and presumably no other\n' "$BC31_DIR" >&2
    printf '       Borland binaries either) -- this directory is gitignored\n' >&2
    printf '       (legacy/bc31/* in .gitignore) precisely because Borland C++\n' >&2
    printf '       3.1 cannot be redistributed in this repo. A plain git clone\n' >&2
    printf '       or zip export of this project will always land here --\n' >&2
    printf '       that is expected, not a broken checkout. You need to supply\n' >&2
    printf '       your own legally-obtained copy of Borland C++ 3.1 (BIN/,\n' >&2
    printf '       INCLUDE/, LIB/ at minimum) at that path yourself. See\n' >&2
    printf '       TOOLCHAIN.md for exactly what this script expects to find.\n' >&2
    exit 1
fi

# Same scratch-drive approach as build-driver.sh, same reasoning (keep
# a failed/partial build from leaving generated files in the checkout,
# give dosbox real host directories for its C:/D: drives). Source
# filenames here are already DOS-8.3-safe (sdtest.c, iftest.c,
# dactest.c) -- see TOOLCHAIN.md's note on why iftest.c is named that
# and not ifacetest.c.
mkdir -p "$WORK_DIR/c" "$WORK_DIR/d/TEMP"
cp -r "$BC31_DIR" "$WORK_DIR/c/BC31"
cp "$CLIENT_DIR"/*.c "$CLIENT_DIR/MAKEFILE" "$WORK_DIR/d/"

cat > "$WORK_DIR/build.conf" << EOF
[autoexec]
mount c $WORK_DIR/c
mount d $WORK_DIR/d
set include=c:\bc31\include\\
set lib=c:\bc31\lib
set temp=c:\temp
set path=c:\;c:\bc31\bin
d:
cd \\
make -f MAKEFILE > LOG.TXT
echo BUILD_DONE_MARKER >> LOG.TXT
exit
EOF

SDL_VIDEODRIVER=dummy timeout 120 dosbox -conf "$WORK_DIR/build.conf" -exit >/dev/null 2>&1 || true

LOG="$WORK_DIR/d/LOG.TXT"
if [ ! -f "$LOG" ] || ! grep -q BUILD_DONE_MARKER "$LOG"; then
    printf 'error: dosbox did not finish the build (timed out or crashed).\n' >&2
    printf '       log so far:\n' >&2
    cat "$LOG" >&2 2>/dev/null || true
    exit 1
fi

# NOTE: matches a line *starting* with "Error " -- bcc's own compile
# errors look like "Error dactest.c 4: message" (filename right after
# "Error ", not a catalog code), which the previous pattern here
# ('Error [A-Z][0-9]', presumably written expecting an assembler-style
# "Error A2081" catalog code) never actually matched. Found this the
# hard way: this script reported a clean build while silently never
# having checked whether DACTEST.EXE existed at all.
#
# The `grep -v '^Error messages:'` second stage exists because TASM's
# own clean-build summary line is literally "Error messages:    None"
# -- a naive '^Error ' match flags that as a failure on every driver
# build. Filter it out, then anything starting with "Error " that's
# left is a real one. Same two-stage check in build-driver.sh.
if grep -E '^Error ' "$LOG" | grep -qv '^Error messages:'; then
    printf 'warning: a real error marker appears in the build log:\n' >&2
    cat "$LOG" >&2
elif grep -qE 'Fatal|Illegal command' "$LOG"; then
    printf 'warning: a real fatal/illegal-command marker appears in the build log:\n' >&2
    cat "$LOG" >&2
fi

if [ ! -f "$WORK_DIR/d/SDTEST.EXE" ] || [ ! -f "$WORK_DIR/d/IFTEST.EXE" ] || [ ! -f "$WORK_DIR/d/DACTEST.EXE" ] || [ ! -f "$WORK_DIR/d/DSSTEST.EXE" ] || [ ! -f "$WORK_DIR/d/DSSSTRES.EXE" ] || [ ! -f "$WORK_DIR/d/DSSCFG.EXE" ]; then
    printf 'error: SDTEST.EXE, IFTEST.EXE, DACTEST.EXE, DSSTEST.EXE, DSSSTRES.EXE, and/or DSSCFG.EXE were not produced. Full log:\n' >&2
    cat "$LOG" >&2
    exit 1
fi

mkdir -p "$DIST_DIR"
cp "$WORK_DIR/d/SDTEST.EXE" "$DIST_DIR/SDTEST.EXE"
cp "$WORK_DIR/d/IFTEST.EXE" "$DIST_DIR/IFTEST.EXE"
cp "$WORK_DIR/d/DACTEST.EXE" "$DIST_DIR/DACTEST.EXE"
cp "$WORK_DIR/d/DSSTEST.EXE" "$DIST_DIR/DSSTEST.EXE"
cp "$WORK_DIR/d/DSSSTRES.EXE" "$DIST_DIR/DSSSTRES.EXE"
cp "$WORK_DIR/d/DSSCFG.EXE" "$DIST_DIR/DSSCFG.EXE"
printf '\nBuilt client: %s\n' "$DIST_DIR/SDTEST.EXE"
printf 'Built client: %s\n' "$DIST_DIR/IFTEST.EXE"
printf 'Built client: %s\n' "$DIST_DIR/DACTEST.EXE"
printf 'Built client: %s\n' "$DIST_DIR/DSSTEST.EXE"
printf 'Built client: %s\n' "$DIST_DIR/DSSSTRES.EXE"
printf 'Built client: %s\n' "$DIST_DIR/DSSCFG.EXE"
