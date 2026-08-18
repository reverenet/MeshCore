# Wrapper around build.sh for building, flashing and erasing a single firmware target.
#
#   make help              every command and every argument it takes
#   make <command> help    one command: what it would do now, and the settings it reads
#   make flags             what the arguments currently resolve to, without building
#
# The help text below is the only copy of that documentation, so it cannot drift from
# a second one kept in comments. Deeper rationale lives next to the code it explains:
# examples/companion_radio/AutoAdvert.h for the advert and tracking arguments,
# configs/boston.conf for the network profile, tools/flash_erase/README.md for erasing.

define HELP_TEXT

MeshCore firmware builds. Every argument below is NAME=VALUE on the make command line,
or exported in the environment beforehand.

COMMANDS

  make firmwares [MATCH='rak ble']
      List every name FIRMWARE= accepts. MATCH narrows to targets containing all of the
      given substrings, in any order. 'make list' is the same command.

  make firmware FIRMWARE=<target> VERSION=<ver> [arguments]
      Build one target and copy the result into BINARIES_DIR. With NAME= the files carry
      the name - <target>@Kitchen_Repeater.uf2 - because the name is compiled in, so one
      target built under two names is two different images and filing both under the
      target name would lose the first. 'make flash' offers whichever of them fit the
      board attached. firmware.zip is the nRF52 DFU package; esp32 and rp2040 targets
      produce firmware.bin / firmware.uf2 instead.
      NAME='Dennis,Eric,Mike' builds one firmware per name, in that order, one after the
      other - a fleet in a single command. A name is compiled in, so each is a full
      build; only the first of them builds an erase image.
      A matching erase image, <target>-erase.uf2, is built alongside the first build of
      that target and then kept: it formats the filesystem and halts before a name could
      mean anything, so one per target is all there is, however many named builds follow.
      WITH_ERASE=1 rebuilds it anyway, WITH_ERASE=0 skips it.

  make flash [FIRMWARE=<target> VERSION=<ver>] [arguments]
      Upload firmware to a device attached over USB. Identifies the board first, names
      it for confirmation, and reuses the image 'make firmware' already built for it
      rather than building again - so with a warm BINARIES_DIR this needs no arguments
      at all. Where several images fit the board - a repeater and a companion, or four
      builds of one target under four device names - it lists them and asks which to use;
      FIRMWARE= or NAME= answers that in advance, and a run with no terminal to ask on
      gets the same list as an error rather than a guess. Falls back to building when
      nothing is built for the board, which needs FIRMWARE= and VERSION=; BUILD=1 forces
      that build even when an image is there.

  make name NAME='Kitchen Repeater' [PORT=<port>]
      Name a board that is already flashed and running, over its serial CLI. Writes the
      saved prefs directly, so it renames a device that has been named before - which
      the compiled-in name cannot do. Repeater, room server and sensor targets only;
      the companion firmware is named from the app.

  make detect
      Say which board is attached and how it was identified, without touching it.

  make erase-firmware FIRMWARE=<target> VERSION=<ver>
      Build an erase image for a board without sending it anywhere. It is ordinary
      firmware for that same target that formats the filesystem as the first thing in
      setup() and then halts, so it recovers a device whose stored state is what breaks
      the boot, and needs no debug probe. Artifacts land in BINARIES_DIR with an -erase
      suffix. Flash it like any other firmware, or hand it to someone else to flash.

  make erase [FIRMWARE=<target> VERSION=<ver>]
      Wipe a board back to blank flash. Identifies the board, names it for confirmation,
      and looks for an image before building one, in order: ERASE_UF2 if named, then
      BINARIES_DIR/<target>-erase.uf2 left by 'make firmware', then anything downloaded
      into tools/flash_erase. Any erase image for the same board will do - they all just
      format the filesystem - so this needs no FIRMWARE= once one has been built.
      An nRF52 or RP2040 board is reset into its bootloader automatically and the image
      copied onto the drive that appears; a board too broken to enumerate its serial port
      has to be put there by hand (double-tap reset) before running this.
      Building only happens when no image was found, and needs FIRMWARE= and VERSION=.
      On ESP32 with neither, falls back to esptool's own flash erase.

  make keys
      Generate the tracking key file. Never writes over one that already exists, and
      prints its fingerprint so two machines can be checked for agreement.

  make flags
      Show how every argument resolves for the build that would run. Key material is
      never printed, only fingerprints.

  make <command> help
      Explain one command instead of running it: what it would do with the arguments as
      they stand, and every setting it reads, with the value that setting has now and
      what its default means. Nothing is written to a device, so this is also the way to
      check a flash or an erase before committing to it. Order does not matter, and more
      than one command can be named: 'make flash help' and 'make help erase flash' both
      work.

  make help
      This text.

ARGUMENTS

 build and flash
  FIRMWARE      target name, from 'make firmwares'. Required by firmware and flash.
  VERSION       version string baked into the build, e.g. v1.17.0. Required by firmware
                and flash. Also accepted under its build.sh name, FIRMWARE_VERSION.
  NAME          the device's own name, max 31 bytes. Does NOT cause a build. On 'make
                firmware' it is compiled in and becomes part of the filename, and several
                comma-separated names there build one firmware each: 'Dennis, Eric,Mike'
                is three. Space around a comma is trimmed and space inside a name is
                kept, so 'Kitchen Repeater, Garage' is two names, not three. A repeated
                name is built once. Every other command takes a single name. On
                'make flash' it first picks the image built under that name, if there is
                one; otherwise it sends the image there is and sets the name over the CLI,
                which renames a board that has been named before. A compiled-in name is
                only the default for a device with no saved prefs, so it will NOT rename
                one already named through the app. The companion firmware answers the app
                protocol rather than the CLI, so a NAME that cannot be applied is reported
                before the prompt rather than passed over: name it from the app, or add
                BUILD=1 to compile it in.
  BLE_PIN       6 digits, replacing the pin from the variant files. Boards with a screen
                use it instead of generating a pin on each boot. Targets built without
                BLE are unaffected.
  PORT          serial port to upload to. 'pio device list' shows what is attached; only
                needed when more than one device is.
  BINARIES_DIR  where the built firmware is copied. Default ../binaries.
  YES           1 answers the "Continue?" prompt that flash and erase ask before writing
                to a device. Needed for a non-interactive run, which otherwise stops.
  BUILD         1 makes 'make flash' build the target instead of reusing a built image.
                Any argument that changes the binary - keys, radio settings, GPS, advert
                and tracking - only takes effect on a build, so passing one implies this.
                NAME does not: it is applied after the flash instead.
  ERASE_UF2     erase image for 'make erase', instead of looking one up or building it.
  WITH_ERASE    0 stops 'make firmware' building the erase image at all, halving the time
                when you do not need one. 1 rebuilds it even though one is already in
                BINARIES_DIR, which is what a version bump wants. Unset builds it only
                when the target has none.

 network profile - radio and regions
  CONFIG        profile to read, as NAME = VALUE lines. Default configs/boston.conf.
                CONFIG= (empty) builds the stock per-variant settings instead. Values in
                the file are defaults, so passing the same name to make overrides one
                line without editing it. Keys are rejected there on purpose - a profile
                gets shared and committed.
  LORA_FREQ     MHz. These four must match the rest of the network exactly. A node that
  LORA_BW       kHz. differs on frequency, bandwidth or spreading factor simply never
  LORA_SF       5-12. hears the mesh, with nothing to indicate why.
  LORA_CR       5-8.
  LORA_TX_POWER dBm. Usually left to the variant, since it depends on the board's PA.
  DEFAULT_REGIONS
                repeater only: comma-separated regions to create flood-enabled on a
                device with none configured yet, e.g. ma,newengland,us. The first is the
                scope the node originates its own floods in. Applied only when no region
                is configured, so it will not undo an operator's saved setup.
  DEFAULT_FLOOD_SCOPE_NAME
                single scope, which is what the room server and sensor take instead.

 keys - the shared secret of a tracking group
  TRACKING_KEY  32 hex characters used as the raw AES-128 key, or any other string, which
                is hashed into one. Unset compiles position tracking out entirely. Only
                the nodes that may read a position need it: repeaters relay reports
                without decrypting them, so a repeater is never given this key. Normally
                left to the key file.
  TRACKING_KEY_FILE, KEYS_DIR
                where the key is read from. Default keys/tracking.key. A missing default
                file is not an error - it just builds without the feature - but a file
                named explicitly has to exist. Use these to keep several networks side
                by side.

 GPS
  GPS_ENABLED   whether the receiver is switched on at first boot. 1 on the companion,
                0 on the repeater, which is normally a fixed install with a hand-set
                position. Needs a board built with ENV_INCLUDE_GPS and a receiver
                actually detected; elsewhere the setting is refused and nothing changes.
                Only applies to a device with no saved prefs - use 'gps on' over the CLI
                or the app's toggle for one already in the field.
  GPS_INTERVAL  seconds between position updates. 0 leaves the sensor manager's own
                cadence alone, which is once a second.

 position tracking - companion only, needs TRACKING_KEY
  TRACK_REPORT  1 = this node reports its own position. Leave unset on a node that should
                receive and display tracks without reporting.
  TRACK_REPORT_SECS
                fixed transmit cadence. Constant rate is the point: it stops airtime from
                revealing whether the node is moving.
  TRACK_SAMPLE_MIN_SECS, TRACK_SAMPLE_MAX_SECS, TRACK_SAMPLE_DIST_M
                how often position is sampled, and how far it must move to force one.
  TRACK_BUFFER  backlog depth, 12 bytes of RAM per sample.
  TRACK_FLOOD   0 = zero-hop (default), 1 = scoped flood.

 advert scheduling - companion only
  AUTO_ADVERT_SECS, AUTO_ADVERT_FLOOD
                periodic advert carrying no position, as a liveness heartbeat.
  AUTO_ADVERT_LOC
                1 enables the adaptive location beacon; everything below tunes it.
  AUTO_ADVERT_LOC_POLICY
                NONE, SHARE or PREFS.
  AUTO_ADVERT_LOC_MIN_SECS, AUTO_ADVERT_LOC_MAX_SECS, AUTO_ADVERT_LOC_DIST_M,
  AUTO_ADVERT_LOC_BACKOFF, AUTO_ADVERT_LOC_JITTER_PCT, AUTO_ADVERT_LOC_STARTUP_SECS,
  AUTO_ADVERT_LOC_FLOOD
                interval floor and ceiling, the distance that forces a send, and the
                backoff and jitter applied while the node sits still.
  FLOOD_MAX_ADVERT
                hops an advert may accumulate before a repeat-enabled companion stops
                forwarding it.

