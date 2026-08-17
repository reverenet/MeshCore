#!/usr/bin/env python3
"""Identify the attached board, choose an image for it, and deliver that image.

`make flash` and `make erase` need the same three answers - which board is on the other
end of the cable, which built image belongs to it, and how that image gets there - so
both go through this one script and cannot drift apart.

The subcommands:

  detect    say what is attached and how it was recognised. Reads only.
  resolve   detect the board, pick the image, and print the result as shell assignments
            for the calling recipe to eval. Never touches the device.
  plan      the same decisions, written out for a person to read before committing.
  send      put one image onto the device, switching it into bootloader mode first when
            that is what the route needs.
  set-name  name a device over its serial CLI, after it has been flashed.

Identification is deliberately conservative. A wrong guess here means writing one board's
firmware onto another, so where the evidence fits several boards this reports all of them
and leaves the choice to FIRMWARE= rather than picking a likely-looking one.

What the evidence is worth, best first:

  INFO_UF2.TXT on a mounted bootloader drive - the board's own bootloader naming itself,
                                               though more loosely than the application:
                                               a Seeed Wio Tracker L1 is "TRACKER L1"
                                               down there. So a drive that identifies
                                               nothing falls through to the serial port
                                               the bootloader is also keeping up.
  The USB product string of a running board  - set from the board manifest, so it matches
                                               a manifest exactly when it is distinctive.
                                               Some are not: four Heltec nRF boards all
                                               enumerate as HT-n5262.
  VID:PID                                    - only ever narrows. 0x239A:0x8029 is generic
                                               Adafruit nRF52840 and 0x303A:0x1001 is every
                                               ESP32-S3, so it identifies a board only when
                                               a manifest claims an id no other one does.

ESP32 boards behind a CP210x or CH340 bridge report the bridge chip and cannot be
identified at all. That is a property of the hardware, not a gap here.
"""

import argparse
import errno
import glob
import json
import os
import re
import shlex
import subprocess
import sys
import time
from pathlib import Path

PROJECT_DIR = Path(__file__).resolve().parent.parent

# how long to wait for a board to come back after being reset into its bootloader
BOOTLOADER_WAIT_SECS = 20


def core_dir():
    return Path(os.environ.get("PLATFORMIO_CORE_DIR") or os.path.expanduser("~/.platformio"))


def pio_python():
    """The interpreter PlatformIO installs its tools against.

    esptool and adafruit-nrfutil both need pyserial, which the system python has no
    reason to have. Falls back to this interpreter so an unusual install still runs.
    """
    candidate = core_dir() / "penv" / "bin" / "python"
    return str(candidate) if candidate.exists() else sys.executable


def norm(text):
    """Compare identity strings without punctuation or case getting in the way.

    'Seeed Wio Tracker L1', 'seeed-wio-tracker-l1' and 'SEEED_WIO_TRACKER_L1' are one
    board written three ways: the manifest id, the USB product string and the drive
    name rarely agree on separators.
    """
    return re.sub(r"[^a-z0-9]", "", (text or "").lower())


def name_slug(name):
    """A device name as it appears in a filename.

    Has to agree with the Makefile's NAME_SLUG, which does the same substitution in sed.
    The filename is the only place the two meet, so a disagreement here would file an
    image under one name and look for it under another.
    """
    return re.sub(r"[^A-Za-z0-9._-]", "_", name or "")


def family_of(mcu):
    mcu = (mcu or "").lower()
    for prefix, family in (("nrf52", "nrf52"), ("esp32", "esp32"),
                           ("rp2", "rp2040"), ("stm32", "stm32")):
        if mcu.startswith(prefix):
            return family
    return ""


# ------------------------------------------------------------------ project inventory

def run_pio(args):
    try:
        out = subprocess.run(["pio"] + args, cwd=PROJECT_DIR, check=True,
                             stdout=subprocess.PIPE, stderr=subprocess.DEVNULL)
    except (OSError, subprocess.CalledProcessError):
        return None
    try:
        return json.loads(out.stdout.decode())
    except ValueError:
        return None


_CONFIG = None


def pio_config():
    """The resolved project config, read once.

    Read from pio rather than parsed here: `board` is set on a base section and reaches
    the env through `extends`, sometimes more than one level, and pio is the only thing
    that resolves that the same way a build does.
    """
    global _CONFIG
    if _CONFIG is None:
        _CONFIG = run_pio(["project", "config", "--json-output"]) or []
    return _CONFIG


def load_envs():
    """{env name: board id} for every flashable target."""
    envs = {}
    for name, options in pio_config():
        if not name.startswith("env:"):
            continue
        board = dict(options).get("board")
        if board:  # the native test envs have none, and are not flashable
            envs[name[4:]] = board
    return envs


def example_dir(env):
    """The examples/ directory a target builds its application from."""
    for name, options in pio_config():
        if name != f"env:{env}":
            continue
        src = dict(options).get("build_src_filter") or []
        for entry in (src if isinstance(src, list) else [src]):
            match = re.search(r"\+<\.\./(examples/[^/>]+)", str(entry))
            if match:
                return PROJECT_DIR / match.group(1)
    return None


