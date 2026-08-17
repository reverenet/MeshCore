# Flash erase images

An erase image wipes a board's filesystem back to blank, for a device whose stored state
is too broken to reach the `erase` CLI command or CLI rescue mode. There are two ways to
get one.

## Build it (no download needed)

`make firmware` already builds one alongside every firmware it produces, so the usual
case needs nothing extra - a bare `make erase` identifies the attached board, finds its
image in `BINARIES_DIR` and applies it without rebuilding. Any erase image built for that
board will do, whichever target it came from: they all format the same filesystem. To
build one on its own:

```
make erase-firmware FIRMWARE=RAK_4631_repeater VERSION=v1.17.0   # build only
make erase FIRMWARE=RAK_4631_repeater VERSION=v1.17.0            # build and apply
```

This is ordinary firmware for that same target, built with `-DFLASH_ERASE_BUILD=1`. It
formats the filesystem as the first thing in `setup()` - before the radio, the display,
and before anything reads stored state - then reports on the serial console and halts.
Running first is the point: the devices that need this are the ones where loading a
corrupt file is what crashes the boot. Halting is also why it is safe to leave on a
board: it never starts the mesh and never erases twice.

Built artifacts go to `BINARIES_DIR` (default `../binaries`) with an `-erase` suffix, so
they cannot be mistaken for real firmware later.

There is one erase image per target, and never one per device name. `make firmware
NAME='Kitchen Repeater'` files its firmware as `<target>@Kitchen_Repeater.uf2` because a
name is compiled in, but an erase image formats the filesystem and halts long before
anything reads a name - so it is built once for the target and kept, and later named
builds reuse it rather than paying for a second full build each time. `WITH_ERASE=1`
rebuilds it, which is what a version bump wants.

What it does **not** do: it formats the filesystem, and does not touch the bootloader or
the SoftDevice. It cannot revive a board whose bootloader is damaged - though neither can
a downloaded UF2, which needs that same bootloader to be copied in at all.

## Or download one

Images from <https://flasher.meshcore.io> work too, and are what MeshCore's own FAQ
recommends. Drop one in this directory and `make erase` uses it when there is no image
built for the specific target in `BINARIES_DIR`:

| Board | Image |
| --- | --- |
| RAK WisBlock, Heltec T114 | `Flash_erase-nRF32_softdevice_v6.uf2` |
| Seeed Studio Xiao nRF52 WIO | `Flash_erase-nRF52_softdevice_v7.uf2` |

They are not committed here. If you keep more than one, `make erase` takes the first
alphabetically, so pass `ERASE_UF2=` to choose - which also skips the lookup entirely, and
is the honest way to use an image that matches no target. RP2040 boards use the standard
`flash_nuke.uf2` from Raspberry Pi. These are named after the SoftDevice rather than
MeshCore, so they are lower-level than a built image and may wipe more.

## Applying it

`make erase` works out what is attached before it writes anything, and says so:

```
$ make erase
using the erase image built for WioTrackerL1_companion_radio_ble
I am erasing Seeed Wio Tracker L1. Continue? [y/N]
```

The board in that prompt is the one identified over USB, not the one implied by
`FIRMWARE=` - so a cable in the wrong board shows up there. `YES=1` answers it for a
script; without a terminal to ask on, the run stops rather than guessing. `make detect`
prints the same identification without touching the device.

`make erase help` goes further and describes the whole run before any of it happens - the
board, the image it would use, the route it would take, and every setting it reads with
the value that setting has now:

```
$ make erase help
...
  board:          Seeed Wio Tracker L1 (/dev/cu.usbmodem101, running)
  image:          ../binaries/WioTrackerL1_companion_radio_ble-erase.uf2
  route:          copy onto the bootloader drive, resetting the board into it first if needed
```

nRF52 and RP2040 boards are reset into their bootloader automatically - the same 1200 bps
touch PlatformIO uses before an upload - and the image is copied onto the drive that
appears. An error dialog about that drive disappearing mid-copy is expected: the board
reboots as soon as it has the image. A board too broken to enumerate its serial port
cannot be reset this way and has to be put into bootloader mode by hand (double-tap reset,
or double-tap the magnetic connector on a T1000-e) before running the command.

ESP32 boards have no bootloader drive. `make erase FIRMWARE=<target> VERSION=<ver>`
uploads a built erase image over serial; without a VERSION to build from it falls back to
esptool's own `erase_flash`, which wipes the whole chip including the app.

Identification is exact for most nRF52 boards and unavailable for ESP32 boards behind a
CP210x or CH340 bridge, which report the bridge chip rather than the board. Where the
evidence fits several boards - four Heltec nRF boards all enumerate as `HT-n5262` - the
command lists them and stops, and `FIRMWARE=<target>` settles it. The reasoning, and what
each piece of evidence is actually worth, is written up at the top of
[`tools/device.py`](../device.py).

See also: [MeshCore FAQ 6.7](../../docs/faq.md), and `make help`.
