#!/usr/bin/env bash

# exit when any command fails
set -e

global_usage() {
  cat - <<EOF
Usage:
sh build.sh <command> [target]

Commands:
  help|usage|-h|--help: Shows this message.
  list|-l: List firmwares available to build.
  build-firmware <target>: Build the firmware for the given build target.
  flash-firmware <target>: Build the firmware, then upload it to a connected device.
                           Set PLATFORMIO_UPLOAD_PORT to pick a port when several are attached.
  build-firmwares: Build all firmwares for all targets.
  build-matching-firmwares <build-match-spec>: Build all firmwares for build targets containing the string given for <build-match-spec>.
  build-companion-firmwares: Build all companion firmwares for all build targets.
  build-repeater-firmwares: Build all repeater firmwares for all build targets.
  build-room-server-firmwares: Build all chat room server firmwares for all build targets.

Examples:
Build firmware for the "RAK_4631_repeater" device target
$ sh build.sh build-firmware RAK_4631_repeater

Build all firmwares for device targets containing the string "RAK_4631"
$ sh build.sh build-matching-firmwares <build-match-spec>

Build all companion firmwares
$ sh build.sh build-companion-firmwares

Build all repeater firmwares
$ sh build.sh build-repeater-firmwares

Build all chat room server firmwares
$ sh build.sh build-room-server-firmwares

Build all kiss radio firmwares
$ sh build.sh build-kiss-radio-firmwares

Environment Variables:
  FIRMWARE_VERSION: Required. The version string baked into the firmware, e.g: v1.0.0
  DISABLE_DEBUG=1: Disables all debug logging flags (MESH_DEBUG, MESH_PACKET_LOGGING, etc.)
                   If not set, debug flags from variant platformio.ini files are used.
  BLE_PIN_CODE=<6 digits>: Overrides the BLE pin code set by the variant platformio.ini files.
                   Applied to every BLE capable target, including boards with a screen, which
                   use the given pin instead of generating a random one on each boot. Targets
                   built without BLE are unaffected. If not set, the pin from the variant
                   files is used, and boards with a screen keep generating a pin per boot.

Examples:
Build without debug logging:
$ export FIRMWARE_VERSION=v1.0.0
$ export DISABLE_DEBUG=1
$ sh build.sh build-firmware RAK_4631_repeater

Build with debug logging (default, uses flags from variant files):
$ export FIRMWARE_VERSION=v1.0.0
$ sh build.sh build-firmware RAK_4631_repeater

Build with a custom static BLE pin code:
$ export FIRMWARE_VERSION=v1.0.0
$ export BLE_PIN_CODE=987654
$ sh build.sh build-firmware RAK_4631_companion_radio_ble
EOF
}

# get a list of pio env names that start with "env:"
get_pio_envs() {
  pio project config | grep 'env:' | sed 's/env://'
}

# Catch cries for help before doing anything else.
case $1 in
  help|usage|-h|--help)
    global_usage
    exit 1
    ;;
  list|-l)
    get_pio_envs
    exit 0
    ;;
esac

# cache project config json for use in get_platform_for_env()
PIO_CONFIG_JSON=$(pio project config --json-output)

# remember the build flags provided by the environment, so per target flags added
# by build_firmware() don't accumulate when building more than one target
PLATFORMIO_BUILD_FLAGS_BASE="${PLATFORMIO_BUILD_FLAGS}"
PLATFORMIO_BUILD_UNFLAGS_BASE="${PLATFORMIO_BUILD_UNFLAGS}"

# extra targets appended to the 'pio run' that build_firmware() performs, e.g. "-t upload".
# Flashing goes through the same function as building on purpose: pio would otherwise
# rebuild with whatever flags happened to be set, and quietly upload a different binary
# from the one 'build-firmware' just produced.
PIO_RUN_TARGETS=""

# $1 should be the string to find (case insensitive)
get_pio_envs_containing_string() {
  shopt -s nocasematch
  envs=($(get_pio_envs))
  for env in "${envs[@]}"; do
      if [[ "$env" == *${1}* ]]; then
        echo $env
      fi
  done
}

