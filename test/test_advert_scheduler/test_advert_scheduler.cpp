#include <gtest/gtest.h>
#include <helpers/AdvertScheduler.h>
#include <vector>
#include <algorithm>

// At the equator one micro-degree of latitude is ~0.11132 m, so distances along a
// meridian convert cleanly and independently of the cos(lat) longitude scaling.
static int32_t metresNorth(float m) { return (int32_t)(m / 0.111320f); }

static const int32_t LAT0 = 0;
static const int32_t LON0 = 0;

static const uint32_t SEED = 0xC0FFEEu;

// Jitter off by default here, so the scheduling tests can assert exact deadlines.
// The jitter behaviour gets its own section at the bottom.
static AdvertScheduler::Config defaultCfg() {
  AdvertScheduler::Config cfg;
  cfg.min_interval_secs = 60;
  cfg.max_interval_secs = 3600;
  cfg.dist_threshold_m = 100;
  cfg.backoff_factor = 2;
  cfg.jitter_pct = 0;
  cfg.startup_spread_secs = 0;
  return cfg;
}

// ---------------------------------------------------------------- no fix handling

TEST(AdvertScheduler, NeverSendsWithoutAFix) {
  AdvertScheduler s;
  s.begin(defaultCfg(), 0, SEED);

  // even long after the interval would have elapsed, no fix means nothing goes out -
  // a node must never beacon a position it doesn't actually have
  for (uint32_t t = 0; t < 10u * 3600u * 1000u; t += 60000) {
    EXPECT_EQ(AdvertScheduler::REASON_NONE, s.poll(t, false, LAT0, LON0)) << "at t=" << t;
  }
  EXPECT_FALSE(s.hasPosition());
}

TEST(AdvertScheduler, FirstFixSendsImmediately) {
  AdvertScheduler s;
  s.begin(defaultCfg(), 0, SEED);

  EXPECT_EQ(AdvertScheduler::REASON_NONE, s.poll(1000, false, LAT0, LON0));
  EXPECT_EQ(AdvertScheduler::REASON_FIRST_FIX, s.poll(2000, true, LAT0, LON0));
  EXPECT_TRUE(s.hasPosition());
  EXPECT_EQ(60u, s.getIntervalSecs());

  // and doesn't immediately send again
  EXPECT_EQ(AdvertScheduler::REASON_NONE, s.poll(2001, true, LAT0, LON0));
}

// ---------------------------------------------------------------- backoff curve

TEST(AdvertScheduler, StationaryNodeBacksOffGeometrically) {
  AdvertScheduler s;
  s.begin(defaultCfg(), 0, SEED);

  uint32_t t = 0;
  ASSERT_EQ(AdvertScheduler::REASON_FIRST_FIX, s.poll(t, true, LAT0, LON0));
  ASSERT_EQ(60u, s.getIntervalSecs());

  const uint32_t expected[] = { 120, 240, 480, 960, 1920, 3600, 3600, 3600 };
  uint32_t due = s.getIntervalSecs();   // seconds until the next send is owed

  for (uint32_t want : expected) {
    t += due * 1000;
    ASSERT_EQ(AdvertScheduler::REASON_INTERVAL, s.poll(t, true, LAT0, LON0));
    EXPECT_EQ(want, s.getIntervalSecs());
    due = s.getIntervalSecs();
  }
}

TEST(AdvertScheduler, BackoffSaturatesAndDoesNotOverflow) {
  AdvertScheduler::Config cfg = defaultCfg();
  cfg.backoff_factor = 200;      // absurd multiplier: must clamp, not wrap
  cfg.max_interval_secs = 3600;
  AdvertScheduler s;
  s.begin(cfg, 0, SEED);

  uint32_t t = 0;
  ASSERT_EQ(AdvertScheduler::REASON_FIRST_FIX, s.poll(t, true, LAT0, LON0));

  for (int i = 0; i < 20; i++) {
    t += s.getIntervalSecs() * 1000;
    ASSERT_EQ(AdvertScheduler::REASON_INTERVAL, s.poll(t, true, LAT0, LON0));
    EXPECT_LE(s.getIntervalSecs(), 3600u);
    EXPECT_GE(s.getIntervalSecs(), 60u);
  }
}

