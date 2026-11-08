# Top-level wrapper for the project build chain.
#
# The individual scripts remain the source of truth for each toolchain; these
# targets make them discoverable through the conventional `make` interface.

ROOT_DIR := $(abspath $(dir $(lastword $(MAKEFILE_LIST))))
PACKAGE_BASENAME := trinket_sd
PACKAGE_DIR := $(ROOT_DIR)/packages
PACKAGE_FILES := TRINKET.SYS SDTEST.EXE IFTEST.EXE DACTEST.EXE DSSTEST.EXE DSSSTRES.EXE DSSCFG.EXE trinket_sd.uf2
SOURCE_DIR := $(notdir $(ROOT_DIR))
SOURCE_ARCHIVE := $(PACKAGE_DIR)/$(PACKAGE_BASENAME)-source.tar.gz

.PHONY: all build package toolchain check-toolchain setup setup-firmware firmware build-firmware client build-client driver build-driver clean clean-all clean-firmware clean-driver clean-client help

all: build

build:
	@"$(ROOT_DIR)/build.sh"

package: build
	@mkdir -p "$(PACKAGE_DIR)"
	@rm -f -- \
		"$(PACKAGE_DIR)/$(PACKAGE_BASENAME).tar.gz" \
		"$(PACKAGE_DIR)/$(PACKAGE_BASENAME).7z" \
		"$(PACKAGE_DIR)/$(PACKAGE_BASENAME).zip" \
		"$(SOURCE_ARCHIVE)"
	@cd "$(ROOT_DIR)/dist" && \
		tar -czf "$(PACKAGE_DIR)/$(PACKAGE_BASENAME).tar.gz" $(PACKAGE_FILES) && \
		7z a -t7z "$(PACKAGE_DIR)/$(PACKAGE_BASENAME).7z" $(PACKAGE_FILES) >/dev/null && \
		zip -q "$(PACKAGE_DIR)/$(PACKAGE_BASENAME).zip" $(PACKAGE_FILES)
	@tar -czf "$(SOURCE_ARCHIVE)" \
		--exclude="$(SOURCE_DIR)/.git" \
		--exclude="$(SOURCE_DIR)/dist" \
		--exclude="$(SOURCE_DIR)/packages" \
		--exclude="$(SOURCE_DIR)/firmware/build" \
		--exclude="$(SOURCE_DIR)/legacy/bc31" \
		--exclude="$(SOURCE_DIR)/legacy/BC31" \
		--exclude='*.EXE' \
		--exclude='*.SYS' \
		--exclude='*.uf2' \
		--exclude='*.elf' \
		--exclude='*.elf.map' \
		--exclude='*.bin' \
		--exclude='*.hex' \
		--exclude='*.dis' \
		-C "$(dir $(ROOT_DIR))" "$(SOURCE_DIR)"
	@printf '%s\n' \
		"Packaged artifacts: $(PACKAGE_DIR)/$(PACKAGE_BASENAME).tar.gz" \
		"                    $(PACKAGE_DIR)/$(PACKAGE_BASENAME).7z" \
		"                    $(PACKAGE_DIR)/$(PACKAGE_BASENAME).zip" \
		"Packaged source:    $(SOURCE_ARCHIVE)"

toolchain: check-toolchain

check-toolchain:
	@"$(ROOT_DIR)/check-toolchain.sh"

setup:
	@"$(ROOT_DIR)/setup-firmware.sh"

setup-firmware: setup

firmware:
	@"$(ROOT_DIR)/build-firmware.sh"

build-firmware: firmware

client:
	@"$(ROOT_DIR)/build-client.sh"

build-client: client

driver:
	@"$(ROOT_DIR)/build-driver.sh"

build-driver: driver

clean: clean-all

clean-all:
	@"$(ROOT_DIR)/clean-all.sh"

clean-firmware:
	@"$(ROOT_DIR)/clean-firmware.sh"

clean-driver:
	@"$(ROOT_DIR)/clean-driver.sh"

clean-client:
	@"$(ROOT_DIR)/clean-client.sh"

help:
	@printf '%s\n' \
		'Usage: make [target]' \
		'' \
		'Build targets:' \
		'  all, build       Build firmware, DOS driver, and DOS client tools' \
		'  package          Build everything and create release + source archives in packages/' \
		'  toolchain        Check dependencies and print setup suggestions' \
		'  setup            Install or link firmware build dependencies' \
		'  firmware         Build RP2040 firmware only' \
		'  client           Build only the DOS client tools (Borland C++ 3.1 + dosbox)' \
		'  driver           Build only the DOS driver (Borland C++ 3.1 + dosbox)' \
		'  setup-firmware, build-firmware, build-client, build-driver' \
		'                   Script-name aliases for the targets above' \
		'' \
		'Clean targets:' \
		'  clean, clean-all  Remove all generated build artifacts' \
		'  clean-firmware    Remove firmware artifacts' \
		'  clean-driver      Remove the DOS driver artifact' \
		'  clean-client      Remove the DOS client tool artifacts'