EXAMPLES

  make firmwares MATCH='rak ble'
  make keys
  make detect
  make flash
  make flash NAME='Kitchen Repeater'
  make name NAME='Kitchen Repeater'
  make firmware FIRMWARE=RAK_4631_repeater VERSION=v1.17.0 NAME='Dennis,Eric,Mike,Dad'
  make flash FIRMWARE=RAK_4631_companion_radio_ble VERSION=v1.17.0 BLE_PIN=987654
  make firmware FIRMWARE=RAK_4631_companion_radio_ble VERSION=v1.17.0 TRACK_REPORT=1
  make firmware FIRMWARE=Heltec_v3_repeater VERSION=v1.17.0 CONFIG= LORA_SF=10
  make erase

endef
export HELP_TEXT

FIRMWARE ?=
BINARIES_DIR ?= ../binaries

# the names build.sh expects are also accepted, for anyone already exporting them
VERSION ?= $(FIRMWARE_VERSION)
BLE_PIN ?= $(BLE_PIN_CODE)

# build.sh reads these from the environment
export FIRMWARE_VERSION := $(VERSION)
export BLE_PIN_CODE := $(BLE_PIN)

# pio auto-detects the port when one device is attached; PORT= settles it when several are
PORT ?=
ifneq ($(strip $(PORT)),)
  export PLATFORMIO_UPLOAD_PORT := $(PORT)
endif

# Which board is attached, which image belongs to it, and how that image gets there - all
# three answered in one place so that flash and erase cannot disagree about any of them.
# What the identification can and cannot be sure of is documented at the top of the script.
DEVICE := python3 tools/device.py

# ------------------------------------------------------------------ build arguments

# passed straight through as -D<name>=<value>, and required to be plain integers:
# they end up in #if expressions, where a typo like TRACK_REPORT=yes would evaluate
# to 0 and silently build the feature out
NUMERIC_ARGS := \
  FLOOD_MAX_ADVERT \
  GPS_ENABLED GPS_INTERVAL \
  TRACK_REPORT TRACK_REPORT_SECS TRACK_SAMPLE_MIN_SECS TRACK_SAMPLE_MAX_SECS \
  TRACK_SAMPLE_DIST_M TRACK_BUFFER TRACK_FLOOD \
  AUTO_ADVERT_SECS AUTO_ADVERT_FLOOD \
  AUTO_ADVERT_LOC AUTO_ADVERT_LOC_MIN_SECS AUTO_ADVERT_LOC_MAX_SECS \
  AUTO_ADVERT_LOC_DIST_M AUTO_ADVERT_LOC_BACKOFF AUTO_ADVERT_LOC_JITTER_PCT \
  AUTO_ADVERT_LOC_STARTUP_SECS AUTO_ADVERT_LOC_FLOOD

# radio settings, which are decimals rather than integers, and the string-valued ones
RADIO_ARGS  := LORA_FREQ LORA_BW LORA_SF LORA_CR LORA_TX_POWER
STRING_ARGS := DEFAULT_REGIONS DEFAULT_FLOOD_SCOPE_NAME

# Arguments that change the binary, named on THIS command line rather than defaulted or
# read from the config file. 'make flash' reuses a built image, and an argument passed to
# a run that reuses one would silently do nothing - so passing one turns the build back
# on. $(origin) is what separates a real command line argument from the same name having
# a default: comparing values cannot tell LORA_SF=9 from an SF that was always set.
#
# NAME is deliberately NOT in this list. It is per-device, so it is given on nearly every
# flash, and building a whole firmware to change 31 bytes of default would make reusing
# an image pointless. It is applied after the flash instead, over the serial CLI, which
# also renames a board that has been named already - something the compiled-in default
# cannot do. See the flash recipe.
BUILD_ARGS := $(NUMERIC_ARGS) $(RADIO_ARGS) $(STRING_ARGS) \
  BLE_PIN BLE_PIN_CODE CONFIG AUTO_ADVERT_LOC_POLICY \
  TRACKING_KEY TRACKING_KEY_FILE KEYS_DIR
CMDLINE_BUILD_ARGS = $(strip $(foreach v,$(BUILD_ARGS),\
  $(if $(filter command line,$(origin $(v))),$(v))))

# limits the firmware itself enforces, mirrored here so a bad value is reported at
# build time instead of being silently clamped on the device (AdvertScheduler::begin)
MAX_SANE_SECS  := 86400
MAX_JITTER_PCT := 50

# empty when $(1) is a non-negative integer
not_a_number = $(shell printf %s '$(1)' | grep -qE '^[0-9]+$$' || echo bad)

# empty when $(1) is a non-negative decimal - LORA_FREQ is 910.525, not an integer
not_a_decimal = $(shell printf %s '$(1)' | grep -qE '^[0-9]+(\.[0-9]+)?$$' || echo bad)

# --------------------------------------------------------------- network config file

# A network profile: radio settings and repeater regions, as NAME = VALUE lines. These
# are defaults, so anything passed to make overrides the file without editing it.
# CONFIG= (empty) builds the stock per-variant settings instead.
CONFIG ?= configs/boston.conf