TEST(AdvertScheduler, BackoffFactorOneHoldsTheFloor) {
  AdvertScheduler::Config cfg = defaultCfg();
  cfg.backoff_factor = 1;
  AdvertScheduler s;
  s.begin(cfg, 0, SEED);

  uint32_t t = 0;
  ASSERT_EQ(AdvertScheduler::REASON_FIRST_FIX, s.poll(t, true, LAT0, LON0));
  for (int i = 0; i < 5; i++) {
    t += 60000;
    ASSERT_EQ(AdvertScheduler::REASON_INTERVAL, s.poll(t, true, LAT0, LON0));
    EXPECT_EQ(60u, s.getIntervalSecs());
  }
}

// ---------------------------------------------------------------- movement

TEST(AdvertScheduler, MovementSendsImmediatelyAndResetsTheInterval) {
  AdvertScheduler s;
  s.begin(defaultCfg(), 0, SEED);

  uint32_t t = 0;
  ASSERT_EQ(AdvertScheduler::REASON_FIRST_FIX, s.poll(t, true, LAT0, LON0));

  // sit still long enough to back well off
  for (int i = 0; i < 5; i++) {
    t += s.getIntervalSecs() * 1000;
    ASSERT_EQ(AdvertScheduler::REASON_INTERVAL, s.poll(t, true, LAT0, LON0));
  }
  ASSERT_GT(s.getIntervalSecs(), 60u);

  // then move: fires at once, no waiting for the (long) interval, and snaps to the floor
  t += 1000;
  EXPECT_EQ(AdvertScheduler::REASON_MOVED, s.poll(t, true, metresNorth(200), LON0));
  EXPECT_EQ(60u, s.getIntervalSecs());
}

TEST(AdvertScheduler, GpsJitterAroundAFixedPointNeverLooksLikeTravel) {
  AdvertScheduler s;
  s.begin(defaultCfg(), 0, SEED);

  uint32_t t = 0;
  ASSERT_EQ(AdvertScheduler::REASON_FIRST_FIX, s.poll(t, true, LAT0, LON0));

  // a parked receiver wanders; with a 100 m threshold none of it may register as
  // movement, or a stationary node would never back off at all
  const int32_t wander[] = { 180, -180, 90, -150, 120, -60, 170, -170, 40, -110 };
  int idx = 0;
  int interval_sends = 0;

  // long enough to walk the whole backoff curve to its ceiling (60+120+...+1920 = 3780s)
  for (int i = 0; i < 2000; i++) {
    t += 5000;
    int32_t lat = wander[idx % 10];
    int32_t lon = wander[(idx + 3) % 10];
    idx++;
    AdvertScheduler::Reason r = s.poll(t, true, lat, lon);
    ASSERT_NE(AdvertScheduler::REASON_MOVED, r) << "jitter read as movement at i=" << i;
    if (r == AdvertScheduler::REASON_INTERVAL) interval_sends++;
  }

  EXPECT_GT(interval_sends, 0);          // heartbeats still happen
  EXPECT_EQ(3600u, s.getIntervalSecs()); // and the backoff still reached its ceiling
}

TEST(AdvertScheduler, SlowDriftAccumulatesUntilItCrosses) {
  // Each step is only 10 m - far under the threshold - but they add up. This only
  // works because distance is measured from the last TRANSMITTED position; comparing
  // against the previous reading would forgive every step forever.
  AdvertScheduler s;
  s.begin(defaultCfg(), 0, SEED);

  uint32_t t = 0;
  ASSERT_EQ(AdvertScheduler::REASON_FIRST_FIX, s.poll(t, true, LAT0, LON0));

  int fired_at = -1;
  for (int step = 1; step <= 15; step++) {
    t += 1000;   // keep well inside the interval so only distance can trigger
    AdvertScheduler::Reason r = s.poll(t, true, metresNorth(10.0f * step), LON0);
    if (r == AdvertScheduler::REASON_MOVED) { fired_at = step; break; }
    ASSERT_EQ(AdvertScheduler::REASON_NONE, r) << "at step " << step;
  }

  ASSERT_NE(-1, fired_at) << "cumulative drift never triggered";
  EXPECT_GE(fired_at, 10);   // not before ~100 m of travel
  EXPECT_LE(fired_at, 11);
}

