#!/bin/sh
#
# The channel key file - keys/channels.key under REVERENET_HOME - and the three things the build
# needs from it: resolve the named channels into the spec the firmware parses, generate a
# key for a channel that has none, and fingerprint them so two machines can be compared.
#
# The names come from CHANNELS in the network profile, which is shared and committed. The
# keys live here, which is not: a channel key IS the channel, so anyone holding it can
# read and send everything on it. See the CHANNELS argument in the Makefile.
#
#   channel_keys.sh spec     <keyfile> <names>   name:hex,name:hex - or what is missing
#   channel_keys.sh generate <keyfile> <names>   add a key for each name that has none
#   channel_keys.sh print    <keyfile> <names>   one fingerprint line per channel
#
# <names> is the comma-separated CHANNELS value. Output is one line, and 'spec' answers
# in one of three shapes so a Makefile can tell them apart without $(shell) exit codes,
# which GNU make 3.81 - the make macOS ships - cannot see:
#
#   name:hex,name:hex     every channel resolved
#   missing: a b          those names have no key yet, run 'make keys'
#   error: ...            the file itself is wrong, and no build should go ahead
#
# FILE FORMAT: NAME = KEY, one per line, # comments and blank lines ignored. Whitespace
# around both is stripped, so a passphrase cannot begin or end with a space. A key is:
#
#   32 hex characters   the raw AES-128 key, which is what 'generate' writes
#   base64 ending in =  a PSK as MeshCore shares them, e.g. izOH6cXN6mrJ5e26oRXNcg==
#   anything else       a passphrase, hashed into a key with SHA-256
#
# The padding is what tells a base64 PSK from a passphrase, so a passphrase must not end
# in '='. 128 bits and no more: CMD_GET_CHANNEL and CMD_SET_CHANNEL carry 16 bytes of
# secret, so a 256-bit channel is one the companion app cannot read back or write.

set -u

MODE="${1:-}"
KEYFILE="${2:-}"
NAMES="${3:-}"

if [ -z "$MODE" ] || [ -z "$KEYFILE" ]; then
  echo "usage: $0 spec|generate|print <keyfile> <names>" >&2
  exit 2
fi

fail() { echo "error: $*"; exit 1; }

sha256_hex() {
  if command -v shasum > /dev/null 2>&1; then
    shasum -a 256
  else
    sha256sum
  fi | cut -d' ' -f1
}

# 16 random bytes as hex. openssl on any machine that has it, /dev/urandom otherwise -
# the same pair the tracking key recipe uses.
random_key() {
  openssl rand -hex 16 2>/dev/null || od -An -N16 -tx1 /dev/urandom | tr -d ' \n'
}

# every value stored under $1, one per line, so a name given twice can be caught rather
# than silently resolving to whichever line came first
lookup() {
  awk -v want="$1" '
    /^[[:space:]]*#/ { next }
    {
      line = $0
      sub(/^[[:space:]]+/, "", line)
      pos = index(line, "=")            # the FIRST = only: base64 padding is part of the value
      if (pos == 0) next
      name = substr(line, 1, pos - 1)
      val  = substr(line, pos + 1)
      sub(/[[:space:]]+$/, "", name)
      sub(/^[[:space:]]+/, "", val)
      sub(/[[:space:]]+$/, "", val)
      if (name == want) print val
    }' "$KEYFILE"
}