CONFIG_ARGS := $(RADIO_ARGS) $(STRING_ARGS) $(NUMERIC_ARGS) AUTO_ADVERT_LOC_POLICY

ifneq ($(strip $(CONFIG)),)
  ifeq ($(wildcard $(CONFIG)),)
    $(error CONFIG=$(CONFIG) does not exist)
  endif

  # NAME=VALUE per setting. Values may not contain spaces, which keeps each setting a
  # single word and lets the foreach below walk them.
  CONFIG_SETTINGS := $(shell sed -n 's/^[[:space:]]*\([A-Za-z_][A-Za-z0-9_]*\)[[:space:]]*=[[:space:]]*\([^[:space:]]*\).*$$/\1=\2/p' '$(CONFIG)')

  # $(eval) rather than include: a plain assignment still loses to the command line,
  # which is exactly the precedence wanted here.
  # NOTE: no $(call) anywhere in this loop - DEFAULT_REGIONS is comma-separated, and
  # $(call) would split it into arguments and silently keep only the first region.
  $(foreach kv,$(CONFIG_SETTINGS),\
    $(if $(filter TRACKING_KEY,$(firstword $(subst =, ,$(kv)))),\
      $(error $(CONFIG) sets $(firstword $(subst =, ,$(kv))): keys belong in a key file, \
              not in a config file that gets shared and committed))\
    $(if $(filter $(firstword $(subst =, ,$(kv))),$(CONFIG_ARGS)),,\
      $(error $(CONFIG): '$(firstword $(subst =, ,$(kv)))' is not a settable argument))\
    $(eval $(firstword $(subst =, ,$(kv))) := $(word 2,$(subst =, ,$(kv)))))
endif

$(foreach v,$(RADIO_ARGS),$(if $($(v)),$(if $(call not_a_decimal,$($(v))),\
  $(error $(v) must be a number, got '$($(v))'))))

# The device's own name, so a board can be flashed ready-labelled. Kept out of the config
# file deliberately: a name identifies one device, a config file describes a network.
#
# This is only the default for a device with no saved prefs yet. Renaming through the app
# or CLI persists, and loadPrefs() runs after this is applied - so reflashing with a new
# NAME does NOT rename a device that has already been named. Erase it first.
NAME ?=

# NAME=Dennis,Eric,Mike builds one firmware per name, in that order, one after the other.
# The comma is the separator, so a name cannot contain one; each name is then trimmed of
# surrounding whitespace and keeps whatever is inside it, leaving 'Kitchen Repeater' a
# single name.
#
# The splitting happens in the recipe rather than here, because make would have to split
# on whitespace to walk a list and that is exactly what has to be preserved. So a list is
# only recognised here, and every check below is left to the sub-make that builds one name:
# it sees a single NAME and validates it the way it always has. Checking the list as one
# string would reject four short names for busting a 31 byte field none of them fills.
comma := ,
NAME_IS_LIST := $(findstring $(comma),$(NAME))