TEST(AdvertScheduler, DistanceIsMeasuredFromTheLastTransmittedPosition) {
  AdvertScheduler s;
  s.begin(defaultCfg(), 0, SEED);

  uint32_t t = 0;
  ASSERT_EQ(AdvertScheduler::REASON_FIRST_FIX, s.poll(t, true, LAT0, LON0));
  EXPECT_EQ(LAT0, s.getLastSentLat());

  t += 1000;
  ASSERT_EQ(AdvertScheduler::REASON_MOVED, s.poll(t, true, metresNorth(150), LON0));
  EXPECT_EQ(metresNorth(150), s.getLastSentLat());

  // 150 m further from the origin, but only 0 m from where we last transmitted
  t += 1000;
  EXPECT_EQ(AdvertScheduler::REASON_NONE, s.poll(t, true, metresNorth(150), LON0));
}

TEST(AdvertScheduler, ZeroThresholdDisablesTheDistanceTrigger) {
  AdvertScheduler::Config cfg = defaultCfg();
  cfg.dist_threshold_m = 0;
  AdvertScheduler s;
  s.begin(cfg, 0, SEED);

  uint32_t t = 0;
  ASSERT_EQ(AdvertScheduler::REASON_FIRST_FIX, s.poll(t, true, LAT0, LON0));

  t += 1000;
  EXPECT_EQ(AdvertScheduler::REASON_NONE, s.poll(t, true, metresNorth(50000), LON0));
}

// ---------------------------------------------------------------- edge geometry

TEST(AdvertScheduler, CrossingTheAntimeridianIsNotHalfAPlanetOfTravel) {
  AdvertScheduler s;
  s.begin(defaultCfg(), 0, SEED);

  uint32_t t = 0;
  ASSERT_EQ(AdvertScheduler::REASON_FIRST_FIX, s.poll(t, true, 0, 179999900));

  // a ~22 m step east, straight over the +/-180 seam
  t += 1000;
  EXPECT_EQ(AdvertScheduler::REASON_NONE, s.poll(t, true, 0, -179999900));
}

TEST(AdvertScheduler, HighLatitudeDoesNotCollapseOrExplode) {
  AdvertScheduler s;
  s.begin(defaultCfg(), 0, SEED);

  const int32_t near_pole = 89500000;   // 89.5 degrees N

  uint32_t t = 0;
  ASSERT_EQ(AdvertScheduler::REASON_FIRST_FIX, s.poll(t, true, near_pole, 0));

  // longitude is heavily foreshortened up here, so a big lon step is a small distance
  t += 1000;
  EXPECT_EQ(AdvertScheduler::REASON_NONE, s.poll(t, true, near_pole, 5000));

  // but latitude still converts normally
  t += 1000;
  EXPECT_EQ(AdvertScheduler::REASON_MOVED, s.poll(t, true, near_pole + metresNorth(200), 5000));
}

// ---------------------------------------------------------------- clock handling

TEST(AdvertScheduler, SurvivesMillisWrapAround) {
  AdvertScheduler s;
  s.begin(defaultCfg(), 0xFFFFFF00u, SEED);

  uint32_t t = 0xFFFFFF00u;
  ASSERT_EQ(AdvertScheduler::REASON_FIRST_FIX, s.poll(t, true, LAT0, LON0));

  // half way to due, having wrapped through zero
  uint32_t half = t + 30000;
  EXPECT_EQ(AdvertScheduler::REASON_NONE, s.poll(half, true, LAT0, LON0));

  // and now due, still on the far side of the wrap
  uint32_t due = t + 60000;
  EXPECT_EQ(AdvertScheduler::REASON_INTERVAL, s.poll(due, true, LAT0, LON0));
  EXPECT_EQ(120u, s.getIntervalSecs());
}