def cli_kind(env):
    """Which command interface the firmware built from this target answers on serial.

    'text' is CommonCLI - the repeater, room server and sensor - which takes lines like
    "set name Kitchen" over the USB serial port at 115200. The companion firmware speaks
    a binary frame protocol to the app instead, which nothing here implements, so it
    reports '' rather than pretending it could send a command.

    Worked out from the sources the target builds rather than from its name, so an
    example that gains or loses the CLI is not something to remember to update here.
    """
    directory = example_dir(env)
    if not directory or not directory.is_dir():
        return ""
    for path in list(directory.glob("*.h")) + list(directory.glob("*.cpp")):
        try:
            if "CommonCLI.h" in path.read_text(errors="replace"):
                return "text"
        except OSError:
            pass
    return ""


def load_boards(wanted):
    """Manifests for the given board ids, project copies shadowing platform ones.

    Restricted to ids this project actually builds for. The platform packages carry a
    few hundred more, and every one of those is a board that could be matched into by
    mistake without ever being buildable here.
    """
    dirs = [PROJECT_DIR / "boards"] + [Path(p) for p in
                                       sorted(glob.glob(str(core_dir() / "platforms" / "*" / "boards")))]
    boards = {}
    for directory in dirs:
        for board_id in wanted:
            if board_id in boards:
                continue
            manifest = directory / f"{board_id}.json"
            if manifest.exists():
                try:
                    boards[board_id] = describe_board(board_id, json.loads(manifest.read_text()))
                except ValueError:
                    pass
    return boards


def collect(node, key, found):
    """Every value stored under `key` anywhere in the manifest.

    usb_product and hwids sit under `build` in most manifests and under a nested
    variant block in others, and the nesting is not worth encoding here.
    """
    if isinstance(node, dict):
        for name, value in node.items():
            if name == key:
                found.append(value)
            else:
                collect(value, key, found)
    elif isinstance(node, list):
        for value in node:
            collect(value, key, found)
    return found


def describe_board(board_id, manifest):
    hwids = set()
    for group in collect(manifest, "hwids", []):
        for pair in group:
            if len(pair) == 2:
                hwids.add((pair[0].lower().replace("0x", ""), pair[1].lower().replace("0x", "")))
    upload = manifest.get("upload", {})
    mcu = (manifest.get("build", {}) or {}).get("mcu", "")
    return {
        "id": board_id,
        "name": manifest.get("name") or board_id,
        "mcu": mcu,
        "family": family_of(mcu),
        "products": [p for p in collect(manifest, "usb_product", []) if isinstance(p, str)],
        "hwids": hwids,
        "speed": upload.get("speed", 115200),
    }


# ------------------------------------------------------------------------- the device

def uf2_volumes():
    """Mounted volumes that are a UF2 bootloader, by the marker file it always writes."""
    roots = glob.glob("/Volumes/*") + glob.glob("/media/*/*") + glob.glob("/run/media/*/*")
    return [v for v in sorted(roots) if os.path.isfile(os.path.join(v, "INFO_UF2.TXT"))]


def read_info_uf2(volume):
    fields = {}
    try:
        text = Path(volume, "INFO_UF2.TXT").read_text(errors="replace")
    except OSError:
        return fields
    for line in text.splitlines():
        if ":" in line:
            key, _, value = line.partition(":")
            fields[key.strip().lower()] = value.strip()
    return fields


def serial_ports():
    """Attached USB serial ports, with the VID:PID split out of pio's hwid string."""
    ports = []
    for entry in run_pio(["device", "list", "--json-output"]) or []:
        hwid = entry.get("hwid") or ""
        match = re.search(r"VID:PID=([0-9A-Fa-f]{4}):([0-9A-Fa-f]{4})", hwid)
        if not match:  # onboard bluetooth and debug consoles, not a board
            continue
        ports.append({
            "port": entry.get("port", ""),
            "description": entry.get("description", ""),
            "vid": match.group(1).lower(),
            "pid": match.group(2).lower(),
        })
    return ports


def match_by_name(boards, *names):
    """Boards whose manifest name or USB product string is one of `names`."""
    wanted = {norm(n) for n in names if n}
    if not wanted:
        return []
    hits = []
    for board in boards.values():
        known = {norm(board["name"]), norm(board["id"])}
        known.update(norm(p) for p in board["products"])
        if known & wanted:
            hits.append(board)
    return hits


def match_by_substring(boards, text):
    """Boards whose id or product string appears inside `text`.

    For Board-ID lines, which are compounds like nRF52840-RAK4631-v1 rather than a name.
    """
    haystack = norm(text)
    if len(haystack) < 4:
        return []
    hits = []
    for board in boards.values():
        needles = [norm(board["id"])] + [norm(p) for p in board["products"]]
        if any(len(n) >= 4 and n in haystack for n in needles):
            hits.append(board)
    return hits


def match_by_hwid(boards, vid, pid):
    return [b for b in boards.values() if (vid, pid) in b["hwids"]]


def match_by_partial(boards, text):
    """Boards whose own name contains the whole of `text`.

    A bootloader names itself with a fragment of what the application calls the board -
    "TRACKER L1" where the manifest says "Seeed Wio Tracker L1" - so here the evidence is
    a substring of the manifest rather than the other way round. Long fragments only: a
    three or four character one would match half the catalogue.
    """
    needle = norm(text)
    if len(needle) < 6:
        return []
    return [b for b in boards.values()
            if needle in norm(b["name"]) or any(needle in norm(p) for p in b["products"])]