ifeq ($(NAME_IS_LIST),)
ifneq ($(NAME),)
  # spaces are fine here - unlike a region name, this only has to survive the shell - but
  # quotes and the rest would break the -D, and node_name is a 32 byte field
  ifneq ($(findstring ',$(NAME))$(findstring ",$(NAME))$(findstring `,$(NAME))$(findstring $$,$(NAME))$(findstring \,$(NAME)),)
    $(error NAME must not contain quotes, backslashes, backticks or $$)
  endif
  # bytes, not characters, because that is what the 32 byte field actually holds - a name
  # of accented or emoji characters runs out sooner than it looks
  ifneq ($(shell test $$(printf %s '$(NAME)' | wc -c) -le 31 || echo bad),)
    $(error NAME does not fit the 31 bytes node_name holds, and would be truncated: '$(NAME)')
  endif
  NAME_ARG_FLAG := -DADVERT_NAME='"$(NAME)"'

  # The name as it appears in a filename: anything a filesystem might object to becomes an
  # underscore. tools/device.py does the same substitution when it reads these back, and
  # the filename is the only place the two meet.
  NAME_SLUG := $(shell printf %s '$(NAME)' | sed 's/[^A-Za-z0-9._-]/_/g')
endif
endif

# What 'make firmware' files its artifacts under. '@' separates the name rather than '-',
# because '-erase' is already a suffix here: a device called "erase" would otherwise
# produce <target>-erase.uf2, which is exactly what an erase image is called.
IMAGE_STEM := $(FIRMWARE)$(if $(NAME_SLUG),@$(NAME_SLUG))

# these travel as -D<name>='"value"', so the same shell-hostile characters are out; the
# firmware also restricts region names to alphanumerics, '-' and a leading '#' or '$'
$(foreach v,$(STRING_ARGS),$(if $($(v)),\
  $(if $(shell printf %s '$($(v))' | grep -qE '^[A-Za-z0-9,#_-]+$$' || echo bad),\
    $(error $(v)='$($(v))' must be names of letters, digits, '-', '_' or '#', comma separated))))

# non-empty when $(1) holds a character that would not survive the trip through the
# shell and into a -D flag intact. a comma is included because make itself splits
# function arguments on it, which would quietly defeat the check rather than producing
# an error.
comma := ,
key_is_unsafe = $(findstring ',$(1))$(findstring ",$(1))$(findstring `,$(1))$(findstring $$,$(1))$(findstring \,$(1))$(findstring $(comma),$(1))

# enough to tell two keys apart without putting either in the build log
key_fingerprint = $(shell printf %s '$(1)' | shasum -a 256 2>/dev/null | cut -c1-8)

$(foreach v,$(NUMERIC_ARGS),$(if $($(v)),$(if $(call not_a_number,$($(v))),\
  $(error $(v) must be a non-negative integer, got '$($(v))'))))

# ----------------------------------------------------------------------- key files

KEYS_DIR ?= keys
TRACKING_KEY_FILE ?= $(KEYS_DIR)/tracking.key

# A key file is one line of 32 hex characters. Lines starting with # are ignored, so a
# generated file can carry a header saying what it is and when it was made.
# NOTE: the \# is not decoration - an unescaped # would end the make variable here, and
# the truncated value goes to the shell as an unterminated quote.
key_file_cmd = grep -v '^[[:space:]]*\#' '$(1)' | grep -m1 '[^[:space:]]' | sed -e 's/^[[:space:]]*//' -e 's/[[:space:]]*$$//'
read_key_file = $(shell $(call key_file_cmd,$(1)) 2>/dev/null)

# fingerprint a file without the key itself ever appearing in a command line, where ps
# would show it to every other user on the machine.
# the tr matters: sed leaves a trailing newline, and hashing that gives a file-held key a
# different fingerprint from the identical key passed inline - which is exactly the false
# mismatch these fingerprints exist to rule out.
key_file_fingerprint = $(shell $(call key_file_cmd,$(1)) 2>/dev/null | tr -d '\n' | shasum -a 256 | cut -c1-8)

# $(1) is the argument name. A key given inline wins; otherwise the key file is read if
# it is there. A MISSING DEFAULT FILE IS NOT AN ERROR - that is just a build with the
# feature compiled out, which is what this repo did before any of this existed. Naming a
# file explicitly is a different statement, so that one has to exist.
define resolve_key_file
ifeq ($$($(1)),)
  ifneq ($$(wildcard $$($(1)_FILE)),)
    $(1) := $$(call read_key_file,$$($(1)_FILE))
    $(1)_SOURCE := $$($(1)_FILE)
    $(1)_FROM_FILE := $$($(1)_FILE)
    ifeq ($$($(1)),)
      $$(error $$($(1)_FILE) holds no key - every line is blank or a # comment)
    endif
  else ifneq ($$(filter command line environment,$$(origin $(1)_FILE)),)
    $$(error $(1)_FILE=$$($(1)_FILE) does not exist - run 'make keys' to generate it)
  endif
else
  ifneq ($$(filter command line environment,$$(origin $(1)_FILE)),)
    $$(error set either $(1) or $(1)_FILE, not both)
  endif
  $(1)_SOURCE := the command line
endif
endef

$(eval $(call resolve_key_file,TRACKING_KEY))

# prefer the file when there is one, so a stored key is never handed to a subshell
key_source_fingerprint = $(strip $(if $($(1)_FROM_FILE),$(call key_file_fingerprint,$($(1)_FROM_FILE)),\
                                                        $(call key_fingerprint,$($(1)))))

# secret, and kept apart from OTHER_ARG_FLAGS so it can never reach a printed summary
KEY_ARG_FLAGS :=
OTHER_ARG_FLAGS := $(foreach v,$(NUMERIC_ARGS) $(RADIO_ARGS),$(if $($(v)),-D$(v)=$($(v))))
OTHER_ARG_FLAGS += $(foreach v,$(STRING_ARGS),$(if $($(v)),-D$(v)='"$($(v))"'))
# Set by the erase-firmware target re-entering make, never by hand: the flag has to be in
# place while make parses, which is well before any recipe runs.
#
# NAME is left out of an erase build on purpose. The erase image formats the filesystem
# and halts before anything reads a name, so compiling one in would change the bytes
# without changing the behaviour - and then every named build of a target would produce a
# different erase image, when one per target is all that is wanted.
ifeq ($(ERASE_BUILD),1)
  OTHER_ARG_FLAGS += -DFLASH_ERASE_BUILD=1
else
  OTHER_ARG_FLAGS += $(NAME_ARG_FLAG)
endif

ifneq ($(TRACKING_KEY),)
  ifneq ($(call key_is_unsafe,$(TRACKING_KEY)),)
    $(error the key from $(TRACKING_KEY_SOURCE) must not contain quotes, backslashes, \
            backticks, commas or $$)
  endif
  KEY_ARG_FLAGS += -DTRACKING_KEY='"$(TRACKING_KEY)"'
endif

# accept the short forms as well as the macro names the firmware uses
ifneq ($(AUTO_ADVERT_LOC_POLICY),)
  ifneq ($(filter NONE SHARE PREFS,$(AUTO_ADVERT_LOC_POLICY)),)
    OTHER_ARG_FLAGS += -DAUTO_ADVERT_LOC_POLICY=ADVERT_LOC_$(AUTO_ADVERT_LOC_POLICY)
  else ifneq ($(filter ADVERT_LOC_NONE ADVERT_LOC_SHARE ADVERT_LOC_PREFS,$(AUTO_ADVERT_LOC_POLICY)),)
    OTHER_ARG_FLAGS += -DAUTO_ADVERT_LOC_POLICY=$(AUTO_ADVERT_LOC_POLICY)
  else
    $(error AUTO_ADVERT_LOC_POLICY must be one of NONE, SHARE, PREFS, got '$(AUTO_ADVERT_LOC_POLICY)')
  endif
endif

# combinations that build cleanly but don't do what they look like they do
ifeq ($(TRACKING_KEY),)
  ifeq ($(TRACK_REPORT),1)
    $(error TRACK_REPORT=1 needs TRACKING_KEY, without it the tracking code is not \
            compiled in and the node reports nothing)
  endif
endif
ifneq ($(strip $(AUTO_ADVERT_LOC_MIN_SECS)$(AUTO_ADVERT_LOC_MAX_SECS)$(AUTO_ADVERT_LOC_DIST_M)$(AUTO_ADVERT_LOC_BACKOFF)$(AUTO_ADVERT_LOC_JITTER_PCT)$(AUTO_ADVERT_LOC_STARTUP_SECS)),)
  ifneq ($(AUTO_ADVERT_LOC),1)
    $(warning the location beacon is tuned but not enabled, add AUTO_ADVERT_LOC=1)
  endif
endif

# values the device would clamp on the way in, which is silent once flashed
check_secs = $(if $(shell test $(2) -le $(MAX_SANE_SECS) || echo bad),\
  $(warning $(1)=$(2) exceeds the firmware limit of $(MAX_SANE_SECS)s and will be clamped))
$(foreach v,AUTO_ADVERT_LOC_MIN_SECS AUTO_ADVERT_LOC_MAX_SECS AUTO_ADVERT_LOC_STARTUP_SECS \
            TRACK_SAMPLE_MIN_SECS TRACK_SAMPLE_MAX_SECS,\
  $(if $($(v)),$(call check_secs,$(v),$($(v)))))

ifneq ($(strip $(AUTO_ADVERT_LOC_MIN_SECS)$(AUTO_ADVERT_LOC_MAX_SECS)),)
  ifneq ($(shell test "$(or $(AUTO_ADVERT_LOC_MAX_SECS),3600)" -ge "$(or $(AUTO_ADVERT_LOC_MIN_SECS),60)" || echo bad),)
    $(warning AUTO_ADVERT_LOC_MAX_SECS is below AUTO_ADVERT_LOC_MIN_SECS, the device \
              will raise it to the floor and the backoff will never engage)
  endif
endif
ifneq ($(AUTO_ADVERT_LOC_JITTER_PCT),)
  ifneq ($(shell test $(AUTO_ADVERT_LOC_JITTER_PCT) -le $(MAX_JITTER_PCT) || echo bad),)
    $(warning AUTO_ADVERT_LOC_JITTER_PCT=$(AUTO_ADVERT_LOC_JITTER_PCT) will be clamped to $(MAX_JITTER_PCT))
  endif
endif

# appended to whatever the caller already exported, which build.sh preserves as the
# base it re-applies per target.
# NOTE: deliberately not $(strip)ped - that collapses runs of whitespace, which would
# quietly rewrite a passphrase containing two consecutive spaces into a different key.
ifneq ($(strip $(KEY_ARG_FLAGS)$(OTHER_ARG_FLAGS)),)
  export PLATFORMIO_BUILD_FLAGS := $(PLATFORMIO_BUILD_FLAGS) $(KEY_ARG_FLAGS) $(OTHER_ARG_FLAGS)
endif

# -------------------------------------------------------------- per-command help
#
# 'make <command> help' explains that command instead of running it: what it would do
# with the arguments as they stand, and every setting it reads, with the value that
# setting has now and what its default means. Nothing is written to a device, so it is
# also the way to see what a flash or an erase is about to do before committing to it.
#
# Order does not matter - 'make help flash' and 'make flash help' are the same thing.

HELP_FOR := $(if $(filter help,$(MAKECMDGOALS)),$(filter-out help,$(MAKECMDGOALS)))

define ABOUT_firmware
Build one target and copy every artifact it produced into BINARIES_DIR, alongside a
matching erase image. Nothing is sent to a device. The UF2 is what later lets
"make flash" and "make erase" deliver an image without building anything.
With NAME= the name is compiled in and carried in the filename, as <target>@<name>.uf2,
so builds of one target under different device names sit side by side rather than
overwriting each other. NAME='Dennis,Eric,Mike' builds one firmware per name, in order,
one after the other. The erase image is never named: one per target is all there is,
and only the first build of a target produces it.
endef

define ABOUT_flash
Send firmware to the board attached over USB. Identifies the board first and names it
in a prompt, then reuses the image already built for it rather than building again.
Where several images fit that board it lists them and asks which to use; FIRMWARE= or
NAME= answers that in advance. Building is the fallback, not the first move.
endef

define ABOUT_erase
Wipe the attached board back to a blank filesystem, for a device whose stored state is
too broken to reach the "erase" CLI command. Identifies the board, names it in a
prompt, resets it into its bootloader if it has one, and writes an erase image.
Any erase image built for the same board will do - they all format the same filesystem.
endef

define ABOUT_erase_firmware
Build an erase image for a target and leave it in BINARIES_DIR with an -erase suffix.
Nothing is sent to a device. The build arguments below are the only ones that matter:
an erase image formats the filesystem and halts before any of the rest could apply.
endef

define ABOUT_name
Name a board that is already flashed and running, over its serial CLI, without sending
it anything else. This writes the saved prefs directly, so it renames a device that has
been named before - which a name compiled into a build cannot do. The repeater, room
server and sensor answer that CLI; the companion firmware is named from the app.
endef

define ABOUT_detect
Say which board is attached and how it was recognised. Read-only: the device is never
written to and never reset, so this is the safe thing to run when a flash or an erase
names a board that is not the one expected.
endef

define ABOUT_keys
Generate the tracking key file, and print its fingerprint so two machines can be
checked for agreement. Never writes over a key that already exists: the key IS the
group, so replacing it would cut a node off from every node already flashed.
endef

define ABOUT_flags
Show how every build argument resolves for the build that would run, without building.
Key material is never printed, only fingerprints.
endef

define ABOUT_firmwares
List every name FIRMWARE= accepts. There are a few hundred, so MATCH narrows them.
endef

ABOUT_list = $(ABOUT_firmwares)

# Exported so the recipe can echo one by name without the shell having to survive the
# quotes and line breaks in it - the same trick HELP_TEXT uses. A shell cannot name a
# variable with a dash in it, which is why erase-firmware is spelt with an underscore
# here and the recipe translates.
export ABOUT_firmware ABOUT_flash ABOUT_erase ABOUT_erase_firmware ABOUT_detect \
       ABOUT_name ABOUT_keys ABOUT_flags ABOUT_firmwares ABOUT_list

# Which settings each command reads. Everything that changes the binary is grouped, since
# the commands that build read all of it and the commands that do not read none of it.
BUILD_SETTINGS := NAME BLE_PIN CONFIG $(RADIO_ARGS) $(STRING_ARGS) \
  TRACKING_KEY TRACKING_KEY_FILE KEYS_DIR GPS_ENABLED GPS_INTERVAL \
  TRACK_REPORT TRACK_REPORT_SECS TRACK_SAMPLE_MIN_SECS TRACK_SAMPLE_MAX_SECS \
  TRACK_SAMPLE_DIST_M TRACK_BUFFER TRACK_FLOOD \
  AUTO_ADVERT_SECS AUTO_ADVERT_FLOOD AUTO_ADVERT_LOC AUTO_ADVERT_LOC_POLICY \
  AUTO_ADVERT_LOC_MIN_SECS AUTO_ADVERT_LOC_MAX_SECS AUTO_ADVERT_LOC_DIST_M \
  AUTO_ADVERT_LOC_BACKOFF AUTO_ADVERT_LOC_JITTER_PCT AUTO_ADVERT_LOC_STARTUP_SECS \
  AUTO_ADVERT_LOC_FLOOD FLOOD_MAX_ADVERT

SETTINGS_firmware       := FIRMWARE VERSION BINARIES_DIR WITH_ERASE $(BUILD_SETTINGS)
SETTINGS_flash          := FIRMWARE VERSION BINARIES_DIR PORT YES BUILD $(BUILD_SETTINGS)
SETTINGS_erase          := FIRMWARE VERSION BINARIES_DIR ERASE_UF2 PORT YES
SETTINGS_erase_firmware := FIRMWARE VERSION BINARIES_DIR
SETTINGS_name           := NAME PORT YES
SETTINGS_detect         := PORT
SETTINGS_keys           := KEYS_DIR TRACKING_KEY_FILE
SETTINGS_flags          := $(BUILD_SETTINGS)
SETTINGS_firmwares      := MATCH
SETTINGS_list           := MATCH

# What it would do right now, for the commands that act on a device. Same code path as
# the run itself, so the plan cannot describe something the run would not do.
PLAN_flash  = $(DEVICE) plan --mode flash --firmware '$(FIRMWARE)' --version '$(VERSION)' \
                --binaries '$(BINARIES_DIR)' --port '$(PORT)' --name $(call shq,$(NAME)) $(FORCE_BUILD)
PLAN_erase  = $(DEVICE) plan --mode erase --firmware '$(FIRMWARE)' --version '$(VERSION)' \
                --binaries '$(BINARIES_DIR)' --image '$(ERASE_UF2)' --port '$(PORT)'
PLAN_detect = $(DEVICE) detect --port '$(PORT)'
PLAN_name   = $(DEVICE) detect --port '$(PORT)'

# What each setting means when nothing is passed. The numbers mirror the firmware headers
# they come from - examples/companion_radio/AutoAdvert.h - so a
# default changed there and not here is a bug, the same as with MAX_SANE_SECS above.
# No apostrophes in this text: every line of it goes through the shell as printf argument.
MEANS_FIRMWARE      := the attached board decides which target to use
MEANS_VERSION       := nothing can be built - only images already built are used
MEANS_BINARIES_DIR  := where make firmware writes images and flash and erase look
MEANS_PORT          := the one attached device - pio chooses when several are
MEANS_YES           := the Continue? prompt is asked - no terminal means it stops
MEANS_BUILD         := an image already built for the board is reused
MEANS_ERASE_UF2     := an image is looked up for the board rather than handed over
MEANS_WITH_ERASE    := the erase image is built only when the target has none yet
MEANS_MATCH         := every target is listed
MEANS_NAME          := the board keeps the name it has - a fresh one names itself
MEANS_BLE_PIN       := the variant pin - a board with a screen makes one per boot
MEANS_CONFIG        := configs/boston.conf - CONFIG= builds the stock settings
MEANS_LORA_FREQ     := from the config file - the variant setting without one
MEANS_LORA_BW       := from the config file - the variant setting without one
MEANS_LORA_SF       := from the config file - the variant setting without one
MEANS_LORA_CR       := from the config file - the variant setting without one
MEANS_LORA_TX_POWER := the variant setting - what the PA on that board supports
MEANS_DEFAULT_REGIONS := a repeater starts with no region and floods nothing
MEANS_DEFAULT_FLOOD_SCOPE_NAME := a room server or sensor starts with no scope
MEANS_TRACKING_KEY  := position reporting is compiled out entirely
MEANS_TRACKING_KEY_FILE := read when it exists - missing is not an error
MEANS_KEYS_DIR      := where make keys writes and the key file is looked for
MEANS_GPS_ENABLED   := firmware default - on for a companion off for a repeater
MEANS_GPS_INTERVAL  := firmware default 0 - the sensor cadence of once a second
MEANS_TRACK_REPORT  := firmware default 0 - tracks are displayed but not reported
MEANS_TRACK_REPORT_SECS := firmware default 300s - a constant rate hides movement
MEANS_TRACK_SAMPLE_MIN_SECS := firmware default 60s - fastest sampling while moving
MEANS_TRACK_SAMPLE_MAX_SECS := firmware default 3600s - slowest once parked
MEANS_TRACK_SAMPLE_DIST_M := firmware default 100m of travel forces a sample
MEANS_TRACK_BUFFER  := firmware default 48 samples - 12 bytes of RAM each
MEANS_TRACK_FLOOD   := firmware default 0 - zero-hop
MEANS_AUTO_ADVERT_SECS := firmware default 0 - no periodic advert
MEANS_AUTO_ADVERT_FLOOD := firmware default 0 - zero-hop
MEANS_AUTO_ADVERT_LOC := firmware default 0 - the location beacon is off
MEANS_AUTO_ADVERT_LOC_POLICY := follows the beacon - NONE while off SHARE once on
MEANS_AUTO_ADVERT_LOC_MIN_SECS := firmware default 60s - the floor while moving
MEANS_AUTO_ADVERT_LOC_MAX_SECS := firmware default 3600s - the ceiling once parked
MEANS_AUTO_ADVERT_LOC_DIST_M := firmware default 100m - under 30m reads GPS noise as travel
MEANS_AUTO_ADVERT_LOC_BACKOFF := firmware default 2x per stationary send
MEANS_AUTO_ADVERT_LOC_JITTER_PCT := firmware default 15% - stops a fleet locking into step
MEANS_AUTO_ADVERT_LOC_STARTUP_SECS := firmware default 0 - each node reports on its first fix
MEANS_AUTO_ADVERT_LOC_FLOOD := firmware default 0 - zero-hop
MEANS_FLOOD_MAX_ADVERT := firmware default 8 hops before a companion stops forwarding

# Where one setting means something different to one command. A build command has to be
# told its target and version; a device command works them out or does without.
MEANS_firmware_FIRMWARE       := required - the target to build
MEANS_firmware_VERSION        := required - the version string baked into the build
MEANS_erase_firmware_FIRMWARE := required - the target to build the erase image from
MEANS_erase_firmware_VERSION  := required - baked in like any other build
MEANS_flash_VERSION           := only needed if nothing is built and it has to build
MEANS_erase_VERSION           := only needed if no erase image exists and it has to build
MEANS_flags_CONFIG            := configs/boston.conf - the profile these flags come from
MEANS_flash_NAME              := every image built for the board is offered - and none is preferred
MEANS_name_NAME               := required - the name to write to the attached board
MEANS_firmware_NAME           := no name compiled in - a comma-separated list builds one each

# single-quote a value for the shell, so a name or a path containing a quote cannot end
# the argument early
shq = '$(subst ','\'',$(1))'

# The value a setting has now, which with nothing passed is its default. Key values are
# never printed - this output is exactly the sort that gets pasted into an issue - so
# those show the same fingerprint 'make flags' does.
setting_value = $(strip $(if $(filter TRACKING_KEY,$(1)),\
  $(if $($(1)),fingerprint $(call key_source_fingerprint,$(1)),(unset)),\
  $(if $($(1)),$($(1)),(unset))))

# Where a value that is not the default came from, so a surprising one can be traced back
# to the thing that set it.
setting_from = $(strip $(if $(filter command line,$(origin $(1))),[cmdline],\
  $(if $(filter $(1)=%,$(CONFIG_SETTINGS)),[$(notdir $(CONFIG))],)))

# $(1) is the setting and $(2) the command reading it, so a per-command meaning can
# override the general one.
setting_means = $(strip $(if $(MEANS_$(2)_$(1)),$(MEANS_$(2)_$(1)),$(MEANS_$(1))))

define setting_row
printf '  %-30s %-18s %s\n' $(call shq,$(1)) \
  $(call shq,$(strip $(call setting_value,$(1)) $(call setting_from,$(1)))) \
  $(call shq,$(call setting_means,$(1),$(2)));
endef

# NOTE: no literal commas anywhere inside this $(if) - make would read them as argument
# separators and silently truncate the text. Commas inside the MEANS_ values are fine:
# only commas written here are parsed. Same for the plan line in the recipe below.
define settings_table
$(if $(SETTINGS_$(1)),\
  echo; echo "settings - the value each has now and what its default means:"; echo; \
  $(foreach s,$(SETTINGS_$(1)),$(call setting_row,$(s),$(1))),:)
endef

ifneq ($(HELP_FOR),)
# Help mode: the commands named alongside 'help' are explained rather than run, so their
# real recipes below are not defined at all. That is the point - a recipe that exists
# cannot be guaranteed not to run, and 'make flash help' must never flash anything.
.PHONY: $(HELP_FOR) help

$(HELP_FOR):
	@$(if $(strip $(ABOUT_$(subst -,_,$@))),:,echo "no command called $@ - run 'make help' for the list"; exit 1)
	@echo; echo "make $@"; echo
	@printf '%s\n' "$$ABOUT_$(subst -,_,$@)"
	@$(call settings_table,$(subst -,_,$@))
	@$(if $(PLAN_$@),echo; echo "what would happen if you ran it now:"; echo; $(PLAN_$@),:)
	@echo

# Already covered by the text above, so it does not print the general help underneath it.
help:
	@:

else

# ------------------------------------------------------------------------- targets

# help rather than a build, so a bare 'make' explains itself instead of failing on a
# missing FIRMWARE
.DEFAULT_GOAL := help

.PHONY: help
help:
	@echo "$$HELP_TEXT"

# One firmware per name in NAME, built in the order given, one after the other. Each name
# goes to its own sub-make, which is what makes every name an ordinary single-name build:
# the validation, the compiled-in -DADVERT_NAME and the @<name> filename all come out of
# the code that was already there, and nothing here has to know about any of it.
#
# A full rebuild per name is unavoidable - the name is a build flag, so pio rebuilds the
# target when it changes - but only the first of them builds an erase image, since the
# rest find the one it left in BINARIES_DIR.
define build_each_name
set -e; \
names=$$(printf '%s' $(call shq,$(NAME)) | tr '$(comma)' '\n' \
         | sed 's/^[[:space:]]*//; s/[[:space:]]*$$//' | awk 'NF && !seen[$$0]++'); \
count=$$(printf '%s\n' "$$names" | wc -l | tr -d ' '); \
echo "building $$count firmwares for $(FIRMWARE), one per name:"; \
printf '%s\n' "$$names" | sed 's/^/  /'; \
echo; \
printf '%s\n' "$$names" | while IFS= read -r one; do \
  echo "---------------------------------------------------------------- $$one"; \
  $(MAKE) --no-print-directory firmware-one NAME="$$one" || exit 1; \
done
endef

.PHONY: firmware
firmware:
	@test -n "$(FIRMWARE)" || { echo "FIRMWARE must be set, e.g: make firmware FIRMWARE=RAK_4631_companion_radio_ble"; exit 1; }
	@test -n "$(VERSION)" || { echo "VERSION must be set, e.g: make firmware FIRMWARE=$(FIRMWARE) VERSION=v1.16.0.4"; exit 1; }
ifneq ($(NAME_IS_LIST),)
	@$(build_each_name)
else
	@$(MAKE) --no-print-directory firmware-one
endif

.PHONY: firmware-one
firmware-one:
# The matching erase image is built FIRST, so that the real firmware is what ends up in
# .pio/build afterwards. Changing FLASH_ERASE_BUILD makes pio rebuild the whole target,
# so whichever runs last owns that directory - and a bare 'pio run -t upload' finding an
# erase image sitting there under the name firmware.zip would be a nasty surprise.
#
# One erase image per target, not one per NAME: it formats the filesystem and halts before
# anything could read a name, so every named build of a target would produce the same file.
# An existing one is therefore kept rather than rebuilt, which is what stops a fleet of
# named builds paying for a second full build each time. WITH_ERASE=1 rebuilds it anyway
# (after a version bump, say), WITH_ERASE=0 skips it entirely.
ifneq ($(WITH_ERASE),0)
	@if [ "$(WITH_ERASE)" != "1" ] && ls "$(BINARIES_DIR)"/$(FIRMWARE)-erase.* >/dev/null 2>&1; then \
	   echo "erase image for $(FIRMWARE) is already in $(BINARIES_DIR) - keeping it (WITH_ERASE=1 rebuilds)"; \
	 else \
	   $(MAKE) --no-print-directory erase-firmware; \
	   echo; \
	 fi
	@echo "now building $(FIRMWARE) itself (WITH_ERASE=0 skips the erase image)"
endif
	sh build.sh build-firmware $(FIRMWARE)
	@mkdir -p $(BINARIES_DIR)
# Every artifact the target produced, not just the DFU package: 'make flash' delivers a
# built image without rebuilding, and which file it can use depends on the board. A UF2
# goes onto a bootloader drive with nothing installed and no port to get wrong, so it is
# the one worth having - and on ESP32 targets there is no .zip to copy at all.
#
# Filed under IMAGE_STEM, which carries NAME when there is one: a name is compiled in, so
# two builds of one target under two names are two different images, and filing both as
# <target> would mean the second silently replaced the first.
	@for ext in uf2 zip bin hex; do \
	   src=.pio/build/$(FIRMWARE)/firmware.$$ext; \
	   [ -f "$$src" ] && cp "$$src" "$(BINARIES_DIR)/$(IMAGE_STEM).$$ext" && \
	     echo "wrote $(BINARIES_DIR)/$(IMAGE_STEM).$$ext"; \
	 done; true
	@merged=.pio/build/$(FIRMWARE)/firmware-merged.bin; \
	 [ -f "$$merged" ] && cp "$$merged" "$(BINARIES_DIR)/$(IMAGE_STEM)-merged.bin" && \
	   echo "wrote $(BINARIES_DIR)/$(IMAGE_STEM)-merged.bin"; true

# generated rather than written by hand, and never regenerated over the top of an
# existing file: the key IS the network, so silently replacing one would cut a node off
# from every other node already flashed. To rotate, delete the file, run this again and
# reflash the whole network - the firmware wires a single key per build, so there is no
# window where old and new nodes can hear each other.
$(TRACKING_KEY_FILE):
	@mkdir -p $(dir $@)
	@chmod 700 $(dir $@)
	@umask 077; { \
	  echo "# MeshCore $(notdir $@), generated $$(date -u +%Y-%m-%dT%H:%M:%SZ)"; \
	  echo "# 32 hex characters: the raw AES-128 key. Every node on this network has"; \
	  echo "# to be built from an identical copy of this file. Do not commit it."; \
	  openssl rand -hex 16 2>/dev/null || head -c 16 /dev/urandom | xxd -p; \
	} > $@
	@chmod 600 $@
	@echo "generated $@"

.PHONY: keys
keys: $(TRACKING_KEY_FILE)
	@echo "tracking key:  $(TRACKING_KEY_FILE) (fingerprint $$($(call key_file_cmd,$(TRACKING_KEY_FILE)) | tr -d '\n' | shasum -a 256 | cut -c1-8))"
	@echo "copy it to every node that may read a position, then compare fingerprints"

.PHONY: flags
flags:
	@echo "config:        $(if $(strip $(CONFIG)),$(CONFIG),none - stock per-variant settings)"
	@echo "tracking key:  $(if $(TRACKING_KEY),$(TRACKING_KEY_SOURCE) (fingerprint $(call key_source_fingerprint,TRACKING_KEY)),unset - position reporting is compiled out)"
	@echo "other flags:   $(if $(strip $(OTHER_ARG_FLAGS)),$(strip $(OTHER_ARG_FLAGS)),none - firmware defaults throughout)"
ifneq ($(NAME_IS_LIST),)
	@printf '%s\n' $(call shq,names:         $(NAME))
	@echo "               one build per name, so the flags above are what each of them gets"
endif

# What is attached, and how it was recognised. Read-only: nothing is written to the device
# and it is never reset, so this is the safe thing to run when a flash or erase names a
# board that is not the one expected.
.PHONY: detect
detect:
	@$(DEVICE) detect --port '$(PORT)'

# A list of names is a build instruction - one firmware per name - and means nothing to a
# command that acts on the single board in front of it. Worth stopping over rather than
# letting a device be named "Dennis,Eric,Mike".
define require_single_name
if [ -n "$(NAME_IS_LIST)" ]; then \
  printf '%s\n' $(call shq,NAME=$(NAME) is a list) \
    "a list builds one firmware per name and is only understood by 'make firmware'." \
    "pass the one name this board should get."; \
  exit 1; \
fi
endef

# Asked before anything is written to a device. The board named in the prompt is the one
# identified over USB, not the one implied by FIRMWARE=, which is what makes it worth
# reading: a mismatch between the two is exactly the mistake being guarded against.
# YES=1 answers it, since a script has no one to ask.
define confirm
if [ "$(YES)" = "1" ]; then \
  echo "$(1) $$DEV_BOARD_NAME."; \
elif [ ! -t 0 ]; then \
  echo "$(1) $$DEV_BOARD_NAME - but there is no terminal to confirm on. re-run with YES=1."; \
  exit 1; \
else \
  printf '$(1) %s. Continue? [y/N] ' "$$DEV_BOARD_NAME"; \
  read -r reply; \
  case "$$reply" in y|Y|yes|YES) ;; *) echo "aborted"; exit 1;; esac; \
fi
endef

# Give the device its name after the image is on it, rather than compiling one image per
# device. NAME is per-device and so is passed on nearly every flash: baking it in would
# mean a full build to change 31 bytes, which is most of the reason to reuse an image at
# all. It also does more this way - the compiled-in name is only the default for a device
# with no saved prefs, while "set name" over the CLI renames a board that already has one.
#
# The repeater, room server and sensor answer that CLI on serial. The companion firmware
# speaks the binary app protocol instead, which nothing here implements, so its name has
# to come from the app or from a build - and saying so is the point: a NAME that quietly
# did nothing would be worse than either.
define name_device
if [ "$$DEV_NAME_TODO" = "1" ] && [ "$$DEV_CLI" = "text" ]; then \
  $(DEVICE) set-name --port "$$DEV_PORT" --name $(call shq,$(NAME)); \
fi
endef

# Said before the prompt rather than after the flash, so it can still be acted on: the
# answer is to abort and add BUILD=1, which is no use as a report of what already happened.
define name_warning
if [ "$$DEV_NAME_TODO" = "1" ] && [ "$$DEV_CLI" != "text" ]; then \
  echo "NOTE: NAME cannot be set on $$DEV_ENV without a build -"; \
  echo "      it answers the app protocol rather than the serial CLI. Continue to flash it"; \
  echo "      unnamed and name it in the app, or abort and add BUILD=1 to compile it in."; \
fi
endef

# Upload firmware to an attached device, reusing the image already built for it.
#
# Building is the fallback rather than the first move: the identified board usually has an
# image in BINARIES_DIR from 'make firmware', and rebuilding it to send the same bytes
# costs minutes. BUILD=1 forces the build, and so does any argument on the command line
# that would change the binary - one passed to a run that reuses an image would otherwise
# look like it had taken effect when it had not. NAME is the exception, handled above.
#
# The delivery route depends on the board and on what was built for it; tools/device.py
# picks it. ESP32 is the one that cannot always be served from BINARIES_DIR, because an
# application image needs its bootloader and partition table flashed alongside it.
FORCE_BUILD := $(if $(CMDLINE_BUILD_ARGS)$(filter 1,$(BUILD)),--force-build)

.PHONY: flash
flash:
ifneq ($(CMDLINE_BUILD_ARGS),)
	@echo "building rather than reusing an image: $(CMDLINE_BUILD_ARGS) given on the command line"
endif
	@set -e; \
	 $(call require_single_name); \
	 eval "$$($(DEVICE) resolve --mode flash --firmware '$(FIRMWARE)' --version '$(VERSION)' \
	          --binaries '$(BINARIES_DIR)' --port '$(PORT)' --name $(call shq,$(NAME)) \
	          --interactive $(FORCE_BUILD))"; \
	 if [ -z "$$DEV_ACTION" ]; then \
	   echo "could not work out what to do here - see the error above"; exit 1; fi; \
	 if [ -n "$$DEV_ERROR" ]; then printf '%s\n' "$$DEV_ERROR"; exit 1; fi; \
	 if [ -n "$$DEV_WARN" ]; then printf '%s\n' "$$DEV_WARN"; fi; \
	 printf '%s\n' "$$DEV_NOTE"; \
	 $(call name_warning); \
	 $(call confirm,I am flashing); \
	 case "$$DEV_ACTION" in \
	   send) \
	     $(DEVICE) send --image "$$DEV_IMAGE" --family "$$DEV_FAMILY" --mcu "$$DEV_MCU" \
	                    --port "$$DEV_PORT" --volume "$$DEV_VOLUME" --speed "$$DEV_SPEED"; \
	     $(call name_device);; \
	   nobuild) \
	     pio run -e "$$DEV_ENV" -t nobuild -t upload; \
	     $(call name_device);; \
	   build) \
	     $(MAKE) --no-print-directory flash-build FIRMWARE="$$DEV_ENV";; \
	   *) \
	     echo "nothing to do"; exit 1;; \
	 esac

