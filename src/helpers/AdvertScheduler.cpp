#include "AdvertScheduler.h"
#include "GeoDistance.h"

// recompute the cos(lat) longitude scale only after this much north/south travel;
// below half a degree the error is far under the accuracy we need for a threshold test
#define COS_REFRESH_E6     500000

#define MAX_SANE_SECS      86400   // a day; keeps secs*1000 well clear of uint32 overflow
#define MAX_JITTER_PCT     50

AdvertScheduler::AdvertScheduler() {
  _cfg = Config();   // defaults live on the struct, so they aren't stated twice
  _last_lat = _last_lon = 0;
  _next_ms = 0;
  _startup_ms = 0;
  _interval_secs = _cfg.min_interval_secs;
  _rand = 0x9E3779B9;
  _cos_ref_lat = 0;
  _cos_lat = 1.0f;
  _have_last = false;
  _cos_valid = false;
}

void AdvertScheduler::begin(const Config& cfg, uint32_t now_ms, uint32_t seed) {
  _cfg = cfg;

  // clamp into something sane, so a bad build flag degrades rather than misbehaves
  if (_cfg.min_interval_secs < 1) _cfg.min_interval_secs = 1;
  if (_cfg.min_interval_secs > MAX_SANE_SECS) _cfg.min_interval_secs = MAX_SANE_SECS;
  if (_cfg.max_interval_secs < _cfg.min_interval_secs) _cfg.max_interval_secs = _cfg.min_interval_secs;
  if (_cfg.max_interval_secs > MAX_SANE_SECS) _cfg.max_interval_secs = MAX_SANE_SECS;
  if (_cfg.backoff_factor < 1) _cfg.backoff_factor = 1;
  if (_cfg.jitter_pct > MAX_JITTER_PCT) _cfg.jitter_pct = MAX_JITTER_PCT;
  if (_cfg.startup_spread_secs > MAX_SANE_SECS) _cfg.startup_spread_secs = MAX_SANE_SECS;

  // xorshift is stuck at zero, and nearby seeds start out correlated, so mix first
  _rand = seed ? seed : 0x9E3779B9;
  for (int i = 0; i < 8; i++) nextRandom();

  _have_last = false;
  _cos_valid = false;
  _interval_secs = _cfg.min_interval_secs;
  _next_ms = now_ms + jitteredMs(_interval_secs);

  // hold the very first advert back by a random slice, so a fleet coming up together
  // after a power event doesn't all report in during the same second
  uint32_t spread = _cfg.startup_spread_secs * 1000;
  _startup_ms = now_ms + (spread ? (nextRandom() % (spread + 1)) : 0);
}

uint32_t AdvertScheduler::nextRandom() {
  _rand ^= _rand << 13;
  _rand ^= _rand >> 17;
  _rand ^= _rand << 5;
  return _rand;
}

uint32_t AdvertScheduler::jitteredMs(uint32_t interval_secs) {
  uint32_t base = interval_secs * 1000;
  if (_cfg.jitter_pct == 0) return base;

  uint32_t span = (uint32_t)(((uint64_t)base * _cfg.jitter_pct) / 100);
  if (span == 0) return base;

  // uniform over [base-span, base+span], so the mean interval is unchanged and the
  // backoff curve keeps its intended average shape
  return base - span + (nextRandom() % (2 * span + 1));
}

void AdvertScheduler::refreshCosLat(int32_t lat_e6) {
  if (_cos_valid) {
    int32_t d = lat_e6 - _cos_ref_lat;
    if (d < 0) d = -d;
    if (d < COS_REFRESH_E6) return;
  }
  _cos_ref_lat = lat_e6;
  _cos_lat = GeoDistance::cosLatFor(lat_e6);
  _cos_valid = true;
}

bool AdvertScheduler::movedFarEnough(int32_t lat_e6, int32_t lon_e6) {
  if (_cfg.dist_threshold_m == 0) return false;   // distance trigger disabled

  // the cached longitude scale is the only reason this isn't a bare call: poll() runs
  // once a second, and the cache is what keeps a cosf() out of that path
  refreshCosLat(lat_e6);

  return GeoDistance::movedAtLeast(_last_lat, _last_lon, lat_e6, lon_e6,
                                   _cos_lat, _cfg.dist_threshold_m);
}

void AdvertScheduler::markSent(uint32_t now_ms, int32_t lat_e6, int32_t lon_e6) {
  _last_lat = lat_e6;
  _last_lon = lon_e6;
  _have_last = true;
  // NOTE: the jitter lands on the deadline, never on _interval_secs. Jittering the
  // state itself would compound across every doubling and let the backoff curve drift
  // away from the configured shape.
  _next_ms = now_ms + jitteredMs(_interval_secs);
}

void AdvertScheduler::undoSend(uint32_t now_ms) {
  // _last_lat/_last_lon are left where markSent() put them; _have_last is what gates
  // every read of them, and clearing it is what makes the next poll a first-fix send.
  _have_last = false;
  _interval_secs = _cfg.min_interval_secs;
  _next_ms = now_ms;
}

void AdvertScheduler::reset(uint32_t now_ms) {
  _interval_secs = _cfg.min_interval_secs;
  _next_ms = now_ms + jitteredMs(_interval_secs);
}

AdvertScheduler::Reason AdvertScheduler::poll(uint32_t now_ms, bool loc_valid, int32_t lat_e6, int32_t lon_e6) {
  if (!loc_valid) return REASON_NONE;   // never beacon a position we don't actually have

  if (!_have_last) {   // first usable fix since boot
    if ((int32_t)(now_ms - _startup_ms) < 0) return REASON_NONE;   // still inside the startup spread
    _interval_secs = _cfg.min_interval_secs;
    markSent(now_ms, lat_e6, lon_e6);
    return REASON_FIRST_FIX;
  }

  if (movedFarEnough(lat_e6, lon_e6)) {
    _interval_secs = _cfg.min_interval_secs;   // moving again - back to the floor
    markSent(now_ms, lat_e6, lon_e6);
    return REASON_MOVED;
  }

  if ((int32_t)(now_ms - _next_ms) >= 0) {   // signed diff, so millis wrap is a non-event
    // sitting still: still send a heartbeat, but less and less often
    uint32_t next = _interval_secs * _cfg.backoff_factor;
    if (next < _interval_secs || next > _cfg.max_interval_secs) {   // saturate (and catch overflow)
      next = _cfg.max_interval_secs;
    }
    _interval_secs = next;
    markSent(now_ms, lat_e6, lon_e6);
    return REASON_INTERVAL;
  }

  return REASON_NONE;
}
