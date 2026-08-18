#pragma once

#include <stdint.h>

/**
 * \brief  Adaptive send schedule for location-bearing adverts.
 *
 * Fires when either the current interval elapses, or the node has moved further than
 * a threshold from the position it last transmitted. While the node stays put the
 * interval grows geometrically up to a ceiling, so parked devices go quiet; any
 * detected movement snaps it straight back to the floor.
 *
 * Distance is always measured against the last TRANSMITTED position, never the last
 * reading. That matters twice over: it stops GPS jitter around a fixed point from
 * looking like travel, and it lets genuine slow drift accumulate until it really does
 * cross the threshold, instead of being forgiven on every poll.
 *
 * Pure logic - no Arduino, RTC or radio deps - so it can be exercised off-device.
 * The caller supplies the clock, which must be a monotonic millisecond counter;
 * wrap-around is handled.
 */
class AdvertScheduler {
public:
  // Defaults are declared here rather than in the constructor so that a caller filling
  // this in from build flags can set only the fields it cares about without reading
  // uninitialised memory for the rest.
  struct Config {
    uint32_t min_interval_secs = 60;    // floor, used while moving
    uint32_t max_interval_secs = 3600;  // ceiling, reached after sitting still
    uint32_t dist_threshold_m = 100;    // movement needed to force a send (0 disables)
    uint8_t  backoff_factor = 2;        // interval multiplier per stationary send
    uint8_t  jitter_pct = 15;           // +/- spread on each deadline (0 = exactly periodic)
    uint32_t startup_spread_secs = 0;   // random delay before the first advert (0 = send on first fix)
  };

  enum Reason : uint8_t {
    REASON_NONE = 0,   // nothing to do
    REASON_FIRST_FIX,  // first usable position since boot
    REASON_MOVED,      // travelled past dist_threshold_m
    REASON_INTERVAL,   // interval elapsed while stationary
  };

  AdvertScheduler();

  /**
   * \brief  (re)start the schedule. Clamps nonsensical config into range.
   * \param  seed  seeds the jitter sequence, and MUST differ between nodes - seed it
   *               from something unique and stable such as the node's public key.
   *               Seeding from millis() at boot defeats the purpose, because a fleet
   *               powering up together would draw near-identical seeds.
   */
  void begin(const Config& cfg, uint32_t now_ms, uint32_t seed);

  /**
   * \brief  Drive the schedule. Safe to call as often as you like.
   * \param  loc_valid  false when there is no usable fix - the scheduler then does
   *                    nothing at all, so a node never beacons a position it doesn't have.
   * \returns  why an advert should be sent now, or REASON_NONE.
   */
  Reason poll(uint32_t now_ms, bool loc_valid, int32_t lat_e6, int32_t lon_e6);

  /**
   * \brief  Force the schedule back to its floor, as if movement had been seen.
   *         Used when something else (a manual advert) makes the backoff stale.
   */
  void reset(uint32_t now_ms);

  /**
   * \brief  Take back the send that poll() just asked for, because it did not happen.
   *
   * poll() commits as it decides: it moves the distance reference to the position it is
   * about to beacon and resets the deadline. If the send then fails - an empty packet
   * pool is enough - the reference has already moved past the travel that triggered it,
   * so the movement trigger will never fire for that leg again and the update is simply
   * lost. This puts the scheduler back to having nothing sent yet, so the next poll
   * beacons the current position immediately.
   */
  void undoSend(uint32_t now_ms);

  uint32_t getIntervalSecs() const { return _interval_secs; }
  bool hasPosition() const { return _have_last; }
  int32_t getLastSentLat() const { return _last_lat; }
  int32_t getLastSentLon() const { return _last_lon; }

private:
  Config   _cfg;
  int32_t  _last_lat, _last_lon;   // position of the last advert we actually sent
  uint32_t _next_ms;
  uint32_t _startup_ms;            // earliest the first advert may go out
  uint32_t _interval_secs;         // exact backoff state - never itself jittered
  uint32_t _rand;
  int32_t  _cos_ref_lat;
  float    _cos_lat;
  bool     _have_last;
  bool     _cos_valid;

  bool movedFarEnough(int32_t lat_e6, int32_t lon_e6);
  void refreshCosLat(int32_t lat_e6);
  void markSent(uint32_t now_ms, int32_t lat_e6, int32_t lon_e6);
  uint32_t nextRandom();
  uint32_t jitteredMs(uint32_t interval_secs);
};