def identify(boards, *, model=None, board_id=None, volume_name=None,
             description=None, vid=None, pid=None):
    """Narrow the evidence down to one board, or report what it could not separate.

    Returns (board or None, candidates, how). `candidates` is only interesting when it
    holds more than one: that is the ambiguous case the caller has to refuse to guess at.
    """
    hits = match_by_name(boards, model, description, volume_name)
    how = "name"
    if not hits and board_id:
        hits = match_by_substring(boards, board_id)
        how = "board-id"
    if not hits and volume_name:
        hits = match_by_substring(boards, volume_name)
        how = "drive name"
    if not hits and vid:
        hits = match_by_hwid(boards, vid, pid)
        how = "usb id"
    if not hits:
        for text in (model, description, volume_name):
            hits = match_by_partial(boards, text)
            if hits:
                how = "partial name"
                break
    if len(hits) > 1 and vid:
        # a shared product string sometimes still splits on the usb id
        narrowed = [b for b in hits if (vid, pid) in b["hwids"]]
        if len(narrowed) == 1:
            return narrowed[0], narrowed, how + " and usb id"
    if len(hits) == 1:
        return hits[0], hits, how
    return None, hits, how


def identify_from_ports(boards, ports, preferred_port=None):
    """The first attached port that matches a board, or that at least narrows it down."""
    if preferred_port:
        ports = [p for p in ports if p["port"] == preferred_port] or ports
    for entry in ports:
        board, candidates, how = identify(boards, description=entry["description"],
                                          vid=entry["vid"], pid=entry["pid"])
        if board or candidates:
            return {"port": entry["port"], "board": board, "candidates": candidates,
                    "how": how,
                    "detail": f"{entry['description']} ({entry['vid']}:{entry['pid']})"}
    return None


def detect(boards, preferred_port=None):
    """What is attached, preferring a bootloader drive over a running board.

    A mounted drive is both the better evidence and the state the erase route needs, so
    it wins when a board presents itself as both.
    """
    found = {"source": "none", "port": "", "volume": "", "board": None,
             "candidates": [], "how": "", "detail": ""}

    for volume in uf2_volumes():
        info = read_info_uf2(volume)
        board, candidates, how = identify(
            boards,
            model=info.get("model"),
            board_id=info.get("board-id"),
            volume_name=os.path.basename(volume.rstrip("/")),
        )
        found.update(source="uf2", volume=volume, board=board, candidates=candidates, how=how,
                     detail=info.get("model") or os.path.basename(volume.rstrip("/")))
        if board:
            return found

        # A bootloader names itself more loosely than the application does - a Seeed Wio
        # Tracker L1 calls itself "TRACKER L1" down here - and it keeps a serial port up
        # while it does. The drive is still where the image has to go, so hold on to it
        # and let the port settle what the board is.
        from_port = identify_from_ports(boards, serial_ports(), preferred_port)
        if from_port and from_port["board"]:
            found.update(board=from_port["board"], candidates=from_port["candidates"],
                         how=f"{from_port['how']} on {from_port['port']}",
                         port=from_port["port"], detail=from_port["detail"])
        return found

    ports = serial_ports()
    from_port = identify_from_ports(boards, ports, preferred_port)
    if from_port:
        found.update(source="serial", port=from_port["port"], board=from_port["board"],
                     candidates=from_port["candidates"], how=from_port["how"],
                     detail=from_port["detail"])
        return found

    # Nothing matched. Report the first port anyway rather than "no board attached": a
    # board that is plugged in and unrecognised is a different problem to solve than one
    # that is not plugged in, and the description is what tells them apart.
    if ports:
        entry = ports[0]
        found.update(source="serial", port=entry["port"],
                     detail=f"{entry['description']} ({entry['vid']}:{entry['pid']})")
    return found


# ---------------------------------------------------------------------- mode switching

