#include "PositionHistory.h"
#include "GeoDistance.h"
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

void PositionHistory::begin(PositionSample* storage, int capacity) {
  _buf = storage;
  _cap = (storage == NULL || capacity < 0) ? 0 : capacity;
  clear();
}

void PositionHistory::clear() {
  _count = 0;
  _head = 0;
  // NOT _added: sequence numbers stay monotonic across a clear, so a cursor held over one
  // sees samples it has not reached go missing rather than reading new ones as old.
}

void PositionHistory::add(const PositionSample& s) {
  if (_cap == 0) return;   // history compiled out, or never given storage

  _buf[_head] = s;
  _head = (_head + 1) % _cap;
  if (_count < _cap) _count++;
  _added++;
}

bool PositionHistory::getSeq(uint32_t seq, PositionSample& dest) const {
  if (_count == 0) return false;
  if (seq < oldestSeq() || seq >= _added) return false;

  // _head is where the NEXT sample goes, so the oldest sits _count back from it
  int offset = (int)(seq - oldestSeq());
  int idx = (_head - _count + offset) % _cap;
  if (idx < 0) idx += _cap;
  dest = _buf[idx];
  return true;
}

void PositionHistory::start(Cursor& c, const PositionHistoryReq& req) const {
  c.req = req;
  c.next_seq = oldestSeq();
  c.lost = false;
  c.have_last = false;
  memset(&c.last, 0, sizeof(c.last));
}

bool PositionHistory::keep(const Cursor& c, const PositionSample& s) const {
  if (!c.have_last) return true;   // the first eligible sample is always kept

  // Neither threshold set is a request for everything, not a request for nothing.
  if (c.req.every_secs == 0 && c.req.every_metres == 0) return true;

  if (c.req.every_secs != 0) {
    // Unsigned on purpose. A sample stamped BEFORE the last one selected - an RTC that
    // was corrected between two samples - wraps to a huge interval and is kept, which is
    // the right way round: a clock stepping backwards should not swallow positions.
    if ((uint32_t)(s.timestamp - c.last.timestamp) >= (uint32_t)c.req.every_secs) return true;
  }

  if (c.req.every_metres != 0) {
    if (GeoDistance::movedAtLeast(c.last.lat_e6, c.last.lon_e6,
                                  s.lat_e6, s.lon_e6, c.req.every_metres)) return true;
  }

  return false;   // too soon and too close: this one is thinned out
}

int PositionHistory::next(Cursor& c, PositionSample* dest, int max) const {
  if (dest == NULL || max <= 0) return 0;

  // Samples this cursor had not reached have fallen off the ring while it was being
  // answered. Skip to what is left and say so, rather than reporting a gap as if the node
  // simply had not moved.
  if (c.next_seq < oldestSeq()) {
    c.next_seq = oldestSeq();
    c.lost = true;
  }

  int out = 0;
  while (out < max && c.next_seq < _added) {
    PositionSample s;
    if (!getSeq(c.next_seq, s)) break;   // cannot happen while next_seq is in range
    c.next_seq++;

    if (s.timestamp < c.req.since) continue;
    if (!keep(c, s)) continue;

    dest[out++] = s;
    c.last = s;
    c.have_last = true;
  }
  return out;
}

int PositionHistory::encodeReqBody(uint8_t* dest, size_t dest_cap, const PositionHistoryReq& req) {
  if (dest == NULL || dest_cap < POS_HIST_REQ_BODY_LEN) return 0;

  int i = 0;
  dest[i++] = req.version;
  put32(&dest[i], req.since); i += 4;
  put16(&dest[i], req.every_secs); i += 2;
  put16(&dest[i], req.every_metres); i += 2;
  return i;
}

bool PositionHistory::decodeReqBody(const uint8_t* src, size_t len, PositionHistoryReq& req) {
  if (src == NULL || len < POS_HIST_REQ_BODY_LEN) return false;

  int i = 0;
  req.version = src[i++];
  req.since = get32(&src[i]); i += 4;
  req.every_secs = get16(&src[i]); i += 2;
  req.every_metres = get16(&src[i]); i += 2;

  // Refused rather than answered as best we can: a body this build cannot read is a
  // request for something it may not be able to perform, and an answer to a question
  // nobody asked is worse than no answer.
  if (req.version != POS_HIST_VERSION) return false;

  return true;
}