TEST(AdvertScheduler, ResetReturnsToTheFloor) {
  AdvertScheduler s;
  s.begin(defaultCfg(), 0, SEED);

  uint32_t t = 0;
  ASSERT_EQ(AdvertScheduler::REASON_FIRST_FIX, s.poll(t, true, LAT0, LON0));
  for (int i = 0; i < 4; i++) {
    t += s.getIntervalSecs() * 1000;
    ASSERT_EQ(AdvertScheduler::REASON_INTERVAL, s.poll(t, true, LAT0, LON0));
  }
  ASSERT_GT(s.getIntervalSecs(), 60u);

  s.reset(t);
  EXPECT_EQ(60u, s.getIntervalSecs());
  EXPECT_EQ(AdvertScheduler::REASON_NONE, s.poll(t + 59000, true, LAT0, LON0));
  EXPECT_EQ(AdvertScheduler::REASON_INTERVAL, s.poll(t + 60000, true, LAT0, LON0));
}

TEST(AdvertScheduler, NonsenseConfigIsClampedRatherThanObeyed) {
  AdvertScheduler::Config cfg;
  cfg.min_interval_secs = 0;      // would mean "send constantly"
  cfg.max_interval_secs = 10;     // below the floor
  cfg.dist_threshold_m = 100;
  cfg.backoff_factor = 0;         // would mean "multiply by zero"
  cfg.jitter_pct = 200;           // would swing a deadline past zero
  cfg.startup_spread_secs = 0;
  AdvertScheduler s;
  s.begin(cfg, 0, SEED);

  uint32_t t = 0;
  ASSERT_EQ(AdvertScheduler::REASON_FIRST_FIX, s.poll(t, true, LAT0, LON0));
  EXPECT_GE(s.getIntervalSecs(), 1u);

  for (int i = 0; i < 5; i++) {
    t += s.getIntervalSecs() * 1000 * 2;   // enough to clear even a +50% jittered deadline
    ASSERT_EQ(AdvertScheduler::REASON_INTERVAL, s.poll(t, true, LAT0, LON0));
    EXPECT_GE(s.getIntervalSecs(), 1u);
  }
}

// ---------------------------------------------------------------- jitter

// Run a stationary node and record when it transmits.
static std::vector<uint32_t> collectSendTimes(AdvertScheduler& s, uint32_t duration_ms, uint32_t step_ms) {
  std::vector<uint32_t> out;
  for (uint32_t t = 0; t <= duration_ms; t += step_ms) {
    if (s.poll(t, true, LAT0, LON0) != AdvertScheduler::REASON_NONE) out.push_back(t);
  }
  return out;
}

static AdvertScheduler::Config flatCfg(uint8_t jitter_pct, uint32_t period_secs) {
  AdvertScheduler::Config cfg = defaultCfg();
  cfg.min_interval_secs = period_secs;   // backoff disabled, so every gap is nominally equal
  cfg.max_interval_secs = period_secs;
  cfg.backoff_factor = 1;
  cfg.jitter_pct = jitter_pct;
  return cfg;
}

TEST(AdvertSchedulerJitter, ZeroJitterIsExactlyPeriodic) {
  AdvertScheduler s;
  s.begin(flatCfg(0, 600), 0, SEED);
  auto times = collectSendTimes(s, 20u * 600u * 1000u, 500);

  ASSERT_GT(times.size(), 10u);
  for (size_t i = 2; i < times.size(); i++) {
    EXPECT_EQ(600000u, times[i] - times[i - 1]) << "gap " << i;
  }
}

TEST(AdvertSchedulerJitter, GapsStayWithinTheConfiguredSpread) {
  AdvertScheduler s;
  s.begin(flatCfg(20, 600), 0, SEED);
  auto times = collectSendTimes(s, 200u * 600u * 1000u, 500);

  ASSERT_GT(times.size(), 100u);
  for (size_t i = 2; i < times.size(); i++) {
    uint32_t gap = times[i] - times[i - 1];
    EXPECT_GE(gap, 480000u - 500u) << "gap " << i << " too short";
    EXPECT_LE(gap, 720000u + 500u) << "gap " << i << " too long";
  }
}

