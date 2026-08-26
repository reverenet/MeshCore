#pragma once

#include <stdint.h>
#include <stddef.h>
#include "PositionReport.h"

/**
 * A node's own recent positions, and the query that asks another node for them.
 *
 * Position reports (PositionReport.h) are a broadcast: everyone with the tracking key
 * hears where a node is, at a cadence chosen by the sender. This is the other question -
 * one contact asking one other contact "where have you been since <instant>?" - and it
 * is answered from a ring of the samples the node has already taken, so the answer costs
 * no GPS work and no extra sampling.
 *
 * WHY A SEPARATE RING. The tracking buffer holds samples that have not been transmitted
 * yet and is drained on every report, so by the time anyone asks, it is usually empty.
 * The ring here keeps a copy of every sample taken, and the oldest falls off the end.
 *
 * TWO LAYERS OF KEY, and they answer different questions. The request and the response
 * travel inside an ordinary contact datagram, so the pairwise secret already means only
 * these two nodes can read them. On top of that both are whitened with the tracking
 * channel secret, and the whitening nonce is derived from the plaintext - so recomputing
 * it after unwhitening says whether the sender held the tracking key. That is the
 * authorisation this feature needs and the pairwise layer cannot give: a contact you
 * have paired with is not necessarily in the tracking group, and only the group may ask
 * where you have been.
 *
 * REQUEST, inside a PAYLOAD_TYPE_REQ contact datagram, after its 4-byte tag:
 *
 *     [REQ_TYPE_GET_POSITION_HISTORY 1][nonce 8][whitened: ver 1, since 4, secs 2, metres 2]
 *
 * RESPONSE, one or more PAYLOAD_TYPE_RESPONSE datagrams:
 *
 *     [tag 4][RESP_TYPE_POSITION_HISTORY 1][seq 1][flags 1][nonce 8][whitened report]
 *
 * The report body is exactly the PositionReport batch format the broadcast reports use,
 * so anything that can already read a position report can read one of these. The tag is
 * the request's, echoed so the asking node can match the answer to what it asked; seq
 * counts the packets of one answer from 0, and flags says whether this is the last of
 * them and whether the node had more to say than it was willing to send at once.
 *
 * An answer always ends with a packet carrying POS_HIST_FLAG_LAST, and that packet may
 * hold no positions at all - a node with nothing since the given instant says so in one
 * empty packet rather than saying nothing, which is what a node that never heard the
 * request looks like. It is the flag that ends an answer, never the emptiness.
 *
 * ASKING, from a client on the companion protocol. The firmware here is the answering
 * end; the asking end is a client that holds the tracking key, and needs no new command:
 *
 *   1. build the request body, whiten it, and prepend the nonce and the type byte
 *   2. CMD_SEND_BINARY_REQ with [contact pub_key 32][the request], which wraps it in the
 *      pairwise encryption and sends it
 *   3. RESP_CODE_SENT comes back carrying the tag the node chose
 *   4. every packet of the answer arrives as PUSH_CODE_BINARY_RESPONSE under that tag,
 *      until one with POS_HIST_FLAG_LAST set
 *
 * The node doing the asking never decrypts any of this and does not need the tracking key
 * itself - it is the radio, and the client is what reads the answer.
 *
 * DOWNSAMPLING happens on the node that holds the history, not on the asker, because the
 * whole point is to spend less airtime than sending every sample would.
 *
 * The two thresholds are asked for together and are an OR, not a choice: "every 300
 * seconds or every 100 metres" keeps a sample as soon as EITHER has been passed. That is
 * the combination worth having, because each covers what the other misses - time alone
 * says nothing about a node that crossed a town between two samples, and distance alone
 * loses a node that stopped somewhere for an hour. Setting one to 0 turns it off and
 * leaves the other to decide on its own; both 0 asks for every sample there is.
 *
 * Both measure against the last sample actually SELECTED rather than the last one looked
 * at, so an interval cannot drift and a distance cannot be defeated by a node that
 * wandered there in small steps.
 */

// ------------------------------------------------------------------ request/response

// data[0] of a contact request, alongside REQ_TYPE_GET_STATUS and friends
#define REQ_TYPE_GET_POSITION_HISTORY   0x04

#define POS_HIST_VERSION                1