# a key, in whichever of the three forms it was written, as 32 hex characters
normalise() {
  raw="$1"
  name="$2"

  if printf %s "$raw" | grep -qE '^[0-9a-fA-F]{32}$'; then
    printf %s "$raw" | tr 'A-F' 'a-f'
    return
  fi

  case "$raw" in
    *=)
      decoded=$(printf %s "$raw" | openssl base64 -d -A 2>/dev/null | od -An -tx1 | tr -d ' \n')
      [ -n "$decoded" ] || fail "the key for '$name' in $KEYFILE ends in '=' so it is read as base64, but it does not decode"
      case ${#decoded} in
        32) printf %s "$decoded"; return ;;
        64) fail "the key for '$name' in $KEYFILE is 256-bit, and a channel the app can read is 128-bit - use 16 bytes" ;;
        *)  fail "the key for '$name' in $KEYFILE decodes to $((${#decoded} / 2)) bytes, and a channel key is 16" ;;
      esac
      ;;
  esac

  # a passphrase. Hashed once, with no salt and no iteration count, exactly as the
  # tracking key is - so a memorable phrase can be ground out offline from one captured
  # packet, and a phrase is the wrong thing to put here for a channel that matters.
  printf %s "$raw" | sha256_hex | cut -c1-32
}

# enough to tell two channels apart without either key reaching a terminal or a build log
fingerprint() {
  printf %s "$1" | sha256_hex | cut -c1-8
}

# CHANNELS is comma-separated; the Makefile has already checked the names themselves
name_list() {
  printf %s "$NAMES" | tr ',' ' '
}

case "$MODE" in
  spec)
    [ -n "$NAMES" ] || exit 0
    missing=""
    spec=""
    for name in $(name_list); do
      if [ ! -f "$KEYFILE" ]; then
        missing="$missing $name"
        continue
      fi
      found=$(lookup "$name")
      count=$(printf %s "$found" | grep -c . || true)
      if [ "$count" -gt 1 ]; then
        fail "$KEYFILE gives '$name' a key $count times - which one is the channel?"
      fi
      if [ -z "$found" ]; then
        missing="$missing $name"
        continue
      fi
      hex=$(normalise "$found" "$name")
      case "$hex" in error:*) echo "$hex"; exit 1 ;; esac
      spec="$spec,$name:$hex"
    done
    if [ -n "$missing" ]; then
      echo "missing:$missing"
      exit 0
    fi
    printf '%s\n' "${spec#,}"
    ;;

  generate)
    [ -n "$NAMES" ] || exit 0
    dir=$(dirname "$KEYFILE")
    mkdir -p "$dir"
    chmod 700 "$dir" 2>/dev/null || true
    if [ ! -f "$KEYFILE" ]; then
      umask 077
      {
        echo "# MeshCore $(basename "$KEYFILE"), generated $(date -u +%Y-%m-%dT%H:%M:%SZ)"
        echo "# NAME = KEY, one channel per line. A key is 32 hex characters - the raw"
        echo "# AES-128 key - or a base64 PSK ending in '=', or a passphrase. Every node"
        echo "# on this network is built from an identical copy of this file. Do not commit it."
      } > "$KEYFILE"
      chmod 600 "$KEYFILE" 2>/dev/null || true
      echo "generated $KEYFILE"
    fi
    for name in $(name_list); do
      found=$(lookup "$name")
      count=$(printf %s "$found" | grep -c . || true)
      if [ "$count" -gt 1 ]; then
        fail "$KEYFILE gives '$name' a key $count times - remove one and run this again"
      fi
      # NEVER over an existing key: the key IS the channel, so replacing one would cut
      # this node off from every node already flashed with the old one.
      [ -n "$found" ] && continue
      printf '%s = %s\n' "$name" "$(random_key)" >> "$KEYFILE"
      echo "added a key for channel '$name'"
    done
    ;;

  print)
    [ -n "$NAMES" ] || exit 0
    for name in $(name_list); do
      found=$([ -f "$KEYFILE" ] && lookup "$name" || true)
      if [ -z "$found" ]; then
        printf '  channel %-16s no key in %s - run make keys\n' "$name" "$KEYFILE"
        continue
      fi
      hex=$(normalise "$found" "$name")
      case "$hex" in error:*) echo "$hex"; exit 1 ;; esac
      printf '  channel %-16s fingerprint %s\n' "$name" "$(fingerprint "$hex")"
    done
    ;;

  *)
    echo "usage: $0 spec|generate|print <keyfile> <names>" >&2
    exit 2
    ;;
esac
