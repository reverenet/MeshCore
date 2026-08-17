#pragma once

// Build-time configuration for automatic advert scheduling and position tracking.
//
// All of it is compile-time on purpose: the companion app protocol is untouched, so a
// device can beacon its position without any client update.

// ---------------------------------------------------------------- GPS
//
// Whether the GPS receiver is switched on when a device first boots. On by default here:
// everything else in this file is about reporting a position, and none of it can do
// anything on a node whose receiver was never started.
//
// Only affects boards built with ENV_INCLUDE_GPS and with a receiver actually detected -
// elsewhere the setting is simply refused and nothing changes. It also only applies to a
// device with no saved prefs: once gps_enabled has been stored, that value wins, so this
// will not switch GPS on for a device already in the field. Use 'gps on' over the CLI, or
// the app's toggle, for those.
//
// Set GPS_ENABLED=0 to keep the receiver off until asked, which is worth doing on a
// mains-free node where the GPS is the largest current draw.
#ifndef GPS_ENABLED
  #define GPS_ENABLED                   1
#endif
#ifndef GPS_INTERVAL
  // Seconds between position updates. 0 leaves the sensor manager's own cadence alone,
  // which is once a second - fast enough that a tracking sample is never stale.
  #define GPS_INTERVAL                  0
#endif

// ---------------------------------------------------------------- position reports
//
// Batched, encrypted position tracking carried on group datagrams instead of adverts.
// Entirely firmware-side: reports are intercepted before the app layer, and a received
// position updates the sender's existing contact record, so the app shows nodes moving
// through the contact list it already has. No client changes, no visible extra channel.
//
//   -D TRACKING_KEY='"the shared secret of the group that may see these positions"'
//   -D TRACK_REPORT=1
//
// TRACKING_KEY unset compiles the whole feature out. Set it WITHOUT TRACK_REPORT on a
// node that should receive and display tracks but not report its own position.
//
// Use a key with real entropy - 'make keys' generates one. It is hashed once into the
// channel secret, with no salt and no iteration count, so a memorable phrase can be
// ground out offline from a single captured packet.
//
// The key belongs only to the nodes that may read a position. Repeaters relay these
// reports without decrypting them, so a repeater never needs it and should not have it.
#ifndef TRACK_REPORT
  #define TRACK_REPORT                  0     // 1 = this node reports its own position
#endif
#ifndef TRACK_REPORT_SECS
  // Fixed transmit cadence. Constant rate is the point: it decouples airtime from
  // movement, so an observer can't tell a moving node from a parked one by timing alone.
  #define TRACK_REPORT_SECS             300
#endif
#ifndef TRACK_SAMPLE_MIN_SECS
  #define TRACK_SAMPLE_MIN_SECS         60    // fastest sampling, while moving
#endif
#ifndef TRACK_SAMPLE_MAX_SECS
  #define TRACK_SAMPLE_MAX_SECS         3600  // slowest, once parked
#endif
#ifndef TRACK_SAMPLE_DIST_M
  #define TRACK_SAMPLE_DIST_M           100
#endif
#ifndef TRACK_BUFFER
  // Backlog depth. Bigger buffers ride out longer gaps in coverage; each sample is 12
  // bytes of RAM here and 6 bytes on air.
  #define TRACK_BUFFER                  48
#endif
#ifndef TRACK_FLOOD
  #define TRACK_FLOOD                   0     // 0 = zero-hop, 1 = scoped flood
#endif
#ifndef TRACK_PEERS
  // How many reporting nodes we remember a freshness watermark for. Reports from beyond
  // this many peers still work; the least recently heard entry is simply recycled.
  #define TRACK_PEERS                   16
#endif

// ---------------------------------------------------------------- forwarding
//
// Max hops an advert may accumulate before a repeat-enabled companion stops forwarding
// it. Matches the repeater/room-server default. The ceiling that matters is bytes, not
// hops: path_bytes = hops * (path_hash_mode + 1) must stay inside the advert blob
// record, which after the cipher overhead leaves 32 bytes.
#ifndef FLOOD_MAX_ADVERT
  #define FLOOD_MAX_ADVERT              8
#endif

// ---------------------------------------------------------------- plain beacon
//
// Periodic advert carrying no position. Mostly useful as a cheap liveness heartbeat.
#ifndef AUTO_ADVERT_SECS
  #define AUTO_ADVERT_SECS              0     // 0 = disabled
#endif
#ifndef AUTO_ADVERT_FLOOD
  #define AUTO_ADVERT_FLOOD             0     // 0 = zero-hop, 1 = scoped flood
#endif

// ---------------------------------------------------------------- location beacon
//
// Adaptive position beacon. Sends when the node has travelled far enough, or when the
// current interval elapses; while it sits still the interval grows to the ceiling.
#ifndef AUTO_ADVERT_LOC
  #define AUTO_ADVERT_LOC               0     // 0 = disabled
#endif
#ifndef AUTO_ADVERT_LOC_POLICY
  // advert_loc_policy defaults to NONE, and an advert carrying no position makes the
  // location beacon pointless - so enabling the beacon defaults the policy with it.
  // This is only the default for a NEW install; the app can still change it afterwards.
  #if AUTO_ADVERT_LOC
    #define AUTO_ADVERT_LOC_POLICY      ADVERT_LOC_SHARE
  #else
    #define AUTO_ADVERT_LOC_POLICY      ADVERT_LOC_NONE
  #endif
#endif
#ifndef AUTO_ADVERT_LOC_MIN_SECS
  #define AUTO_ADVERT_LOC_MIN_SECS      60    // floor, while moving
#endif
#ifndef AUTO_ADVERT_LOC_MAX_SECS
  #define AUTO_ADVERT_LOC_MAX_SECS      3600  // ceiling, once parked
#endif
#ifndef AUTO_ADVERT_LOC_DIST_M
  // Keep this well above GPS noise. Below ~30m a stationary receiver's own wander reads
  // as travel, the backoff never engages, and a parked node beacons at the floor forever.
  #define AUTO_ADVERT_LOC_DIST_M        100
#endif
#ifndef AUTO_ADVERT_LOC_BACKOFF
  #define AUTO_ADVERT_LOC_BACKOFF       2     // interval multiplier per stationary send
#endif
#ifndef AUTO_ADVERT_LOC_JITTER_PCT
  #define AUTO_ADVERT_LOC_JITTER_PCT    15    // stops a fleet settling into lockstep
#endif
#ifndef AUTO_ADVERT_LOC_STARTUP_SECS
  // Worth setting on a large fleet: without it every node reports in the instant it gets
  // a fix, so a site-wide power event produces one synchronised burst.
  #define AUTO_ADVERT_LOC_STARTUP_SECS  0
#endif
#ifndef AUTO_ADVERT_LOC_FLOOD
  // Zero-hop by default. Flooding position updates multiplies their airtime by the size
  // of the mesh, which is rarely what you want for a tracking beacon.
  #define AUTO_ADVERT_LOC_FLOOD         0
#endif