# Name a board that is already flashed and running, without sending it anything else.
# Writes the name straight into the saved prefs over the CLI, so unlike the compiled-in
# default it renames a device that has been named before. Only the targets that answer
# the text CLI on serial can take it - the companion is named from the app.
.PHONY: name
name:
	@test -n "$(NAME)" || { echo "NAME must be set, e.g: make name NAME='Kitchen Repeater'"; exit 1; }
	@set -e; \
	 $(call require_single_name); \
	 DEV_BOARD_NAME=$$($(DEVICE) detect --port '$(PORT)' --name-only); \
	 $(call confirm,I am naming); \
	 $(DEVICE) set-name --port '$(PORT)' --name $(call shq,$(NAME)) --settle 0

# Build and upload in one step. Goes through build.sh rather than calling pio directly so
# that what lands on the device is built from the same flags `make firmware` would use -
# a bare `pio run -t upload` would rebuild without the version, key and config defines.
#
# VERSION is not on the command line below and does not need to be: build.sh takes it
# from FIRMWARE_VERSION in the environment, exported above, the same way `firmware` does.
# The guard here only exists to fail with a clearer message than build.sh's own.
.PHONY: flash-build
flash-build:
	@test -n "$(FIRMWARE)" || { echo "FIRMWARE must be set, e.g: make flash FIRMWARE=RAK_4631_companion_radio_ble VERSION=v1.16.0.4"; exit 1; }
	@test -n "$(VERSION)" || { echo "VERSION must be set, e.g: make flash FIRMWARE=$(FIRMWARE) VERSION=v1.16.0.4"; exit 1; }
	sh build.sh flash-firmware $(FIRMWARE)