TEST(AdvertSchedulerJitter, SpreadIsSymmetricSoTheMeanRateIsUnchanged) {
  // Jitter must not quietly speed the fleet up or slow it down - the airtime budget
  // is calculated from the nominal interval.
  AdvertScheduler s;
  s.begin(flatCfg(20, 600), 0, SEED);
  auto times = collectSendTimes(s, 200u * 600u * 1000u, 500);

  ASSERT_GT(times.size(), 100u);
  double total = 0;
  int n = 0;
  for (size_t i = 2; i < times.size(); i++) { total += (times[i] - times[i - 1]); n++; }
  double mean = total / n;

  EXPECT_NEAR(600000.0, mean, 600000.0 * 0.05);
}

TEST(AdvertSchedulerJitter, BackoffStateStaysExact) {
  // The jitter lands on the deadline, never on the interval itself. If it leaked into
  // the state, the doubling sequence would wander away from its configured shape.
  AdvertScheduler::Config cfg = defaultCfg();
  cfg.jitter_pct = 25;
  AdvertScheduler s;
  s.begin(cfg, 0, SEED);

  uint32_t t = 0;
  ASSERT_EQ(AdvertScheduler::REASON_FIRST_FIX, s.poll(t, true, LAT0, LON0));
  ASSERT_EQ(60u, s.getIntervalSecs());

  const uint32_t expected[] = { 120, 240, 480, 960, 1920, 3600, 3600 };
  size_t idx = 0;
  for (t = 0; t < 30000u * 1000u && idx < 7; t += 1000) {
    if (s.poll(t, true, LAT0, LON0) == AdvertScheduler::REASON_INTERVAL) {
      EXPECT_EQ(expected[idx], s.getIntervalSecs()) << "step " << idx;
      idx++;
    }
  }
  EXPECT_EQ(7u, idx);
}

TEST(AdvertSchedulerJitter, NodesStartedTogetherDoNotStayInLockstep) {
  // The failure this exists to prevent: a fleet powering up together, stepping through
  // an identical deterministic backoff, and transmitting in synchronised bursts forever.
  const uint32_t DURATION = 6u * 3600u * 1000u;

  // control: no jitter, different seeds - they must collide on every single send
  {
    AdvertScheduler a, b;
    a.begin(flatCfg(0, 300), 0, 1111);
    b.begin(flatCfg(0, 300), 0, 2222);
    auto ta = collectSendTimes(a, DURATION, 1000);
    auto tb = collectSendTimes(b, DURATION, 1000);
    ASSERT_GT(ta.size(), 50u);
    ASSERT_EQ(ta, tb) << "without jitter two nodes should be perfectly in lockstep";
  }

  // with jitter, near-total separation
  {
    AdvertScheduler a, b;
    a.begin(flatCfg(15, 300), 0, 1111);
    b.begin(flatCfg(15, 300), 0, 2222);
    auto ta = collectSendTimes(a, DURATION, 1000);
    auto tb = collectSendTimes(b, DURATION, 1000);
    ASSERT_GT(ta.size(), 50u);

    int coincident = 0;
    for (uint32_t x : ta) {
      if (std::find(tb.begin(), tb.end(), x) != tb.end()) coincident++;
    }
    EXPECT_LE(coincident, 2) << coincident << " of " << ta.size() << " sends still collided";
  }
}

TEST(AdvertSchedulerJitter, StartupSpreadHoldsBackTheFirstAdvert) {
  AdvertScheduler::Config cfg = defaultCfg();
  cfg.startup_spread_secs = 600;

  std::vector<uint32_t> firsts;
  for (uint32_t seed = 1; seed <= 8; seed++) {
    AdvertScheduler s;
    s.begin(cfg, 0, seed * 7919);

    uint32_t first = 0;
    for (uint32_t t = 0; t <= 600000u; t += 1000) {
      // a fix is available from the very first poll - only the spread holds it back
      if (s.poll(t, true, LAT0, LON0) == AdvertScheduler::REASON_FIRST_FIX) { first = t; break; }
    }
    ASSERT_GT(first, 0u) << "seed " << seed << " never sent inside the spread window";
    EXPECT_LE(first, 600000u);
    firsts.push_back(first);
  }

  // and those first transmissions must actually be spread out, not clustered
  std::sort(firsts.begin(), firsts.end());
  EXPECT_GT(firsts.back() - firsts.front(), 120000u);
}