// The two downsampling thresholds. Either may be 0, meaning "do not thin on this", and
// both 0 asks for every sample there is.
#define POS_HIST_REQ_BODY_LEN           9   // ver 1 + since 4 + secs 2 + metres 2
#define POS_HIST_REQ_LEN                (1 + POS_NONCE_LEN + POS_HIST_REQ_BODY_LEN)

// data[4] of a response, the byte that tells the asking node this is a multi-packet
// answer rather than the single reply every other request type sends back. High and
// arbitrary: it only has to differ from the first byte of the other response kinds,
// and a telemetry payload starts with a small channel number.
#define RESP_TYPE_POSITION_HISTORY      0xB5

#define POS_HIST_FLAG_LAST              0x01   // no more packets for this request
#define POS_HIST_FLAG_TRUNCATED         0x02   // there was more history than was sent

#define POS_HIST_RESP_HEADER_LEN        (4 + 1 + 1 + 1 + POS_NONCE_LEN)

struct PositionHistoryReq {
  uint8_t  version;
  uint32_t since;         // UNIX seconds; samples at or after this are eligible
  uint16_t every_secs;    // keep a sample once this long since the last kept one (0 = never on time)
  uint16_t every_metres;  // keep a sample once this far from the last kept one (0 = never on distance)
};

// ------------------------------------------------------------------------- the ring

class PositionHistory {
public:
  /**
   * Where a partly-answered query has got to. Held by the caller rather than by the ring,
   * so a node can hand out its history without the ring having to know how many askers
   * there are or how far each has got.
   *
   * Positions are absolute sequence numbers, not indexes: samples keep arriving while an
   * answer is being sent, and on a full ring each new one drops the oldest. An index
   * would quietly slide by one every time that happened and skip a sample.
   */
  struct Cursor {
    PositionHistoryReq req;
    uint32_t next_seq;    // first sample not yet considered
    bool     lost;        // the ring dropped samples this cursor had not reached
    bool     have_last;
    PositionSample last;  // the last sample SELECTED - what downsampling measures from
  };

  PositionHistory() : _buf(NULL), _cap(0), _count(0), _head(0), _added(0) { }

  /** \brief  Adopt the caller's storage. Sized by the caller, so the RAM is visible where it is spent. */
  void begin(PositionSample* storage, int capacity);

  void add(const PositionSample& s);
  void clear();

  int count() const { return _count; }
  bool isEmpty() const { return _count == 0; }

  /** \brief  Total ever added, which is also the sequence number the next add() will take. */
  uint32_t nextSeq() const { return _added; }

  /** \brief  The oldest sequence number still held. */
  uint32_t oldestSeq() const { return _added - (uint32_t)_count; }

  /** \brief  One sample by sequence number. Returns false once it has fallen off the ring. */
  bool getSeq(uint32_t seq, PositionSample& dest) const;

  /** \brief  Begin answering 'req'. Cheap: it selects nothing until next() is called. */
  void start(Cursor& c, const PositionHistoryReq& req) const;

  /**
   * \brief  Select up to 'max' more samples, oldest first, applying the downsampling.
   * \returns  how many were written to 'dest'. 0 means this cursor is finished - or that
   *           'max' was 0, which the caller asked for.
   */
  int next(Cursor& c, PositionSample* dest, int max) const;

  /** \brief  Whether every sample has been considered, so the answer is complete. */
  bool isFinished(const Cursor& c) const { return c.next_seq >= _added; }

  // ------------------------------------------------------------------ request coding
  //
  // The body only - whitening is applied by the caller, which is the only part that needs
  // the tracking key, and keeps this class free of any crypto at all.

  /** \returns  bytes written (POS_HIST_REQ_BODY_LEN), or 0 if it would not fit. */
  static int encodeReqBody(uint8_t* dest, size_t dest_cap, const PositionHistoryReq& req);

  /** \returns  false for a short body, an unknown version, or a mode this build cannot answer. */
  static bool decodeReqBody(const uint8_t* src, size_t len, PositionHistoryReq& req);

private:
  PositionSample* _buf;
  int      _cap;
  int      _count;
  int      _head;      // where the next sample goes
  uint32_t _added;     // total ever added; the ring holds the last _count of them

  bool keep(const Cursor& c, const PositionSample& s) const;
};
