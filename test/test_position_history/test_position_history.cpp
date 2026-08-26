#include <gtest/gtest.h>
#include <helpers/PositionHistory.h>
#include <string.h>

// Boston-ish, where a micro-degree of longitude is about 0.075 m
static const int32_t LAT0 = 42360000;
static const int32_t LON0 = -71058000;

// metres north as micro-degrees, which convert independently of the cos(lat) scaling
static int32_t north(float m) { return (int32_t)(m / 0.111320f); }

static PositionSample sample(uint32_t t, int32_t lat, int32_t lon) {
  PositionSample s;
  s.timestamp = t;
  s.lat_e6 = lat;
  s.lon_e6 = lon;
  return s;
}

static PositionHistoryReq reqFor(uint32_t since, uint16_t every_secs, uint16_t every_metres) {
  PositionHistoryReq r;
  r.version = POS_HIST_VERSION;
  r.since = since;
  r.every_secs = every_secs;
  r.every_metres = every_metres;
  return r;
}

// select everything a cursor will give, in as many calls as it takes
static int drain(const PositionHistory& h, PositionHistory::Cursor& c,
                 PositionSample* dest, int max, int per_call = 4) {
  int total = 0;
  for (;;) {
    int n = h.next(c, &dest[total], (total + per_call > max) ? (max - total) : per_call);
    if (n == 0) break;
    total += n;
    if (total >= max) break;
  }
  return total;
}

// --------------------------------------------------------------------------- the ring

TEST(PositionHistory, StartsEmpty) {
  PositionSample storage[8];
  PositionHistory h;
  h.begin(storage, 8);

  EXPECT_EQ(0, h.count());
  EXPECT_TRUE(h.isEmpty());

  PositionHistory::Cursor c;
  h.start(c, reqFor(0, 0, 0));
  PositionSample got[8];
  EXPECT_EQ(0, h.next(c, got, 8));
  EXPECT_TRUE(h.isFinished(c));
}

TEST(PositionHistory, KeepsSamplesOldestFirst) {
  PositionSample storage[8];
  PositionHistory h;
  h.begin(storage, 8);

  for (int i = 0; i < 5; i++) h.add(sample(1000 + i * 60, LAT0 + north(i * 10.0f), LON0));

  ASSERT_EQ(5, h.count());
  PositionSample s;
  ASSERT_TRUE(h.getSeq(0, s));
  EXPECT_EQ(1000u, s.timestamp);
  ASSERT_TRUE(h.getSeq(4, s));
  EXPECT_EQ(1240u, s.timestamp);
  EXPECT_FALSE(h.getSeq(5, s)) << "one past the end";
}

TEST(PositionHistory, DropsTheOldestWhenFull) {
  PositionSample storage[4];
  PositionHistory h;
  h.begin(storage, 4);

  for (int i = 0; i < 10; i++) h.add(sample(1000 + i * 60, LAT0, LON0));

  EXPECT_EQ(4, h.count());
  EXPECT_EQ(6u, h.oldestSeq());
  EXPECT_EQ(10u, h.nextSeq());

  PositionSample s;
  EXPECT_FALSE(h.getSeq(5, s)) << "fallen off the ring";
  ASSERT_TRUE(h.getSeq(6, s));
  EXPECT_EQ(1000u + 6 * 60, s.timestamp);
  ASSERT_TRUE(h.getSeq(9, s));
  EXPECT_EQ(1000u + 9 * 60, s.timestamp);
}

TEST(PositionHistory, SurvivesWrappingManyTimesOver) {
  PositionSample storage[3];
  PositionHistory h;
  h.begin(storage, 3);

  for (int i = 0; i < 1000; i++) h.add(sample(1000 + i, LAT0 + north((float)i), LON0));

  EXPECT_EQ(3, h.count());
  PositionSample s;
  ASSERT_TRUE(h.getSeq(999, s));
  EXPECT_EQ(1999u, s.timestamp);
  ASSERT_TRUE(h.getSeq(997, s));
  EXPECT_EQ(1997u, s.timestamp);
  EXPECT_FALSE(h.getSeq(996, s));
}

TEST(PositionHistory, StorageIsOptional) {
  PositionHistory h;                 // never given any - the feature compiled out
  h.add(sample(1000, LAT0, LON0));   // must not write anywhere
  EXPECT_EQ(0, h.count());

  PositionHistory::Cursor c;
  h.start(c, reqFor(0, 0, 0));
  PositionSample got[4];
  EXPECT_EQ(0, h.next(c, got, 4));
}

