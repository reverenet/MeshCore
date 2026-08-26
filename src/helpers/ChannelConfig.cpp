#include "ChannelConfig.h"
#include <string.h>

// -1 for anything that is not a hex digit, the null terminator included - which is what
// lets the loop below read a pair at a time without first measuring the string.
static int hexVal(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

int ChannelConfig::parse(const char* spec, ChannelConfigEntry* dest, int max_entries) {
  if (spec == NULL || dest == NULL || max_entries <= 0) return -1;

  int count = 0;
  const char* p = spec;

  while (*p) {
    const char* name = p;
    while (*p && *p != ':' && *p != ',') p++;
    if (*p != ':') return -1;                   // a name with no key after it

    size_t name_len = (size_t)(p - name);
    if (name_len == 0 || name_len >= CHANNEL_CONFIG_NAME_LEN) return -1;
    p++;                                        // past the ':'

    uint8_t secret[CHANNEL_CONFIG_SECRET_LEN];
    for (int i = 0; i < CHANNEL_CONFIG_SECRET_LEN; i++) {
      int hi = hexVal(p[0]);
      if (hi < 0) return -1;                    // short key, or not hex at all
      int lo = hexVal(p[1]);                    // safe: p[0] was not the terminator
      if (lo < 0) return -1;
      secret[i] = (uint8_t)((hi << 4) | lo);
      p += 2;
    }
    if (*p != 0 && *p != ',') return -1;        // longer than 128 bits, or trailing junk

    if (count == max_entries) return count;     // full - the caller sized it, so stop here

    memset(&dest[count], 0, sizeof(dest[count]));
    memcpy(dest[count].name, name, name_len);
    memcpy(dest[count].secret, secret, sizeof(secret));
    count++;

    if (*p == ',') {
      p++;
      if (*p == 0) return -1;                   // trailing separator, so a name was lost
    }
  }
  return count;
}