def touch_1200(port):
    """Reset a board into its bootloader by opening the port at 1200 baud and closing it.

    The same signal PlatformIO sends before uploading (use_1200bps_touch in the board
    manifests). HUPCL is what makes the close drop DTR, which is the part the bootloader
    is actually watching for.
    """
    import termios

    fd = os.open(port, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
    try:
        attrs = termios.tcgetattr(fd)
        attrs[2] |= termios.HUPCL
        attrs[4] = termios.B1200
        attrs[5] = termios.B1200
        termios.tcsetattr(fd, termios.TCSANOW, attrs)
    finally:
        os.close(fd)


def wait_for_volume(timeout=BOOTLOADER_WAIT_SECS):
    deadline = time.time() + timeout
    while time.time() < deadline:
        volumes = uf2_volumes()
        if volumes:
            time.sleep(1)  # let the mount settle before writing to it
            return volumes[0]
        time.sleep(0.5)
    return None


def wait_for_port(before, timeout=BOOTLOADER_WAIT_SECS):
    """The bootloader's serial port after a reset.

    It usually enumerates under a different name than the application did, so a new port
    is the one wanted; a port that merely comes back is accepted once nothing else shows.
    """
    known = {p["port"] for p in before}
    deadline = time.time() + timeout
    fallback = None
    while time.time() < deadline:
        now = serial_ports()
        fresh = [p["port"] for p in now if p["port"] not in known]
        if fresh:
            time.sleep(1)
            return fresh[0]
        fallback = fallback or next((p["port"] for p in now if p["port"] in known), None)
        time.sleep(0.5)
    return fallback


# -------------------------------------------------------------------------- delivering

def copy_to_volume(image, volume):
    """Copy a UF2 onto a bootloader drive.

    The board reboots the moment it has the whole image, which pulls the drive out from
    under the copy - so an error at the very end is the success case, not a failure.
    """
    target = os.path.join(volume, os.path.basename(image))
    written = 0
    size = os.path.getsize(image)
    try:
        with open(image, "rb") as src, open(target, "wb") as dst:
            while True:
                chunk = src.read(64 * 1024)
                if not chunk:
                    break
                dst.write(chunk)
                written += len(chunk)
            dst.flush()
            os.fsync(dst.fileno())
    except OSError as exc:
        expected = (errno.EIO, errno.ENOENT, errno.ENODEV, errno.EBUSY, errno.EINVAL)
        if written >= size and exc.errno in expected:
            return True
        if written == 0:
            print(f"could not write to {volume}: {exc}", file=sys.stderr)
            return False
        print(f"the drive went away after {written} of {size} bytes - "
              "if the board does not come back, copy the image again", file=sys.stderr)
    return True


def send_uf2(args, image):
    volume = args.volume or (uf2_volumes() or [None])[0]
    if not volume and args.port and args.family in ("nrf52", "rp2040"):
        print(f"resetting {args.port} into bootloader mode (1200 bps touch)")
        try:
            touch_1200(args.port)
        except OSError as exc:
            print(f"could not open {args.port}: {exc}", file=sys.stderr)
            return 1
        volume = wait_for_volume()
    if not volume:
        print("no UF2 bootloader drive appeared. put the board into bootloader mode by hand "
              "(double-tap reset, or double-tap the magnetic connector on a T1000-e) and "
              "run this again.", file=sys.stderr)
        return 2
    print(f"writing {image.name} to {volume}")
    if not copy_to_volume(str(image), volume):
        return 1
    print("\nimage copied. an error dialog about ejecting the drive is expected and harmless.")
    return 0


def send_dfu(args, image):
    """nRF52 DFU package over the bootloader's serial port."""
    tool = core_dir() / "packages" / "tool-adafruit-nrfutil" / "adafruit-nrfutil.py"
    if not tool.exists():
        print(f"adafruit-nrfutil is not installed at {tool} - build this target once so "
              "PlatformIO installs it, or flash the .uf2 instead.", file=sys.stderr)
        return 1
    port = args.port
    if not port:
        print("no serial port to upload to; pass PORT=", file=sys.stderr)
        return 2
    before = serial_ports()
    print(f"resetting {port} into bootloader mode (1200 bps touch)")
    try:
        touch_1200(port)
    except OSError as exc:
        print(f"could not open {port}: {exc}", file=sys.stderr)
        return 1
    port = wait_for_port(before) or port
    print(f"uploading {image.name} to {port}")
    return subprocess.call([pio_python(), str(tool), "dfu", "serial", "-p", port,
                            "-b", str(args.speed), "--singlebank", "-pkg", str(image)])


def send_esp(args, image):
    """ESP32 merged image, which carries its own bootloader and partition table at 0x0."""
    tool = core_dir() / "packages" / "tool-esptoolpy" / "esptool.py"
    if not tool.exists():
        print(f"esptool is not installed at {tool}", file=sys.stderr)
        return 1
    command = [pio_python(), str(tool)]
    if args.mcu:
        command += ["--chip", args.mcu]
    if args.port:
        command += ["--port", args.port]
    command += ["--baud", "460800", "write_flash", "-z", "0x0", str(image)]
    print(f"uploading {image.name}")
    return subprocess.call(command)


# ------------------------------------------------------------------------- naming

def open_serial(port, baud):
    """A raw serial port, using termios rather than pyserial.

    The only other thing here that opens a port is the 1200 bps touch, which needs
    termios anyway - so doing this the same way keeps the tool running on a plain
    python3 with nothing installed.
    """
    import termios

    fd = os.open(port, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
    speed = getattr(termios, f"B{baud}")
    iflag, oflag, cflag, lflag, _, _, cc = termios.tcgetattr(fd)
    cflag = termios.CREAD | termios.CLOCAL | termios.CS8
    termios.tcsetattr(fd, termios.TCSANOW, [0, 0, cflag, 0, speed, speed, cc])
    return fd


def ask_cli(fd, line, timeout=5):
    """Send one CLI line and collect whatever comes back before the timeout.

    The CLI treats \\r as end of line and ignores \\n entirely (simple_repeater/main.cpp),
    so the terminator is not a detail that can be left to habit.
    """
    import select

    os.write(fd, (line + "\r").encode())
    deadline = time.time() + timeout
    reply = b""
    while time.time() < deadline:
        ready, _, _ = select.select([fd], [], [], deadline - time.time())
        if not ready:
            break
        chunk = os.read(fd, 512)
        reply += chunk
        if b"->" in reply and reply.endswith((b"\n", b"\r")):
            break
    return reply.decode(errors="replace")


def cmd_set_name(args):
    """Name a device over its serial CLI, after it has been flashed.

    This is how a name reaches a board that was flashed with an image someone else built:
    the name compiled into a build is only a default for a device with no saved prefs,
    while the CLI writes prefs directly - so it also renames a board that already has one.
    """
    # The port a board comes back on after a flash is often not the one it went away on,
    # so the named port is waited for first and anything that has appeared is accepted
    # after that. Several ports and no way to tell which is the board is not a guess
    # worth taking: naming the wrong device is the mistake this tool exists to prevent.
    port = ""
    deadline = time.time() + BOOTLOADER_WAIT_SECS
    patience = deadline - BOOTLOADER_WAIT_SECS / 2
    while time.time() < deadline:
        ports = [p["port"] for p in serial_ports()]
        if args.port and args.port in ports:
            port = args.port
            break
        if ports and (not args.port or time.time() > patience):
            if len(ports) > 1:
                print(f"several devices are attached and the one just flashed cannot be "
                      f"told apart from the rest, so the name was NOT set. set it with: "
                      f"make name NAME={shlex.quote(args.name)} PORT=<port>", file=sys.stderr)
                return 1
            port = ports[0]
            break
        time.sleep(0.5)

    if not port:
        print("no serial port came back to set the name on - the device may still be "
              f"booting, and is otherwise fine. set the name with: make name "
              f"NAME={shlex.quote(args.name)}", file=sys.stderr)
        return 1

    time.sleep(args.settle)  # the firmware brings the radio up before it reads the CLI
    try:
        fd = open_serial(port, 115200)
    except OSError as exc:
        print(f"could not open {port}: {exc}", file=sys.stderr)
        return 1
    try:
        for attempt in range(3):
            reply = ask_cli(fd, f"set name {args.name}")
            if "OK" in reply:
                print(f"named {port} {args.name!r}")
                return 0
            if "Error" in reply:
                print(f"the device refused the name: {reply.strip()}", file=sys.stderr)
                return 1
            time.sleep(2)  # still booting, most likely
    finally:
        os.close(fd)
    print(f"no answer from the CLI on {port} - the name was NOT set. the device is "
          "flashed and working; set the name from the app, or try again with "
          f"--port {port}", file=sys.stderr)
    return 1


def cmd_send(args):
    image = Path(args.image)
    if not image.exists():
        print(f"no such image: {image}", file=sys.stderr)
        return 1
    suffix = image.suffix.lower()
    if suffix == ".uf2":
        return send_uf2(args, image)
    if suffix == ".zip":
        return send_dfu(args, image)
    if suffix == ".bin":
        if not image.name.endswith("-merged.bin"):
            print(f"{image.name} is an application image on its own, and flashing it without "
                  "its bootloader and partition table would brick the board. Use the merged "
                  "bin, or build the target.", file=sys.stderr)
            return 1
        return send_esp(args, image)
    print(f"nothing here knows how to send a {suffix or 'file'} image", file=sys.stderr)
    return 1


# ----------------------------------------------------------------------------- resolve

# Deliverable image kinds, best route first: UF2 goes onto a bootloader drive with nothing
# installed and no port to get wrong, the DFU package needs a port, and the ESP32 merged
# bin is last because only a merged one can be flashed on its own at all.
IMAGE_EXTS = (".uf2", ".zip", "-merged.bin")


def image_candidates(binaries_dir, env, erase):
    """The images built for one env, one entry per name it was built under.

    'make firmware' files an unnamed build under <env> and a named one under <env>@<slug>,
    so a fleet of differently-named devices built from one target leaves several images
    side by side. '@' separates the name rather than '-' because '-erase' is already a
    suffix here: a device named "erase" would otherwise produce a file indistinguishable
    from an erase image.

    Erase images are never named - one per target is all there is, since an erase image
    formats the filesystem and halts long before a name could mean anything.
    """
    if erase:
        for ext in IMAGE_EXTS:
            path = Path(binaries_dir, f"{env}-erase{ext}")
            if path.exists():
                return [{"path": path, "env": env, "name": ""}]
        return []

    pattern = re.compile(rf"^{re.escape(env)}(?:@(.+))?({'|'.join(re.escape(e) for e in IMAGE_EXTS)})$")
    by_name = {}
    for path in sorted(Path(binaries_dir).glob(f"{glob.escape(env)}*")):
        match = pattern.match(path.name)
        if match:
            by_name.setdefault(match.group(1) or "", []).append(path)

    found = []
    for name, paths in sorted(by_name.items()):
        paths.sort(key=lambda p: next(i for i, e in enumerate(IMAGE_EXTS) if p.name.endswith(e)))
        found.append({"path": paths[0], "env": env, "name": name})
    return found


def describe_image(candidate):
    return f"{candidate['name'].replace('_', ' ') if candidate['name'] else '(unnamed)'}"


def choose_image(candidates, board_name):
    """Ask which of several built images to send.

    Only reached on a terminal: a run with nothing to ask gets the same list as an error
    instead, since picking one on someone's behalf is picking which node this board
    becomes.
    """
    print(f"Found {len(candidates)} named firmwares for {board_name}, which to use?\n",
          file=sys.stderr)
    for index, candidate in enumerate(candidates, 1):
        print(f"  {index}) {describe_image(candidate):<28} {candidate['path'].name}",
              file=sys.stderr)
    print(file=sys.stderr)

    for _ in range(3):
        print(f"choice [1-{len(candidates)}]: ", end="", file=sys.stderr, flush=True)
        try:
            answer = input().strip()
        except EOFError:
            break
        if answer.isdigit() and 1 <= int(answer) <= len(candidates):
            return candidates[int(answer) - 1]
        print("not one of the choices", file=sys.stderr)
    return None


def emit(values):
    for key, value in values.items():
        print(f"DEV_{key}={shlex.quote(str(value))}")


def describe_candidates(candidates, limit=6):
    """Name the boards that could not be told apart, without printing a wall of them.

    A generic ESP32-S3 USB id matches twenty boards here, and listing all twenty buries
    the one sentence that matters underneath them.
    """
    shown = ", ".join(f"{b['name']} ({b['id']})" for b in sorted(candidates, key=lambda b: b["id"])[:limit])
    if len(candidates) > limit:
        shown += f", and {len(candidates) - limit} more"
    return shown


def build_plan(args):
    """Everything decided about this run, without any of it having happened yet.

    Both 'resolve' and 'plan' come through here: what a plan says would happen has to be
    what the run then does, and the only way to be sure of that is one code path.
    """
    envs = load_envs()
    if not envs:
        return {"ERROR": "could not read the PlatformIO project config - is pio on PATH?"}, None

    boards = load_boards(set(envs.values()))
    found = detect(boards, preferred_port=args.port or None)
    board = found["board"]

    out = {
        "SOURCE": found["source"],
        "PORT": found["port"] or args.port,
        "VOLUME": found["volume"],
        "BOARD": board["id"] if board else "",
        "BOARD_NAME": board["name"] if board else "an unidentified board",
        "FAMILY": board["family"] if board else "",
        "MCU": board["mcu"] if board else "",
        "SPEED": board["speed"] if board else 115200,
        "ENV": "",
        "ENVS": "",
        "IMAGE": "",
        "ACTION": "none",
        "NOTE": "",
        "WARN": "",
        "ERROR": "",
        "HOW": found["how"],
        "DETAIL": found["detail"],
        "CLI": "",
        "NAME_TODO": "",
    }

    # An explicit FIRMWARE= settles which env is built or flashed, but says nothing about
    # what is plugged in - so the board it targets is checked against the one detected.
    env = args.firmware or ""
    if env and env not in envs:
        out["ERROR"] = f"unknown target '{env}' - see 'make firmwares'"
        return out, found

    if env:
        env_board = boards.get(envs[env])
        if env_board:
            out["FAMILY"] = out["FAMILY"] or env_board["family"]
            out["MCU"] = out["MCU"] or env_board["mcu"]
            if not board:
                out["BOARD_NAME"] = env_board["name"]
                out["SPEED"] = env_board["speed"]
            elif board["id"] != env_board["id"]:
                out["WARN"] = (f"WARNING: {env} is built for {env_board['name']}, but the board "
                               f"attached is {board['name']}. Sending it would put the wrong "
                               "image on this board.")
        out["ENV"] = env
    elif board:
        matches = sorted(e for e, b in envs.items() if b == board["id"])
        out["ENVS"] = " ".join(matches)
    elif found["candidates"]:
        out["ERROR"] = ("the attached board matches more than one target board: "
                        f"{describe_candidates(found['candidates'])}.\n"
                        "  they share a USB identity, so pass FIRMWARE=<target> to say which it is.")
        return out, found
    elif found["source"] == "none":
        out["ERROR"] = ("no board found. connect one over USB, or put it into bootloader mode\n"
                        "  (double-tap reset) if it is too broken to enumerate its serial port.")
        return out, found
    else:
        out["ERROR"] = (f"found {found['detail']} on {found['port'] or found['volume']}, but no "
                        "target board matches it.\n"
                        "  pass FIRMWARE=<target> to say what it is - see 'make firmwares'.")
        return out, found

    resolver = resolve_erase if args.mode == "erase" else resolve_flash
    resolver(args, out, envs, boards, found)
    if out["ENV"]:
        out["CLI"] = cli_kind(out["ENV"])
    return out, found


def cmd_resolve(args):
    out, _ = build_plan(args)
    emit(out)
    return 0


# What each action does to the device, for the plan to describe before it happens.
ROUTES = {
    "send.uf2": "copy onto the bootloader drive, resetting the board into it first if needed",
    "send.zip": "reset into the bootloader, then upload the DFU package over serial",
    "send.bin": "esptool write_flash over serial, at offset 0x0",
    "nobuild": "upload the build already in .pio/build with pio, without rebuilding it",
    "build": "build the target from source, then upload it",
    "build-uf2": "build the erase image, then copy it onto the bootloader drive",
    "build-upload": "build the erase image, then upload it over serial",
    "esptool-erase": "esptool erase_flash, which wipes the application as well as the filesystem",
    "none": "nothing",
}


def cmd_plan(args):
    """What the same arguments would do, said out loud and without doing any of it."""
    out, found = build_plan(args)
    verb = "erasing" if args.mode == "erase" else "flashing"

    if out.get("ERROR"):
        print(f"this would not run as things stand:\n  {out['ERROR']}")
        return 0

    action = out["ACTION"]
    key = action + Path(out["IMAGE"]).suffix.lower() if action == "send" else action
    route = ROUTES.get(key, action)

    where = out["VOLUME"] or out["PORT"] or "nothing attached"
    state = "in bootloader mode" if out["SOURCE"] == "uf2" else "running"
    rows = [("board", f"{out['BOARD_NAME']} ({where}, {state})")]
    if out["HOW"]:
        rows.append(("recognised by", f"{out['HOW']} - {out['DETAIL']}"))
    if out["ENV"]:
        rows.append(("target", out["ENV"]))
    if out["IMAGE"]:
        rows.append(("image", out["IMAGE"]))
    rows.append(("route", route))
    if args.name:
        if out["IMAGE"].endswith(f"@{name_slug(args.name)}" + Path(out["IMAGE"]).suffix):
            naming = f"{args.name!r} is already built into the image above"
        elif action == "build":
            naming = f"{args.name!r} compiled into the build as the name a fresh device starts with"
        elif out["CLI"] == "text":
            naming = f"{args.name!r} set over the serial CLI once the board is back up"
        else:
            naming = (f"{args.name!r} CANNOT be applied - {out['ENV']} answers the app protocol"
                      " rather than the serial CLI. BUILD=1 compiles it in instead")
        rows.append(("naming", naming))
    rows.append(("confirm", f'you are asked "I am {verb} {out["BOARD_NAME"]}. Continue?" first'))

    if out["WARN"]:
        print(out["WARN"] + "\n")
    for label, value in rows:
        print(f"  {label + ':':16s}{value}")
    return 0


def resolve_erase(args, out, envs, boards, found):
    # ERASE_UF2 named on the command line wins outright: it is the operator saying which
    # image to use, including a downloaded SoftDevice one this cannot know anything about.
    if args.image:
        out.update(IMAGE=args.image, ACTION="send",
                   NOTE=f"using the image named on the command line: {args.image}")
        return

    # Every target for one board produces the same erase image, so a sibling's image is
    # worth using rather than spending a full build reproducing it. The named target is
    # still tried first, and a substitution is always reported.
    siblings = out["ENVS"].split()
    if out["ENV"] and not siblings:
        siblings = sorted(e for e, b in envs.items() if b == envs.get(out["ENV"]))
    search = ([out["ENV"]] if out["ENV"] else []) + [e for e in siblings if e != out["ENV"]]

    for env in search:
        images = image_candidates(args.binaries, env, erase=True)
        if images:
            note = f"using the erase image built for {env}"
            if out["ENV"] and env != out["ENV"]:
                note += f" - any erase image for this board formats the same filesystem"
            out.update(ENV=env, IMAGE=str(images[0]["path"]), ACTION="send", NOTE=note)
            return

    downloaded = sorted(glob.glob(str(PROJECT_DIR / "tools" / "flash_erase" / "*.uf2")))
    if downloaded and out["FAMILY"] in ("nrf52", "rp2040"):
        out.update(IMAGE=downloaded[0], ACTION="send",
                   NOTE=f"using the downloaded image {os.path.basename(downloaded[0])}",
                   WARN=(out["WARN"] + "\n" if out["WARN"] else "") +
                        "note: a downloaded SoftDevice image is not built for this board and "
                        "wipes more than the filesystem.")
        return

    # Any target for this board gives the same erase image, so the first is as good as
    # any - and the list is sorted, so the same board always builds from the same target.
    env = out["ENV"] or (out["ENVS"].split() or [""])[0]
    if not env:
        out["ERROR"] = "no target to build an erase image from - pass FIRMWARE=<target>"
        return
    if not args.version:
        # esptool can erase a running ESP32 over the same port it flashes; nRF52 and
        # RP2040 have no equivalent without a debug probe.
        if out["FAMILY"] == "esp32":
            out.update(ENV=env, ACTION="esptool-erase",
                       NOTE=f"no erase image built for {env} - falling back to esptool's own "
                            "flash erase, which wipes the application too")
        else:
            out["ERROR"] = (f"no erase image built for {env}, and no VERSION= to build one with.\n"
                            f"  build one: make erase FIRMWARE={env} VERSION=<ver>")
        return
    out.update(ENV=env,
               ACTION="build-uf2" if out["FAMILY"] in ("nrf52", "rp2040") else "build-upload",
               NOTE=f"no erase image built for {env} yet - building one")


def which_target(envs_to_try, doing):
    """The one env to act on, or a message saying why it cannot be settled.

    A repeater, a companion and a room server are all built for the same board and are
    not interchangeable the way erase images are, so this never picks between them.
    """
    if len(envs_to_try) == 1:
        return envs_to_try[0], ""
    if not envs_to_try:
        return "", "no target for this board - pass FIRMWARE=<target>, see 'make firmwares'"
    return "", (f"more than one target is built for this board:\n" +
                "\n".join(f"    {env}" for env in envs_to_try) +
                f"\n  pass FIRMWARE=<target> to say which to {doing}.")


def resolve_flash(args, out, envs, boards, found):
    envs_to_try = [out["ENV"]] if out["ENV"] else out["ENVS"].split()

    if args.force_build:
        env, problem = which_target(envs_to_try, "build")
        out["ERROR"] = problem
        if env:
            out.update(ENV=env, ACTION="build", NOTE=f"building {env}")
        return

    # Everything already built for this board: several targets can fit it, and each target
    # can have been built once per device name.
    ready = []
    for env in envs_to_try:
        ready += image_candidates(args.binaries, env, erase=False)

    # NAME= given at flash time picks the image built under that name, when there is one.
    # Only then is it a selector; with no image of that name it stays what it was, a name
    # to set on the device after the flash.
    if args.name and len(ready) > 1:
        slug = name_slug(args.name)
        exact = [c for c in ready if c["name"] == slug]
        if exact:
            ready = exact

    if len(ready) > 1:
        chosen = choose_image(ready, out["BOARD_NAME"]) if args.interactive and sys.stdin.isatty() else None
        if not chosen:
            width = max(len(c["path"].name) for c in ready)
            out["ERROR"] = ("more than one firmware is built for this board:\n" +
                            "\n".join(f"    {c['path'].name:<{width}}  {describe_image(c)}"
                                      for c in ready) +
                            "\n  pass FIRMWARE=<target> or NAME=<name> to say which to flash"
                            "\n  (or run this on a terminal, where it asks).")
            return
        ready = [chosen]

    if ready:
        chosen = ready[0]
        # A .pio/build directory left by 'make erase-firmware' holds an erase image under
        # the ordinary firmware name. Anything in BINARIES_DIR carries the -erase suffix
        # when that is what it is, which is why only BINARIES_DIR is trusted here.
        # A name already compiled into the chosen image needs nothing done after the
        # flash; one that is not is still to be set, and NAME_TODO is what says which.
        if args.name and chosen["name"] != name_slug(args.name):
            out["NAME_TODO"] = "1"
        out.update(ENV=chosen["env"], IMAGE=str(chosen["path"]), ACTION="send",
                   NOTE=f"flashing {chosen['path'].name} from {args.binaries}, "
                        "already built (BUILD=1 builds it again instead)")
        return

    env, problem = which_target(envs_to_try, "build")
    if not env:
        out["ERROR"] = problem
        return

    # ESP32 has no self-contained prebuilt route: the application image needs the
    # bootloader and partition table flashed alongside it at their own offsets. pio can
    # upload the ones in .pio/build without rebuilding, when that directory is warm.
    if out["FAMILY"] == "esp32" and Path(PROJECT_DIR, ".pio", "build", env, "firmware.bin").exists():
        out["NAME_TODO"] = "1" if args.name else ""
        out.update(ENV=env, ACTION="nobuild",
                   NOTE=f"uploading the {env} build already in .pio/build, without rebuilding")
        return

    if not args.version:
        out["ERROR"] = (f"nothing built for {env} in {args.binaries}, and no VERSION= to build "
                        "it with.\n"
                        f"  build it: make flash FIRMWARE={env} VERSION=<ver>")
        return
    out.update(ENV=env, ACTION="build",
               NOTE=f"nothing built for {env} in {args.binaries} - building it")


def cmd_detect(args):
    envs = load_envs()
    boards = load_boards(set(envs.values()))
    found = detect(boards, preferred_port=args.port or None)
    board = found["board"]
    if args.name_only:
        print(board["name"] if board else "an unidentified board")
        return 0
    if board:
        where = found["volume"] or found["port"]
        print(f"{board['name']} ({board['id']}, {board['mcu']}) on {where}")
        print(f"  identified by:  {found['how']} - {found['detail']}")
        print(f"  state:          {'bootloader drive mounted' if found['source'] == 'uf2' else 'running, serial port up'}")
        targets = sorted(e for e, b in envs.items() if b == board["id"])
        print(f"  targets:        {', '.join(targets) if targets else 'none'}")
    elif found["candidates"]:
        print(f"could not separate {found['detail']} from: {describe_candidates(found['candidates'])}")
    elif found["source"] == "none":
        print("no board attached")
    else:
        print(f"found {found['detail']} on {found['port'] or found['volume']}, "
              "matching no board this project builds for")
    return 0


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    sub = parser.add_subparsers(dest="command", required=True)

    detect_cmd = sub.add_parser("detect", help="report what is attached")
    detect_cmd.add_argument("--port", default="")
    detect_cmd.add_argument("--name-only", action="store_true",
                            help="print just the board name, for a prompt to use")
    detect_cmd.set_defaults(func=cmd_detect)

    resolve_cmd = sub.add_parser("resolve", help="choose an image, print shell assignments")
    resolve_cmd.add_argument("--mode", choices=("flash", "erase"), required=True)
    resolve_cmd.add_argument("--firmware", default="")
    resolve_cmd.add_argument("--version", default="")
    resolve_cmd.add_argument("--binaries", default="../binaries")
    resolve_cmd.add_argument("--image", default="", help="an image named explicitly")
    resolve_cmd.add_argument("--port", default="")
    resolve_cmd.add_argument("--name", default="",
                             help="picks the image built under this name when there is one")
    resolve_cmd.add_argument("--force-build", action="store_true",
                             help="build the target rather than reusing a built image")
    resolve_cmd.add_argument("--interactive", action="store_true",
                             help="ask which image to use when several are built")
    resolve_cmd.set_defaults(func=cmd_resolve)

    plan_cmd = sub.add_parser("plan", help="say what the same arguments would do, and stop")
    for name, kwargs in (("--mode", {"choices": ("flash", "erase"), "required": True}),
                         ("--firmware", {"default": ""}), ("--version", {"default": ""}),
                         ("--binaries", {"default": "../binaries"}), ("--image", {"default": ""}),
                         ("--port", {"default": ""}), ("--name", {"default": ""})):
        plan_cmd.add_argument(name, **kwargs)
    plan_cmd.add_argument("--force-build", action="store_true")
    plan_cmd.set_defaults(func=cmd_plan)

    name_cmd = sub.add_parser("set-name", help="name a device over its serial CLI")
    name_cmd.add_argument("--name", required=True)
    name_cmd.add_argument("--port", default="")
    name_cmd.add_argument("--settle", type=float, default=3.0,
                          help="seconds to let the firmware boot before asking")
    name_cmd.set_defaults(func=cmd_set_name)

    send_cmd = sub.add_parser("send", help="put one image onto the device")
    send_cmd.add_argument("--image", required=True)
    send_cmd.add_argument("--family", default="")
    send_cmd.add_argument("--mcu", default="")
    send_cmd.add_argument("--port", default="")
    send_cmd.add_argument("--volume", default="")
    send_cmd.add_argument("--speed", default=115200)
    send_cmd.set_defaults(func=cmd_send)

    args = parser.parse_args()
    return args.func(args)


if __name__ == "__main__":
    sys.exit(main())
