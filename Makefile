# Wrapper around build.sh for building a single firmware target and copying the
# result into the binaries dir next to this repo.
#
# usage:
#   make firmware FIRMWARE=RAK_4631_companion_radio_ble VERSION=v1.16.0.4 BLE_PIN=<6 digits>
#
# any of these can come from the environment instead:
#   export VERSION=v1.16.0.4
#   make firmware FIRMWARE=RAK_4631_companion_radio_ble
#
# FIRMWARE and VERSION are required, BLE_PIN is optional and when left unset the
# pin from the variant platformio.ini files is used.
#
# note: firmware.zip is the nrf52 DFU package, so this target suits nrf52 boards.
# esp32 and rp2040 targets build firmware.bin / firmware.uf2 instead.

FIRMWARE ?=
BINARIES_DIR ?= ../binaries

# the names build.sh expects are also accepted, for anyone already exporting them
VERSION ?= $(FIRMWARE_VERSION)
BLE_PIN ?= $(BLE_PIN_CODE)

# build.sh reads these from the environment
export FIRMWARE_VERSION := $(VERSION)
export BLE_PIN_CODE := $(BLE_PIN)

.PHONY: firmware
firmware:
	@test -n "$(FIRMWARE)" || { echo "FIRMWARE must be set, e.g: make firmware FIRMWARE=RAK_4631_companion_radio_ble"; exit 1; }
	@test -n "$(VERSION)" || { echo "VERSION must be set, e.g: make firmware FIRMWARE=$(FIRMWARE) VERSION=v1.16.0.4"; exit 1; }
	sh build.sh build-firmware $(FIRMWARE)
	mkdir -p $(BINARIES_DIR)
	cp .pio/build/$(FIRMWARE)/firmware.zip $(BINARIES_DIR)/$(FIRMWARE).zip

.PHONY: list
list:
	@sh build.sh list
