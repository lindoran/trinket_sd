# Top-level wrapper for the project build chain.
#
# The individual scripts remain the source of truth for each toolchain; these
# targets make them discoverable through the conventional `make` interface.

ROOT_DIR := $(abspath $(dir $(lastword $(MAKEFILE_LIST))))
PACKAGE_BASENAME := trinket_sd
PACKAGE_DIR := $(ROOT_DIR)/dist
PACKAGE_FILES := TRINKET.SYS SDTEST.EXE IFTEST.EXE

.PHONY: all build package toolchain check-toolchain setup setup-firmware firmware build-firmware client driver build-driver clean clean-all clean-firmware clean-driver help

all: build

build:
	@"$(ROOT_DIR)/build.sh"

package: build
	@cd "$(PACKAGE_DIR)" && \
		tar -czf "$(PACKAGE_BASENAME).tar.gz" $(PACKAGE_FILES) && \
		7z a -t7z "$(PACKAGE_BASENAME).7z" $(PACKAGE_FILES) >/dev/null && \
		zip -q "$(PACKAGE_BASENAME).zip" $(PACKAGE_FILES)
	@printf '%s\n' \
		"Packaged artifacts: $(PACKAGE_DIR)/$(PACKAGE_BASENAME).tar.gz" \
		"                    $(PACKAGE_DIR)/$(PACKAGE_BASENAME).7z" \
		"                    $(PACKAGE_DIR)/$(PACKAGE_BASENAME).zip"

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
	@$(MAKE) -C "$(ROOT_DIR)/firmware/client"

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

help:
	@printf '%s\n' \
		'Usage: make [target]' \
		'' \
		'Build targets:' \
		'  all, build       Build firmware, client tools, and DOS driver' \
		'  package          Build everything and create .tar.gz, .7z, and .zip archives' \
		'  toolchain        Check dependencies and print setup suggestions' \
		'  setup            Install or link firmware build dependencies' \
		'  firmware         Build firmware and client tools' \
		'  client           Build only the DOS client tools' \
		'  driver           Build only the DOS driver' \
		'  setup-firmware, build-firmware, build-driver' \
		'                   Script-name aliases for the targets above' \
		'' \
		'Clean targets:' \
		'  clean, clean-all  Remove all generated build artifacts' \
		'  clean-firmware    Remove firmware and client artifacts' \
		'  clean-driver      Remove the DOS driver artifact'