// -------------------------------------------------------------------------- selecting

TEST(PositionHistory, SinceExcludesEarlierSamples) {
  PositionSample storage[16];
  PositionHistory h;
  h.begin(storage, 16);

  for (int i = 0; i < 10; i++) h.add(sample(1000 + i * 60, LAT0, LON0));

  PositionHistory::Cursor c;
  h.start(c, reqFor(1300, 0, 0));   // 1300 = the sample at i=5

  PositionSample got[16];
  int n = drain(h, c, got, 16);
  ASSERT_EQ(5, n);
  EXPECT_EQ(1300u, got[0].timestamp) << "the boundary sample itself is included";
  EXPECT_EQ(1540u, got[4].timestamp);
}

TEST(PositionHistory, EverySecondsKeepsOneSamplePerInterval) {
  PositionSample storage[32];
  PositionHistory h;
  h.begin(storage, 32);

  // a sample a minute for half an hour
  for (int i = 0; i < 30; i++) h.add(sample(10000 + i * 60, LAT0, LON0));

  PositionHistory::Cursor c;
  h.start(c, reqFor(0, 300, 0));   // every 5 minutes

  PositionSample got[32];
  int n = drain(h, c, got, 32);
  ASSERT_EQ(6, n);
  for (int i = 0; i < n; i++) {
    EXPECT_EQ(10000u + i * 300, got[i].timestamp) << "at " << i;
  }
}

TEST(PositionHistory, EverySecondsMeasuresFromTheLastKeptSample) {
  PositionSample storage[16];
  PositionHistory h;
  h.begin(storage, 16);

  // an irregular run: measuring from the last sample LOOKED AT rather than the last one
  // KEPT would let the interval drift
  h.add(sample(1000, LAT0, LON0));   // first eligible, kept
  h.add(sample(1100, LAT0, LON0));   // +100 on 1000, dropped
  h.add(sample(1250, LAT0, LON0));   // +250 on 1000, kept
  h.add(sample(1300, LAT0, LON0));   // +50 on 1250, dropped
  h.add(sample(1450, LAT0, LON0));   // +200 on 1250, exactly the interval, kept
  h.add(sample(1500, LAT0, LON0));   // +50 on 1450, dropped

  PositionHistory::Cursor c;
  h.start(c, reqFor(0, 200, 0));

  PositionSample got[16];
  int n = drain(h, c, got, 16);
  ASSERT_EQ(3, n);
  EXPECT_EQ(1000u, got[0].timestamp);
  EXPECT_EQ(1250u, got[1].timestamp);
  EXPECT_EQ(1450u, got[2].timestamp) << "measured from 1250, not from the 1300 in between";
}

TEST(PositionHistory, EveryMetresKeepsOneSamplePerDistance) {
  PositionSample storage[32];
  PositionHistory h;
  h.begin(storage, 32);

  // 20 m of travel a minute, due north
  for (int i = 0; i < 20; i++) h.add(sample(10000 + i * 60, LAT0 + north(i * 20.0f), LON0));

  // 90 rather than 100: a sample is stored as whole micro-degrees, so a point meant to
  // be exactly 100 m away lands a few centimetres short of it and a threshold sitting on
  // the sample spacing would be deciding on rounding rather than on distance
  PositionHistory::Cursor c;
  h.start(c, reqFor(0, 0, 90));

  PositionSample got[32];
  int n = drain(h, c, got, 32);
  ASSERT_EQ(4, n);   // 0 m, then the first sample past each 90 m: 100, 200, 300 of 380 m
  EXPECT_EQ(10000u, got[0].timestamp);
  EXPECT_EQ(10000u + 5 * 60, got[1].timestamp);
  EXPECT_EQ(10000u + 10 * 60, got[2].timestamp);
  EXPECT_EQ(10000u + 15 * 60, got[3].timestamp);
}

TEST(PositionHistory, EveryMetresIgnoresTimeSpentStill) {
  PositionSample storage[32];
  PositionHistory h;
  h.begin(storage, 32);

  h.add(sample(10000, LAT0, LON0));
  for (int i = 1; i < 20; i++) h.add(sample(10000 + i * 60, LAT0, LON0));   // parked
  h.add(sample(20000, LAT0 + north(500.0f), LON0));                          // then a move

  PositionHistory::Cursor c;
  h.start(c, reqFor(0, 0, 100));

  PositionSample got[32];
  int n = drain(h, c, got, 32);
  ASSERT_EQ(2, n) << "a stationary node is one point, however long it sat there";
  EXPECT_EQ(10000u, got[0].timestamp);
  EXPECT_EQ(20000u, got[1].timestamp);
}