# Wipe a device back to a blank filesystem. Two entirely different mechanisms, because
# the chips do not agree on what "erase" means over USB:
#
#   nRF52 / RP2040 - the bootloader presents a USB drive, and an erase image copied onto
#                    it does the work. pio's own erase target is nrfjprog --eraseall,
#                    which needs a J-Link probe, so it is no use to anyone flashing over
#                    a cable. Put the board in bootloader mode first: double-tap reset on
#                    RAK and T114, double-tap the magnetic connector on a T1000-e.
#   ESP32          - esptool can erase over the same serial port it flashes with, so this
#                    needs no bootloader drive but does need FIRMWARE= to pick the env.
#
# Naming ERASE_UF2 skips the lookup in tools/device.py entirely and writes that file. It
# is how a downloaded SoftDevice image gets used deliberately: those are not built here,
# match no target, and wipe more than the filesystem.
ERASE_UF2 ?=

# Build an erase image for a board without sending it anywhere. It is an ordinary
# firmware for that same target with -DFLASH_ERASE_BUILD=1, which formats the filesystem
# as the first thing in setup() and then halts - so it works on a device whose stored
# state is what breaks the boot, and needs no debug probe.
#
# Artifacts land in BINARIES_DIR with an -erase suffix. That suffix is not cosmetic:
# these images are the same size and shape as real firmware, and one filed under the
# plain target name would be indistinguishable from it later.
#
# Re-enters make because ERASE_BUILD has to be set while the flags are assembled at parse
# time. Command line arguments carry through to the sub-make on their own.
.PHONY: erase-firmware
erase-firmware:
	@$(MAKE) --no-print-directory erase-firmware-build ERASE_BUILD=1