TEST(AdvertSchedulerJitter, ZeroStartupSpreadKeepsSendingOnFirstFix) {
  AdvertScheduler::Config cfg = defaultCfg();
  cfg.jitter_pct = 20;
  cfg.startup_spread_secs = 0;
  AdvertScheduler s;
  s.begin(cfg, 0, SEED);
  EXPECT_EQ(AdvertScheduler::REASON_FIRST_FIX, s.poll(0, true, LAT0, LON0));
}

TEST(AdvertSchedulerJitter, SameSeedReproducesAndZeroSeedIsSafe) {
  AdvertScheduler a, b;
  a.begin(flatCfg(20, 300), 0, 12345);
  b.begin(flatCfg(20, 300), 0, 12345);
  EXPECT_EQ(collectSendTimes(a, 3600u * 1000u, 1000), collectSendTimes(b, 3600u * 1000u, 1000));

  // xorshift is stuck at zero forever if seeded with it
  AdvertScheduler z;
  z.begin(flatCfg(20, 300), 0, 0);
  auto times = collectSendTimes(z, 3600u * 1000u, 1000);
  ASSERT_GT(times.size(), 5u);
  bool varies = false;
  for (size_t i = 3; i < times.size(); i++) {
    if ((times[i] - times[i - 1]) != (times[2] - times[1])) { varies = true; break; }
  }
  EXPECT_TRUE(varies) << "seed 0 produced a degenerate, non-varying sequence";
}

TEST(AdvertScheduler, AFailedSendDoesNotSwallowTheMovementTrigger) {
  // poll() commits as it decides - it moves the distance reference to the position it is
  // about to beacon. If the send then fails and nothing hands that back, the travel that
  // triggered it is forgotten and the update is lost until the interval elapses.
  AdvertScheduler s;
  AdvertScheduler::Config cfg;
  cfg.min_interval_secs = 600;        // long, so only movement can trigger below
  cfg.max_interval_secs = 3600;
  cfg.dist_threshold_m = 100;
  cfg.jitter_pct = 0;
  cfg.startup_spread_secs = 0;
  s.begin(cfg, 0, 0xABCD);

  uint32_t t = 1000;
  ASSERT_EQ(AdvertScheduler::REASON_FIRST_FIX, s.poll(t, true, 51500000, -120000));

  // travel far enough to trigger, and suppose the send fails
  t += 60000;
  ASSERT_EQ(AdvertScheduler::REASON_MOVED, s.poll(t, true, 51502000, -120000));
  s.undoSend(t);

  // the very next poll must want to send again, at the same place
  t += 1000;
  EXPECT_NE(AdvertScheduler::REASON_NONE, s.poll(t, true, 51502000, -120000))
      << "the movement that triggered the failed send was forgotten";
}

TEST(AdvertScheduler, UndoIsOnlyNeededOnceTheSendHasFailed) {
  // the ordinary path is untouched: a successful send still parks until the next trigger
  AdvertScheduler s;
  AdvertScheduler::Config cfg;
  cfg.min_interval_secs = 600;
  cfg.max_interval_secs = 3600;
  cfg.dist_threshold_m = 100;
  cfg.jitter_pct = 0;
  cfg.startup_spread_secs = 0;
  s.begin(cfg, 0, 0xABCD);

  uint32_t t = 1000;
  ASSERT_EQ(AdvertScheduler::REASON_FIRST_FIX, s.poll(t, true, 51500000, -120000));
  t += 60000;
  ASSERT_EQ(AdvertScheduler::REASON_MOVED, s.poll(t, true, 51502000, -120000));
  t += 1000;
  EXPECT_EQ(AdvertScheduler::REASON_NONE, s.poll(t, true, 51502000, -120000));
}

int main(int argc, char **argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