TEST(PositionHistory, NeitherThresholdKeepsEverything) {
  PositionSample storage[16];
  PositionHistory h;
  h.begin(storage, 16);

  for (int i = 0; i < 10; i++) h.add(sample(1000 + i, LAT0, LON0));

  PositionHistory::Cursor c;
  h.start(c, reqFor(0, 0, 0));
  PositionSample got[16];
  EXPECT_EQ(10, drain(h, c, got, 16)) << "no thinning asked for is everything, not nothing";
}

// ----------------------------------------------------------- packing a report from it

TEST(PositionHistory, NewestTakesTheEndOfTheRing) {
  PositionSample storage[32];
  PositionHistory h;
  h.begin(storage, 32);

  for (int i = 0; i < 20; i++) h.add(sample(1000 + i * 60, LAT0, LON0));

  PositionSample got[8];
  ASSERT_EQ(5, h.newest(got, 5));
  // oldest-first, because that is the order the report format encodes in
  EXPECT_EQ(1000u + 15 * 60, got[0].timestamp);
  EXPECT_EQ(1000u + 19 * 60, got[4].timestamp) << "the last one must be the newest sample";
}

TEST(PositionHistory, NewestTakesWhatThereIsWhenItIsShort) {
  PositionSample storage[32];
  PositionHistory h;
  h.begin(storage, 32);

  h.add(sample(1000, LAT0, LON0));
  h.add(sample(1060, LAT0, LON0));

  PositionSample got[8];
  ASSERT_EQ(2, h.newest(got, 8));
  EXPECT_EQ(1000u, got[0].timestamp);
  EXPECT_EQ(1060u, got[1].timestamp);

  PositionHistory empty;
  empty.begin(storage, 32);
  EXPECT_EQ(0, empty.newest(got, 8));
}

TEST(PositionHistory, NewestFollowsTheRingAsItWraps) {
  PositionSample storage[4];
  PositionHistory h;
  h.begin(storage, 4);

  for (int i = 0; i < 10; i++) h.add(sample(1000 + i * 60, LAT0, LON0));

  PositionSample got[8];
  ASSERT_EQ(3, h.newest(got, 3));
  EXPECT_EQ(1000u + 7 * 60, got[0].timestamp);
  EXPECT_EQ(1000u + 9 * 60, got[2].timestamp);

  ASSERT_EQ(4, h.newest(got, 8)) << "never more than the ring holds";
  EXPECT_EQ(1000u + 6 * 60, got[0].timestamp);
}

TEST(PositionHistory, SendingNothingChangesWhatIsHeld) {
  PositionSample storage[8];
  PositionHistory h;
  h.begin(storage, 8);

  for (int i = 0; i < 5; i++) h.add(sample(1000 + i * 60, LAT0, LON0));

  PositionSample got[8];
  h.newest(got, 3);   // as a report would
  h.newest(got, 3);

  EXPECT_EQ(5, h.count()) << "a report consumes nothing";
  PositionHistory::Cursor c;
  h.start(c, reqFor(0, 0, 0));
  EXPECT_EQ(5, drain(h, c, got, 8)) << "including the ones already broadcast";
}

// ------------------------------------------------------------------ both thresholds
//
// Asked for together they are an OR: a sample is kept as soon as either has been passed,
// and each covers what the other misses.

TEST(PositionHistory, DistanceKeepsWhatTimeAloneWouldMiss) {
  PositionSample storage[16];
  PositionHistory h;
  h.begin(storage, 16);

  h.add(sample(1000, LAT0, LON0));                        // kept, the first one
  h.add(sample(1060, LAT0 + north(400.0f), LON0));        // 60 s but 400 m: kept on distance
  h.add(sample(1120, LAT0 + north(430.0f), LON0));        // 60 s and 30 m: thinned out

  PositionHistory::Cursor c;
  h.start(c, reqFor(0, 300, 90));   // every 5 minutes OR every 90 metres

  PositionSample got[16];
  int n = drain(h, c, got, 16);
  ASSERT_EQ(2, n);
  EXPECT_EQ(1000u, got[0].timestamp);
  EXPECT_EQ(1060u, got[1].timestamp) << "a node that covered 400 m inside a minute";
}