# $1 should be the string to find (case insensitive)
get_pio_envs_ending_with_string() {
  shopt -s nocasematch
  envs=($(get_pio_envs))
  for env in "${envs[@]}"; do
    if [[ "$env" == *${1} ]]; then
      echo $env
    fi
  done
}

# get platform flag for a given environment
# $1 should be the environment name
get_platform_for_env() {
  local env_name=$1
  printf '%s' "$PIO_CONFIG_JSON" | python3 -c "
import sys, json, re
data = json.load(sys.stdin)
for section, options in data:
    if section == 'env:$env_name':
        for key, value in options:
            if key == 'build_flags':
                for flag in value:
                    match = re.search(r'(ESP32_PLATFORM|NRF52_PLATFORM|STM32_PLATFORM|RP2040_PLATFORM)', flag)
                    if match:
                        print(match.group(1))
                        sys.exit(0)
"
}

# get the BLE pin code a given environment builds with, as set by the variant
# platformio.ini files. prints nothing for targets that don't define one.
# $1 should be the environment name
get_ble_pin_code_for_env() {
  local env_name=$1
  printf '%s' "$PIO_CONFIG_JSON" | python3 -c "
import sys, json, re
data = json.load(sys.stdin)
for section, options in data:
    if section == 'env:$env_name':
        for key, value in options:
            if key == 'build_flags':
                for flag in value:
                    match = re.search(r'-D\s*BLE_PIN_CODE\s*=\s*(\S+)', flag)
                    if match:
                        print(match.group(1))
                        sys.exit(0)
"
}

# override the static BLE pin code if BLE_PIN_CODE is set
# $1 should be the environment name
override_ble_pin_code() {
  if [ -z "$BLE_PIN_CODE" ]; then
    return
  fi

  # the BLE pairing passkey is always 6 digits
  if [[ ! "$BLE_PIN_CODE" =~ ^[0-9]{6}$ ]]; then
    echo "BLE_PIN_CODE must be a 6 digit number, got: $BLE_PIN_CODE"
    exit 1
  fi

  # targets without a BLE pin code have no BLE interface at all, defining one
  # here would change what gets compiled into them, so leave them alone
  CURRENT_BLE_PIN_CODE=$(get_ble_pin_code_for_env $1)
  if [ -z "$CURRENT_BLE_PIN_CODE" ]; then
    echo "BLE_PIN_CODE is set, but $1 does not use a BLE pin code, skipping override"
    return
  fi

  # drop the pin set by the variant build flags, then define the requested one.
  # note: -UBLE_PIN_CODE can't be used here, platformio passes unflags to the
  # compiler after all of the defines, which would undefine the new pin too.
  # when the pins match there is nothing to swap, and unflagging would remove
  # both copies of the define, leaving the target with no pin at all.
  if [ "$CURRENT_BLE_PIN_CODE" != "$BLE_PIN_CODE" ]; then
    export PLATFORMIO_BUILD_UNFLAGS="${PLATFORMIO_BUILD_UNFLAGS_BASE} -DBLE_PIN_CODE=${CURRENT_BLE_PIN_CODE}"
    export PLATFORMIO_BUILD_FLAGS="${PLATFORMIO_BUILD_FLAGS} -DBLE_PIN_CODE=${BLE_PIN_CODE}"
  fi

  # boards with a screen generate a random pin on each boot rather than using
  # the pin from the variant build flags. this tells them to use the pin asked
  # for here instead.
  export PLATFORMIO_BUILD_FLAGS="${PLATFORMIO_BUILD_FLAGS} -DBLE_PIN_CODE_STATIC=1"
}

# disable all debug logging flags if DISABLE_DEBUG=1 is set
disable_debug_flags() {
  if [ "$DISABLE_DEBUG" == "1" ]; then
    export PLATFORMIO_BUILD_FLAGS="${PLATFORMIO_BUILD_FLAGS} -UMESH_DEBUG -UBLE_DEBUG_LOGGING -UWIFI_DEBUG_LOGGING -UBRIDGE_DEBUG -UGPS_NMEA_DEBUG -UCORE_DEBUG_LEVEL -UESPNOW_DEBUG_LOGGING -UDEBUG_RP2040_WIRE -UDEBUG_RP2040_SPI -UDEBUG_RP2040_CORE -UDEBUG_RP2040_PORT -URADIOLIB_DEBUG_SPI -UCFG_DEBUG -URADIOLIB_DEBUG_BASIC -URADIOLIB_DEBUG_PROTOCOL"
  fi
}

