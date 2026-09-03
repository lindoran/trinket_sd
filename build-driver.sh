#!/usr/bin/env bash
# Builds TRINKET.SYS via the vendored Borland C++ 3.1 toolchain
# (legacy/bc31) inside dosbox, headless. See TOOLCHAIN.md for why
# dosbox is the only DOS layer this script supports today, and what
# would be involved in adding another one.
set -eu

ROOT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
SRC_DIR="$ROOT_DIR/src"
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

# dosbox wants its own C:/D: drive roots as real host directories, and
# make -f MAKEFILE writes .OBJ/.SYS output alongside the sources -- so
# both drives are built fresh in a scratch dir rather than mounting the
# real src/ directly (keeps a failed or partial build from leaving
# generated files in the checkout).
mkdir -p "$WORK_DIR/c" "$WORK_DIR/d/TEMP"
cp -r "$BC31_DIR" "$WORK_DIR/c/BC31"
cp "$SRC_DIR"/*.C "$SRC_DIR"/*.H "$SRC_DIR"/*.ASM "$SRC_DIR/MAKEFILE" "$WORK_DIR/d/"

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

# -exit closes dosbox once the autoexec section finishes; SDL_VIDEODRIVER=dummy
# keeps it from trying to open a real display. 120s is generous headroom --
# a clean build of this project's driver takes a few seconds inside dosbox.
SDL_VIDEODRIVER=dummy timeout 120 dosbox -conf "$WORK_DIR/build.conf" -exit >/dev/null 2>&1 || true

LOG="$WORK_DIR/d/LOG.TXT"
if [ ! -f "$LOG" ] || ! grep -q BUILD_DONE_MARKER "$LOG"; then
    printf 'error: dosbox did not finish the build (timed out or crashed).\n' >&2
    printf '       log so far:\n' >&2
    cat "$LOG" >&2 2>/dev/null || true
    exit 1
fi

if grep -qE 'Error [A-Z][0-9]|Fatal|Illegal command' "$LOG"; then
    printf 'warning: a real error/fatal marker appears in the build log:\n' >&2
    cat "$LOG" >&2
fi

if [ ! -f "$WORK_DIR/d/TRINKET.SYS" ]; then
    printf 'error: TRINKET.SYS was not produced. Full log:\n' >&2
    cat "$LOG" >&2
    exit 1
fi

mkdir -p "$DIST_DIR"
cp "$WORK_DIR/d/TRINKET.SYS" "$DIST_DIR/TRINKET.SYS"
printf '\nBuilt driver: %s\n' "$DIST_DIR/TRINKET.SYS"