TEST(PositionHistory, TimeKeepsWhatDistanceAloneWouldMiss) {
  PositionSample storage[16];
  PositionHistory h;
  h.begin(storage, 16);

  h.add(sample(1000, LAT0, LON0));    // kept, the first one
  h.add(sample(1100, LAT0, LON0));    // 100 s parked: neither threshold, thinned out
  h.add(sample(1300, LAT0, LON0));    // 300 s on the last kept: kept on time
  h.add(sample(1400, LAT0, LON0));    // 100 s on that: thinned out

  PositionHistory::Cursor c;
  h.start(c, reqFor(0, 300, 90));

  PositionSample got[16];
  int n = drain(h, c, got, 16);
  ASSERT_EQ(2, n);
  EXPECT_EQ(1000u, got[0].timestamp);
  EXPECT_EQ(1300u, got[1].timestamp) << "a node that has not moved is still somewhere";
}

TEST(PositionHistory, EitherThresholdAloneStillDecides) {
  PositionSample storage[16];
  PositionHistory h;
  h.begin(storage, 16);

  h.add(sample(1000, LAT0, LON0));
  h.add(sample(1300, LAT0, LON0));                    // 300 s, no movement
  h.add(sample(1310, LAT0 + north(400.0f), LON0));    // 10 s, 400 m

  {   // distance off: only the interval decides
    PositionHistory::Cursor c;
    h.start(c, reqFor(0, 300, 0));
    PositionSample got[16];
    int n = drain(h, c, got, 16);
    ASSERT_EQ(2, n);
    EXPECT_EQ(1300u, got[1].timestamp);
  }
  {   // time off: only the distance decides
    PositionHistory::Cursor c;
    h.start(c, reqFor(0, 0, 90));
    PositionSample got[16];
    int n = drain(h, c, got, 16);
    ASSERT_EQ(2, n);
    EXPECT_EQ(1310u, got[1].timestamp);
  }
}

TEST(PositionHistory, KeepsASampleStampedBeforeTheLastOneKept) {
  PositionSample storage[8];
  PositionHistory h;
  h.begin(storage, 8);

  h.add(sample(50000, LAT0, LON0));
  h.add(sample(40000, LAT0, LON0));   // the RTC was corrected backwards between samples

  PositionHistory::Cursor c;
  h.start(c, reqFor(0, 300, 0));

  PositionSample got[8];
  int n = drain(h, c, got, 8);
  ASSERT_EQ(2, n) << "a clock stepping back must not swallow positions";
  EXPECT_EQ(40000u, got[1].timestamp);
}

// ---------------------------------------------------------------------------- cursors

TEST(PositionHistory, StreamsAcrossSeveralCalls) {
  PositionSample storage[32];
  PositionHistory h;
  h.begin(storage, 32);

  for (int i = 0; i < 12; i++) h.add(sample(1000 + i * 60, LAT0, LON0));

  PositionHistory::Cursor c;
  h.start(c, reqFor(0, 0, 0));

  PositionSample got[32];
  int first = h.next(c, got, 5);
  ASSERT_EQ(5, first);
  EXPECT_FALSE(h.isFinished(c));

  int second = h.next(c, &got[5], 5);
  ASSERT_EQ(5, second);

  int third = h.next(c, &got[10], 5);
  ASSERT_EQ(2, third);
  EXPECT_TRUE(h.isFinished(c));

  for (int i = 0; i < 12; i++) EXPECT_EQ(1000u + i * 60, got[i].timestamp) << "at " << i;
  EXPECT_EQ(0, h.next(c, got, 5)) << "nothing left";
}

TEST(PositionHistory, SeesSamplesAddedWhileItIsBeingAnswered) {
  PositionSample storage[32];
  PositionHistory h;
  h.begin(storage, 32);

  for (int i = 0; i < 4; i++) h.add(sample(1000 + i * 60, LAT0, LON0));

  PositionHistory::Cursor c;
  h.start(c, reqFor(0, 0, 0));

  PositionSample got[32];
  ASSERT_EQ(4, h.next(c, got, 32));
  EXPECT_TRUE(h.isFinished(c));

  h.add(sample(1240, LAT0, LON0));   // a new fix mid-answer
  EXPECT_FALSE(h.isFinished(c));
  ASSERT_EQ(1, h.next(c, got, 32));
  EXPECT_EQ(1240u, got[0].timestamp);
}