# build firmware for the provided pio env in $1
build_firmware() {
  # get env platform for post build actions
  ENV_PLATFORM=($(get_platform_for_env $1))

  # get git commit sha
  COMMIT_HASH=$(git rev-parse --short HEAD)

  # set firmware build date
  FIRMWARE_BUILD_DATE=$(date '+%d-%b-%Y')

  # get FIRMWARE_VERSION, which should be provided by the environment
  if [ -z "$FIRMWARE_VERSION" ]; then
    echo "FIRMWARE_VERSION must be set in environment"
    exit 1
  fi

  # set firmware version string
  # e.g: v1.0.0-abcdef
  FIRMWARE_VERSION_STRING="${FIRMWARE_VERSION}-${COMMIT_HASH}"

  # craft filename
  # e.g: RAK_4631_Repeater-v1.0.0-SHA
  FIRMWARE_FILENAME="$1-${FIRMWARE_VERSION_STRING}"

  # add firmware version info to end of existing platformio build flags in environment vars
  export PLATFORMIO_BUILD_FLAGS="${PLATFORMIO_BUILD_FLAGS_BASE} -DFIRMWARE_BUILD_DATE='\"${FIRMWARE_BUILD_DATE}\"' -DFIRMWARE_VERSION='\"${FIRMWARE_VERSION_STRING}\"'"
  export PLATFORMIO_BUILD_UNFLAGS="${PLATFORMIO_BUILD_UNFLAGS_BASE}"

  # override the static BLE pin code if requested
  override_ble_pin_code $1

  # disable debug flags if requested
  disable_debug_flags

  # build firmware target (and upload it too, when PIO_RUN_TARGETS says so)
  pio run -e $1 $PIO_RUN_TARGETS

  # build merge-bin for esp32 fresh install, copy .bins to out folder (e.g: Heltec_v3_room_server-v1.0.0-SHA.bin)
  if [ "$ENV_PLATFORM" == "ESP32_PLATFORM" ]; then
    pio run -t mergebin -e $1
    cp .pio/build/$1/firmware.bin out/${FIRMWARE_FILENAME}.bin 2>/dev/null || true
    cp .pio/build/$1/firmware-merged.bin out/${FIRMWARE_FILENAME}-merged.bin 2>/dev/null || true
  fi

  # build .uf2 for nrf52 boards, copy .uf2 and .zip to out folder (e.g: RAK_4631_Repeater-v1.0.0-SHA.uf2)
  if [ "$ENV_PLATFORM" == "NRF52_PLATFORM" ]; then
    python3 bin/uf2conv/uf2conv.py .pio/build/$1/firmware.hex -c -o .pio/build/$1/firmware.uf2 -f 0xADA52840
    cp .pio/build/$1/firmware.uf2 out/${FIRMWARE_FILENAME}.uf2 2>/dev/null || true
    cp .pio/build/$1/firmware.zip out/${FIRMWARE_FILENAME}.zip 2>/dev/null || true
  fi

  # for stm32, copy .bin and .hex to out folder
  if [ "$ENV_PLATFORM" == "STM32_PLATFORM" ]; then
    cp .pio/build/$1/firmware.bin out/${FIRMWARE_FILENAME}.bin 2>/dev/null || true
    cp .pio/build/$1/firmware.hex out/${FIRMWARE_FILENAME}.hex 2>/dev/null || true
  fi

  # for rp2040, copy .bin and .uf2 to out folder
  if [ "$ENV_PLATFORM" == "RP2040_PLATFORM" ]; then
    cp .pio/build/$1/firmware.bin out/${FIRMWARE_FILENAME}.bin 2>/dev/null || true
    cp .pio/build/$1/firmware.uf2 out/${FIRMWARE_FILENAME}.uf2 2>/dev/null || true
  fi

}

# firmwares containing $1 will be built
build_all_firmwares_matching() {
  envs=($(get_pio_envs_containing_string "$1"))
  for env in "${envs[@]}"; do
      build_firmware $env
  done
}