.PHONY: erase-firmware-build
erase-firmware-build:
	@test "$(ERASE_BUILD)" = "1" || { echo "use 'make erase-firmware', which sets ERASE_BUILD"; exit 1; }
	@test -n "$(FIRMWARE)" || { echo "FIRMWARE must be set, e.g: make erase-firmware FIRMWARE=RAK_4631_repeater VERSION=v1.16.0.4"; exit 1; }
	@test -n "$(VERSION)" || { echo "VERSION must be set, e.g: make erase-firmware FIRMWARE=$(FIRMWARE) VERSION=v1.16.0.4"; exit 1; }
	sh build.sh build-firmware $(FIRMWARE)
	@mkdir -p $(BINARIES_DIR)
	@for ext in uf2 zip bin hex; do \
	   src=.pio/build/$(FIRMWARE)/firmware.$$ext; \
	   [ -f "$$src" ] && cp "$$src" "$(BINARIES_DIR)/$(FIRMWARE)-erase.$$ext" && \
	     echo "wrote $(BINARIES_DIR)/$(FIRMWARE)-erase.$$ext"; \
	 done; true

# Any erase image built for the same board will do, whichever target it came from: they
# all format the same filesystem and halt before anything else runs. That is why this
# needs no FIRMWARE= once one has been built, while 'make flash' does - there, which
# target is being sent is the whole point.
.PHONY: erase
erase:
	@set -e; \
	 eval "$$($(DEVICE) resolve --mode erase --firmware '$(FIRMWARE)' --version '$(VERSION)' \
	          --binaries '$(BINARIES_DIR)' --image '$(ERASE_UF2)' --port '$(PORT)')"; \
	 if [ -z "$$DEV_ACTION" ]; then \
	   echo "could not work out what to do here - see the error above"; exit 1; fi; \
	 if [ -n "$$DEV_ERROR" ]; then printf '%s\n' "$$DEV_ERROR"; exit 1; fi; \
	 if [ -n "$$DEV_WARN" ]; then printf '%s\n' "$$DEV_WARN"; fi; \
	 printf '%s\n' "$$DEV_NOTE"; \
	 $(call confirm,I am erasing); \
	 case "$$DEV_ACTION" in \
	   build-uf2) \
	     $(MAKE) --no-print-directory erase-firmware FIRMWARE="$$DEV_ENV"; \
	     DEV_IMAGE="$(BINARIES_DIR)/$$DEV_ENV-erase.uf2";; \
	   build-upload) \
	     echo "no bootloader drive on this board - building the erase image and uploading it."; \
	     $(MAKE) --no-print-directory flash-build FIRMWARE="$$DEV_ENV" ERASE_BUILD=1; \
	     DEV_ACTION=done;; \
	   esptool-erase) \
	     pio run -e "$$DEV_ENV" -t erase; \
	     DEV_ACTION=done;; \
	 esac; \
	 if [ "$$DEV_ACTION" != "done" ]; then \
	   $(DEVICE) send --image "$$DEV_IMAGE" --family "$$DEV_FAMILY" --mcu "$$DEV_MCU" \
	                  --port "$$DEV_PORT" --volume "$$DEV_VOLUME" --speed "$$DEV_SPEED"; \
	 fi; \
	 echo; \
	 echo "the board reboots into the erase image and reports on the serial console;"; \
	 echo "when it says OK, flash normal firmware again with 'make flash'."