TEST(PositionHistory, DoesNotSkipWhenTheRingWrapsMidAnswer) {
  PositionSample storage[4];
  PositionHistory h;
  h.begin(storage, 4);

  for (int i = 0; i < 4; i++) h.add(sample(1000 + i * 60, LAT0, LON0));

  PositionHistory::Cursor c;
  h.start(c, reqFor(0, 0, 0));

  PositionSample got[8];
  ASSERT_EQ(2, h.next(c, got, 2));     // 1000, 1060 - two still to go
  EXPECT_EQ(1060u, got[1].timestamp);

  h.add(sample(1240, LAT0, LON0));     // evicts 1000, which this cursor has passed anyway
  ASSERT_EQ(2, h.next(c, got, 2));
  EXPECT_EQ(1120u, got[0].timestamp) << "an index would have slid by one and skipped this";
  EXPECT_EQ(1180u, got[1].timestamp);
  EXPECT_FALSE(c.lost);
}

TEST(PositionHistory, SaysSoWhenTheRingOutranAnAnswer) {
  PositionSample storage[4];
  PositionHistory h;
  h.begin(storage, 4);

  for (int i = 0; i < 4; i++) h.add(sample(1000 + i * 60, LAT0, LON0));

  PositionHistory::Cursor c;
  h.start(c, reqFor(0, 0, 0));

  PositionSample got[8];
  ASSERT_EQ(1, h.next(c, got, 1));   // took 1000; 1060 is next

  for (int i = 0; i < 4; i++) h.add(sample(2000 + i * 60, LAT0, LON0));   // everything it had left

  ASSERT_EQ(4, h.next(c, got, 8));
  EXPECT_TRUE(c.lost);
  EXPECT_EQ(2000u, got[0].timestamp);
}

// ----------------------------------------------------------------------- request body

TEST(PositionHistoryReqCoding, RoundTrips) {
  PositionHistoryReq in = reqFor(1750000000u, 300, 250);
  uint8_t buf[POS_HIST_REQ_BODY_LEN];

  ASSERT_EQ(POS_HIST_REQ_BODY_LEN, PositionHistory::encodeReqBody(buf, sizeof(buf), in));

  PositionHistoryReq out;
  ASSERT_TRUE(PositionHistory::decodeReqBody(buf, sizeof(buf), out));
  EXPECT_EQ(in.version, out.version);
  EXPECT_EQ(in.since, out.since);
  EXPECT_EQ(in.every_secs, out.every_secs);
  EXPECT_EQ(in.every_metres, out.every_metres);
}

TEST(PositionHistoryReqCoding, IsLittleEndianOnTheWire) {
  PositionHistoryReq in = reqFor(0x04030201u, 0x0605, 0x0807);
  uint8_t buf[POS_HIST_REQ_BODY_LEN];
  ASSERT_EQ(POS_HIST_REQ_BODY_LEN, PositionHistory::encodeReqBody(buf, sizeof(buf), in));

  const uint8_t expect[POS_HIST_REQ_BODY_LEN] = {
    POS_HIST_VERSION, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08
  };
  EXPECT_EQ(0, memcmp(expect, buf, sizeof(expect)));
}

TEST(PositionHistoryReqCoding, RejectsWhatItCannotAnswer) {
  uint8_t buf[POS_HIST_REQ_BODY_LEN];
  PositionHistoryReq out;

  PositionHistoryReq ok = reqFor(1000, 60, 0);
  ASSERT_EQ(POS_HIST_REQ_BODY_LEN, PositionHistory::encodeReqBody(buf, sizeof(buf), ok));

  EXPECT_FALSE(PositionHistory::decodeReqBody(buf, POS_HIST_REQ_BODY_LEN - 1, out)) << "short body";
  EXPECT_FALSE(PositionHistory::decodeReqBody(NULL, POS_HIST_REQ_BODY_LEN, out)) << "no body";

  uint8_t bad_version[POS_HIST_REQ_BODY_LEN];
  memcpy(bad_version, buf, sizeof(buf));
  bad_version[0] = POS_HIST_VERSION + 1;
  EXPECT_FALSE(PositionHistory::decodeReqBody(bad_version, sizeof(bad_version), out));

  // both thresholds zero is a request for everything, and a perfectly good one
  PositionHistoryReq everything = reqFor(1000, 0, 0);
  ASSERT_EQ(POS_HIST_REQ_BODY_LEN, PositionHistory::encodeReqBody(buf, sizeof(buf), everything));
  EXPECT_TRUE(PositionHistory::decodeReqBody(buf, sizeof(buf), out));
}

TEST(PositionHistoryReqCoding, RefusesToOverrunTheBuffer) {
  uint8_t small[POS_HIST_REQ_BODY_LEN - 1];
  PositionHistoryReq in = reqFor(1000, 60, 0);
  EXPECT_EQ(0, PositionHistory::encodeReqBody(small, sizeof(small), in));
}

int main(int argc, char **argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