# firmwares ending with $1 will be built
build_all_firmwares_by_suffix() {
  envs=($(get_pio_envs_ending_with_string "$1"))
  for env in "${envs[@]}"; do
    build_firmware $env
  done
}

build_repeater_firmwares() {

#  # build specific repeater firmwares
#  build_firmware "Heltec_v2_repeater"
#  build_firmware "Heltec_v3_repeater"
#  build_firmware "Xiao_C3_Repeater_sx1262"
#  build_firmware "Xiao_S3_WIO_Repeater"
#  build_firmware "LilyGo_T3S3_sx1262_Repeater"
#  build_firmware "RAK_4631_Repeater"

  # build all repeater firmwares
  build_all_firmwares_by_suffix "_repeater"

}

build_companion_firmwares() {

#  # build specific companion firmwares
#  build_firmware "Heltec_v2_companion_radio_usb"
#  build_firmware "Heltec_v2_companion_radio_ble"
#  build_firmware "Heltec_v3_companion_radio_usb"
#  build_firmware "Heltec_v3_companion_radio_ble"
#  build_firmware "Xiao_S3_WIO_companion_radio_ble"
#  build_firmware "LilyGo_T3S3_sx1262_companion_radio_usb"
#  build_firmware "LilyGo_T3S3_sx1262_companion_radio_ble"
#  build_firmware "RAK_4631_companion_radio_usb"
#  build_firmware "RAK_4631_companion_radio_ble"
#  build_firmware "t1000e_companion_radio_ble"

  # build all companion firmwares
  build_all_firmwares_by_suffix "_companion_radio_usb"
  build_all_firmwares_by_suffix "_companion_radio_ble"

}

build_room_server_firmwares() {

#  # build specific room server firmwares
#  build_firmware "Heltec_v3_room_server"
#  build_firmware "RAK_4631_room_server"

  # build all room server firmwares
  build_all_firmwares_by_suffix "_room_server"

}

build_kiss_modem_firmwares() {

#  # build specific kiss radio firmwares
#  build_firmware "Heltec_v3_kiss_modem"
#  build_firmware "RAK_4631_kiss_modem"

  # build all room server firmwares
  build_all_firmwares_by_suffix "_kiss_modem"

}

build_firmwares() {
  build_companion_firmwares
  build_repeater_firmwares
  build_room_server_firmwares
}

# clean build dir
rm -rf out
mkdir -p out

# handle script args
if [[ $1 == "build-firmware" ]]; then
  TARGETS=${@:2}
  if [ "$TARGETS" ]; then
    for env in $TARGETS; do
      build_firmware $env
    done
  else
    echo "usage: $0 build-firmware <target>"
    exit 1
  fi
elif [[ $1 == "flash-firmware" ]]; then
  if [ "$2" ]; then
    PIO_RUN_TARGETS="-t upload"
    build_firmware $2
  else
    echo "usage: $0 flash-firmware <target>"
    exit 1
  fi
elif [[ $1 == "build-matching-firmwares" ]]; then
  if [ "$2" ]; then
     build_all_firmwares_matching $2
  else
     echo "usage: $0 build-matching-firmwares <build-match-spec>"
    exit 1
  fi
elif [[ $1 == "build-firmwares" ]]; then
  build_firmwares
elif [[ $1 == "build-companion-firmwares" ]]; then
  build_companion_firmwares
elif [[ $1 == "build-repeater-firmwares" ]]; then
  build_repeater_firmwares
elif [[ $1 == "build-room-server-firmwares" ]]; then
  build_room_server_firmwares
elif [[ $1 == "build-kiss-radio-firmwares" ]]; then
  build_kiss_modem_firmwares
elif [[ $1 == "get-companion-firmwares-to-build" ]]; then
  get_pio_envs_ending_with_string "_companion_radio_usb"
  get_pio_envs_ending_with_string "_companion_radio_ble"
elif [[ $1 == "get-repeater-firmwares-to-build" ]]; then
  get_pio_envs_ending_with_string "_repeater"
elif [[ $1 == "get-room-server-firmwares-to-build" ]]; then
  get_pio_envs_ending_with_string "_room_server"
fi