# Every name accepted as FIRMWARE=, which is every pio env except the host test ones -
# those build and run on this machine and are not flashable firmware.
#
# There are a few hundred, so MATCH takes one or more substrings and narrows by all of
# them at once, in any order: MATCH='rak ble' finds the RAK BLE companion targets.
# The names go to stdout and the count to stderr, so a pipeline gets names only.
#
# Sorted case-insensitively, which is what keeps a board's targets together: pio reports
# them grouped by variant file, and a plain sort would file Heltec_ct62 away from
# Heltec_T190 on the case of one letter.
.PHONY: firmwares
firmwares:
	@list=$$(sh build.sh list | grep -v '^native' | sort -f); \
	 for term in $(MATCH); do \
	   list=$$(printf '%s\n' "$$list" | grep -i -- "$$term" || true); \
	 done; \
	 if [ -z "$$list" ]; then \
	   echo "no firmware matches '$(MATCH)'" >&2; exit 0; \
	 fi; \
	 printf '%s\n' "$$list"; \
	 echo "$$(printf '%s\n' "$$list" | wc -l | tr -d ' ') firmwares$(if $(MATCH), matching '$(MATCH)')" >&2

# kept as the name this had before, since it does the same job
.PHONY: list
list: firmwares

endif  # help mode - see HELP_FOR above
