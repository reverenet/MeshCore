#pragma once

#include <stdint.h>
#include <stddef.h>

// Sub-type carried in the GRP_DATA header. Nodes without the tracking key never see
// this; nodes with it intercept it before the app layer, so no client ever meets it.
#define POSITION_REPORT_DATA_TYPE  0x5054
#define POSITION_REPORT_VERSION    2

#define POS_PREFIX_LEN   6                                   // enough to identify a contact
#define POS_HEADER_LEN   (1 + POS_PREFIX_LEN + 4 + 4 + 4 + 1)
#define POS_DELTA_LEN    6
#define POS_NONCE_LEN    8
#define POS_SECRET_LEN   32   // the group channel secret, SHA256-sized

struct PositionSample {
  uint32_t timestamp;          // UNIX seconds
  int32_t  lat_e6, lon_e6;     // micro-degrees
};

/**
 * \brief  A batch of timestamped positions, delta-compressed.
 *
 * Adverts are the wrong carrier for tracking: app_data caps at 32 bytes, every advert
 * is archived in the blob store, and each one must be decryptable by every repeater in
 * order to be routed at all. Group datagrams have none of those limits, and Mesh routes
 * them without decrypting - so repeaters relay position reports they cannot read.
 *
 * Batching is what decouples transmit rate from movement. A node samples when it moves
 * but transmits on a fixed cadence, so an observer can no longer infer motion from
 * packet timing.
 *
 * A report carries the NEWEST samples that fit, not the oldest waiting ones - see
 * encodeNewest(). Nothing is consumed by sending: the sender keeps a rolling window of
 * its recent positions, and each report is a view of the end of it. A receiver that can
 * see it is missing everything between the last report and this one asks for that stretch
 * back with a position history request (PositionHistory.h), which is answered from the
 * same window. That is a better trade than spending a report on old positions while the
 * newest ones wait: an out-of-date position helps nobody, and the gap is recoverable.
 *
 * Wire format:
 *   [ver 1][pubkey_prefix 6][base_time u32][base_lat i32][base_lon i32][n 1]
 *   then n * [dt u16][dlat i16][dlon i16]
 *
 * Deltas are relative to the preceding sample, so a i16 covers about +/-3.6km of travel
 * and dt covers 18 hours. Anything beyond that simply starts a new report.
 *
 * WHITENING:
 *
 * The group datagram that carries this is encrypted with AES in ECB mode (Utils::encrypt),
 * where each 16-byte block is enciphered independently and deterministically. The report
 * header holds base_time, so the first block differs every time - but the blocks after it
 * hold only coordinates and deltas. A node that has not moved would emit byte-identical
 * trailing blocks report after report, and an observer with no key could read "this node
 * is stationary" straight off the wire. That is the one thing the fixed transmit cadence
 * exists to hide.
 *
 * So the report is XORed with a keystream before it is handed to the channel:
 *
 *     nonce      = SHA256(secret || plaintext)[0..7]        (sent in the clear)
 *     keystream  = SHA256(secret || nonce || block_index)
 *
 * Every byte then depends on the nonce, so a single changed bit anywhere in the report
 * changes every block of it. The nonce is derived rather than drawn from the RNG for the
 * same reason the advert cipher did it: uniqueness by construction beats hoping an
 * embedded PRNG is good. Two reports can only share a keystream if they are byte-identical
 * - in which case they are the same packet, and the mesh dedup tables drop the second
 * exactly as they do today.
 */
class PositionReport {
public:
  /**
   * \brief  Encode as many leading samples as will fit.
   * \param  consumed  (OUT) how many samples were packed - the caller keeps the rest
   *                   for the next report.
   * \returns  bytes written, or 0 if nothing could be encoded.
   */
  static int encode(uint8_t* dest, size_t dest_cap,
                    const uint8_t pubkey_prefix[POS_PREFIX_LEN],
                    const PositionSample* samples, int num_samples, int* consumed);

  /**
   * \brief  Encode the NEWEST samples that will fit, counting back from the end.
   *
   * encode() starts at samples[0] and fills forwards, which is what a queue being drained
   * wants. A report from a rolling window wants the opposite: the last thing anyone needs
   * is where a node was an hour ago while where it is now waits for the next report.
   *
   * Deltas are relative to the sample before, so a step too big to express - more than
   * about 3.6 km or 18 hours - ends a report. Counting back cannot simply start at
   * num_samples - capacity and encode: a break part way through that stretch would leave
   * the report holding the OLDER half and drop exactly the samples this call exists to
   * carry. So it starts past each break it finds and tries again, and what comes back is
   * the newest unbroken run that fits.
   *
   * \param  first  (OUT) index of the first sample included, so the caller can tell how
   *                far back the report reaches.
   * \param  count  (OUT) how many samples went in.
   * \returns  bytes written, or 0 if nothing could be encoded.
   */
  static int encodeNewest(uint8_t* dest, size_t dest_cap,
                          const uint8_t pubkey_prefix[POS_PREFIX_LEN],
                          const PositionSample* samples, int num_samples,
                          int* first, int* count);

  /**
   * \brief  Decode a report.
   * \returns  number of samples recovered (oldest first), or 0 if malformed.
   */
  static int decode(const uint8_t* src, size_t src_len,
                    uint8_t pubkey_prefix[POS_PREFIX_LEN],
                    PositionSample* dest, int max_samples);

  /**
   * \returns  how many samples fit in a report of at most 'cap' bytes.
   */
  /**
   * \brief  The nonce for one report body, derived from it and the channel secret.
   */
  static void deriveNonce(const uint8_t secret[POS_SECRET_LEN], const uint8_t* plain, size_t len,
                          uint8_t nonce[POS_NONCE_LEN]);

  /**
   * \brief  XOR a report body with the keystream for 'nonce'. Its own inverse, so the
   *         same call both applies and removes it.
   */
  static void whiten(const uint8_t secret[POS_SECRET_LEN], const uint8_t nonce[POS_NONCE_LEN],
                     uint8_t* data, size_t len);

  // constexpr so a caller can size an array with it. Without that the size is a runtime
  // value, and 'PositionSample samples[capacityFor(...)]' is a VLA - a GNU extension
  // rather than C++, and a stack frame that cannot be read off the function.
  //
  // One return statement, not an if: the device targets build as C++11, where that is
  // all a constexpr function may contain. Only the native test envs set -std=c++17.
  static constexpr int capacityFor(size_t cap) {
    return (cap < POS_HEADER_LEN) ? 0 : 1 + (int)((cap - POS_HEADER_LEN) / POS_DELTA_LEN);
  }
};
