#pragma once

// Build-time configuration for automatic advert scheduling and position tracking.
//
// Almost all of it is compile-time on purpose: the companion app protocol is untouched,
// so a device can beacon its position without any client update. The two exceptions are
// TRACK_REPORT and TRACK_REPORT_SECS, which are settable at runtime and so are defaults
// rather than settings - each is marked below.

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
// A timestamp beyond now + this is not a late report, it is a bad one. See
// MyMesh::handleTrackReport: the per-peer replay watermark advances to whatever arrives,
// so an unbounded future timestamp is a one-packet denial of service against that peer.
#define TRACK_FUTURE_SLACK_SECS         3600

// Below this, the RTC has clearly never been set (1 Jan 2020), and no timestamp check
// against it would mean anything.
#define CLOCK_LOOKS_SET_EPOCH           1577836800

// Both of the next two are settable at runtime, over the companion protocol's custom
// variables - as 'track' and 'track_interval', or under these same build-flag names,
// which are accepted as aliases (see MyMesh::trackingVarName). What is set here is
// therefore only the DEFAULT for a device with no saved prefs: once either has been
// stored, that value wins, and changing this file will not move a device already in the
// field. Reflashing will not move it either - the saved value survives, so a fleet-wide
// change of mind about reporting has to go out over the command, not the build. The build
// flag decides what a NEWLY flashed node does on first boot, which is a different job.
//
// Note what runtime settability costs: TRACKING_KEY alone no longer guarantees a node
// cannot report. The transmit path is compiled in whenever the key is, so a receive-only
// node is now one custom-var write away from beaconing its position. Anyone who can pair
// with the device can make that write. If a node must be incapable of reporting rather
// than merely configured not to, leave TRACKING_KEY out of that build entirely and give
// it a receive-only build of its own.
#ifndef TRACK_REPORT
  #define TRACK_REPORT                  0     // 1 = this node reports its own position
#endif
#ifndef TRACK_REPORT_SECS
  // Fixed transmit cadence. Constant rate is the point: it decouples airtime from
  // movement, so an observer can't tell a moving node from a parked one by timing alone.
  #define TRACK_REPORT_SECS             300
#endif

// Bounds on the runtime cadence, enforced wherever it is set or loaded. The floor is not
// a policy preference: _next_track_report is scheduled as now + interval, so an interval
// of zero - out of a corrupt prefs file, or an app sending a bad value - would fire on
// every pass of the loop and transmit continuously. The ceiling is a day, past which a
// tracking node is not tracking anything.
#define TRACK_REPORT_MIN_SECS           10
#define TRACK_REPORT_MAX_SECS           86400

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

#ifndef TRACK_PUSH_REPORTS
  // Whether a report that arrives is also handed to the connected client, still encrypted,
  // as PUSH_CODE_TRACK_REPORT.
  //
  // Without this a batch of positions collapses to one point. handleTrackReport keeps the
  // newest sample and writes it to the sender's contact record, because that is the only
  // place a position can go without the app knowing anything new - and a contact record
  // holds one position. Every sample before the last is decoded, checked, and dropped. A
  // node reporting every 900s therefore draws one point every 900s on a client's map, no
  // matter how much of a trail was in the packet.
  //
  // With it, the report goes up exactly as it came off the air - nonce and all - and a
  // client holding the tracking key decodes the whole batch:
  //
  //   [0x91][snr x4 1][rssi 1][path_len 1][pubkey prefix 6][nonce 8][whitened report]
  //
  // path_len is 0xFF when the report did not arrive by flood. The prefix says who is
  // reporting without the client having to decrypt anything to find out. That is the same decoding a
  // position history answer needs, so a client implements it once and uses it for both.
  // A client without the key sees a frame code it does not know and ignores it, and the
  // contact record is updated either way.
  #define TRACK_PUSH_REPORTS            1
#endif

// ---------------------------------------------------------------- position history
//
// Answering one contact's question "where have you been since <instant>?" - see
// src/helpers/PositionHistory.h for the request and response on the wire.
//
// The reports above are a broadcast on a fixed cadence; this is a reply to a direct
// question, sent only to the contact that asked and only when they hold the tracking key
// as well. It costs no extra sampling: it is answered from a ring of the samples the node
// has already taken, which means it follows the same switch reporting does. A node with
// TRACK_REPORT off is not sampling, so it has nothing to answer with - which is the
// intended answer for a node told not to say where it is.
#ifndef TRACK_HISTORY
  // Samples kept to answer with, 12 bytes of RAM each. 0 compiles the whole feature out,
  // and a node that cannot answer simply does not - it is a request type it has never
  // heard of, which is what every other node on the mesh already thinks of it.
  //
  // At the 60s sampling floor this is about four hours of continuous movement, and far
  // longer than that for a node that spends most of its time parked, because the sampler
  // backs off to an hour when it is not moving.
  #define TRACK_HISTORY                 256
#endif
#ifndef TRACK_HISTORY_MAX_PKTS
  // Most packets one answer may take. A cap belongs here rather than in the request
  // because the airtime is spent by this node, on a shared channel, at the say-so of
  // somebody else: an answer that runs to hundreds of packets is a denial of service with
  // extra steps. What is left over is flagged as truncated, and the asker can come back
  // for it with a later 'since'.
  #define TRACK_HISTORY_MAX_PKTS        6
#endif
#ifndef TRACK_HISTORY_GAP_MS
  // Spacing between the packets of one answer. Back-to-back sends would hold the channel
  // for the whole answer and stamp on anything else trying to use it.
  #define TRACK_HISTORY_GAP_MS          3000
#endif
#ifndef TRACK_HISTORY_MIN_GAP_SECS
  // Least time between the STARTS of two answers, whoever asked. Without it a captured
  // request replayed in a loop turns this node into a transmitter: the tracking key check
  // stops a stranger asking, but it does not stop the same valid request being sent again.
  #define TRACK_HISTORY_MIN_GAP_SECS    15
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
