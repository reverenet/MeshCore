#include "PositionReport.h"
#include <Utils.h>
#include <string.h>

static void put32(uint8_t* p, uint32_t v) {
  p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}
static uint32_t get32(const uint8_t* p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static void put16(uint8_t* p, uint16_t v) {
  p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8);
}
static uint16_t get16(const uint8_t* p) {
  return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

int PositionReport::encode(uint8_t* dest, size_t dest_cap,
                           const uint8_t pubkey_prefix[POS_PREFIX_LEN],
                           const PositionSample* samples, int num_samples, int* consumed) {
  if (consumed) *consumed = 0;
  if (num_samples <= 0 || dest_cap < POS_HEADER_LEN) return 0;

  int max_deltas = (int)((dest_cap - POS_HEADER_LEN) / POS_DELTA_LEN);
  if (max_deltas > 255) max_deltas = 255;

  const PositionSample& base = samples[0];

  int i = 0;
  dest[i++] = POSITION_REPORT_VERSION;
  memcpy(&dest[i], pubkey_prefix, POS_PREFIX_LEN); i += POS_PREFIX_LEN;
  put32(&dest[i], base.timestamp); i += 4;
  put32(&dest[i], (uint32_t)base.lat_e6); i += 4;
  put32(&dest[i], (uint32_t)base.lon_e6); i += 4;
  int count_pos = i++;   // filled in once we know how many deltas fit

  int n = 0;
  const PositionSample* prev = &base;
  while (n < max_deltas && (n + 1) < num_samples) {
    const PositionSample& s = samples[n + 1];

    // deltas are relative to the previous sample; anything that won't fit ends this
    // report, and the caller carries the remainder into the next one
    int64_t dt = (int64_t)s.timestamp - (int64_t)prev->timestamp;
    int64_t dlat = (int64_t)s.lat_e6 - (int64_t)prev->lat_e6;
    int64_t dlon = (int64_t)s.lon_e6 - (int64_t)prev->lon_e6;
    if (dt < 0 || dt > 65535) break;
    if (dlat < -32768 || dlat > 32767) break;
    if (dlon < -32768 || dlon > 32767) break;

    put16(&dest[i], (uint16_t)dt); i += 2;
    put16(&dest[i], (uint16_t)(int16_t)dlat); i += 2;
    put16(&dest[i], (uint16_t)(int16_t)dlon); i += 2;

    prev = &s;
    n++;
  }

  dest[count_pos] = (uint8_t)n;
  if (consumed) *consumed = n + 1;
  return i;
}

int PositionReport::decode(const uint8_t* src, size_t src_len,
                           uint8_t pubkey_prefix[POS_PREFIX_LEN],
                           PositionSample* dest, int max_samples) {
  if (src_len < POS_HEADER_LEN) return 0;
  if (src[0] != POSITION_REPORT_VERSION) return 0;
  if (max_samples < 1) return 0;

  int i = 1;
  memcpy(pubkey_prefix, &src[i], POS_PREFIX_LEN); i += POS_PREFIX_LEN;

  PositionSample cur;
  cur.timestamp = get32(&src[i]); i += 4;
  cur.lat_e6 = (int32_t)get32(&src[i]); i += 4;
  cur.lon_e6 = (int32_t)get32(&src[i]); i += 4;
  int n = src[i++];

  if (src_len < (size_t)(POS_HEADER_LEN + n * POS_DELTA_LEN)) return 0;   // truncated

  dest[0] = cur;
  int out = 1;

  for (int k = 0; k < n; k++) {
    uint16_t dt = get16(&src[i]); i += 2;
    int16_t dlat = (int16_t)get16(&src[i]); i += 2;
    int16_t dlon = (int16_t)get16(&src[i]); i += 2;

    cur.timestamp += dt;
    cur.lat_e6 += dlat;
    cur.lon_e6 += dlon;

    if (out < max_samples) dest[out++] = cur;
  }
  return out;
}

// The keystream is SHA256(secret || nonce || block_index), one 32-byte block at a time.
// SHA256 rather than AES because it is the primitive every platform here already has in
// hardware or software, and a report is a few dozen bytes - two hashes, not a cipher setup.
void PositionReport::whiten(const uint8_t secret[POS_SECRET_LEN], const uint8_t nonce[POS_NONCE_LEN],
                            uint8_t* data, size_t len) {
  uint8_t input[POS_SECRET_LEN + POS_NONCE_LEN + 1];
  memcpy(input, secret, POS_SECRET_LEN);
  memcpy(input + POS_SECRET_LEN, nonce, POS_NONCE_LEN);

  uint8_t block[32];
  size_t done = 0;
  for (uint8_t index = 0; done < len; index++) {
    input[POS_SECRET_LEN + POS_NONCE_LEN] = index;
    mesh::Utils::sha256(block, sizeof(block), input, sizeof(input));

    size_t n = len - done;
    if (n > sizeof(block)) n = sizeof(block);
    for (size_t i = 0; i < n; i++) data[done + i] ^= block[i];
    done += n;
  }
}

// Derived from the plaintext, not drawn from the RNG: two reports can then only share a
// nonce by being byte-identical, which is the one case where a shared keystream gives
// nothing away. Folding in the secret stops an observer confirming a guessed report by
// recomputing its nonce.
void PositionReport::deriveNonce(const uint8_t secret[POS_SECRET_LEN], const uint8_t* plain,
                                 size_t len, uint8_t nonce[POS_NONCE_LEN]) {
  uint8_t full[32];
  mesh::Utils::sha256(full, sizeof(full), secret, POS_SECRET_LEN, plain, (int)len);
  memcpy(nonce, full, POS_NONCE_LEN);
}
