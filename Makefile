# Host-side wrapper Makefile -- expected at the REPO ROOT, one level
# above legacy/ and src/, NOT inside src/TRINKET_SD/ alongside the
# Borland-toolchain MAKEFILE (that one is unchanged and still the only
# thing that actually knows how to build TRINKET.SYS -- this file's
# entire job is getting dosemu2 to run it non-interactively).
#
# Repo layout this assumes:
#   legacy/            Borland C++ 3.1 install which needs to be copied to
#                      ./legacy/bc31. The makefile assumes that you have
#                      a fairly standard dosemu2 setup, with a
#                      ~/.dosemu/drive_c/ directory that is your C: drive,
#                      which is where the legacy/ directory will be copied.
#   src/
#     BUILD.BAT        DOS-side driver script this file invokes
#     MAKEFILE         Borland-toolchain makefile (unchanged)
#     *.C *.H *.ASM
#
# src/ is passed via -K rather than -d so it's dosemu2's current drive
# when -E's command runs -- BUILD.BAT can then be invoked by bare name,
# no drive letter needed for it at all, only for legacy (see above).
#
# Usage from the repo root:
#   make             (normal build)
#   make clean       (passed through to Borland MAKE)

LEGACY_DIR := $(CURDIR)/legacy
SRC_DIR    := $(CURDIR)/src
DOSEMU_HOME ?= $(HOME)/.dosemu
DOSEMU_C_DRIVE ?= $(DOSEMU_HOME)/drive_c
DOS_TARGET ?=

.PHONY: all prepare-dosemu clean

all: prepare-dosemu
	dosemu  -quiet -t -K $(SRC_DIR) -E "BUILD.BAT $(DOS_TARGET)"
	@mkdir -p ./bin
	@mv -f ./src/*.sys ./bin/
	@mv -f ./src/*.map ./bin/
	@mv -f ./src/*.obj ./bin/

prepare-dosemu:
	@if [ ! -d "$(DOSEMU_HOME)" ]; then \
		echo "ERROR: dosemu2 home directory is missing: $(DOSEMU_HOME)"; \
		echo ""; \
		echo "Run dosemu once so dosemu2 can create its home directory, then rerun make."; \
		echo "If your dosemu2 home lives somewhere else, run:"; \
		echo "  make DOSEMU_HOME=/path/to/.dosemu"; \
		exit 1; \
	fi
	@mkdir -p "$(DOSEMU_C_DRIVE)"
	@cp -a --update=none "$(LEGACY_DIR)/." "$(DOSEMU_C_DRIVE)/"

clean:
	@rm -fr ./bin
