#include <gtest/gtest.h>
#include <helpers/PositionReport.h>
#include <MeshCore.h>
#include <string.h>
#include <vector>

static const uint8_t PREFIX[POS_PREFIX_LEN] = { 0xDE, 0xAD, 0xBE, 0xEF, 0x01, 0x02 };

// Same budget the firmware has: a group datagram minus the GRP_DATA sub-header.
static const size_t BUDGET = MAX_GROUP_DATA_LENGTH - 3;

static PositionSample mk(uint32_t t, int32_t lat, int32_t lon) {
  PositionSample s; s.timestamp = t; s.lat_e6 = lat; s.lon_e6 = lon; return s;
}

// ---------------------------------------------------------------- round trip

TEST(PositionReport, RoundTripsASingleSample) {
  PositionSample in[] = { mk(1750000000, 51500000, -120000) };
  uint8_t buf[BUDGET];
  int consumed = 0;
  int n = PositionReport::encode(buf, sizeof(buf), PREFIX, in, 1, &consumed);
  ASSERT_GT(n, 0);
  EXPECT_EQ(1, consumed);
  EXPECT_EQ(POS_HEADER_LEN, n);

  uint8_t prefix[POS_PREFIX_LEN];
  PositionSample out[8];
  ASSERT_EQ(1, PositionReport::decode(buf, n, prefix, out, 8));
  EXPECT_EQ(0, memcmp(PREFIX, prefix, POS_PREFIX_LEN));
  EXPECT_EQ(in[0].timestamp, out[0].timestamp);
  EXPECT_EQ(in[0].lat_e6, out[0].lat_e6);
  EXPECT_EQ(in[0].lon_e6, out[0].lon_e6);
}

TEST(PositionReport, RoundTripsAFullTrackExactly) {
  std::vector<PositionSample> in;
  uint32_t t = 1750000000;
  int32_t lat = 51500000, lon = -120000;
  for (int i = 0; i < 20; i++) {
    in.push_back(mk(t, lat, lon));
    t += 60; lat += 900; lon -= 450;      // ~100m per step
  }

  uint8_t buf[BUDGET];
  int consumed = 0;
  int n = PositionReport::encode(buf, sizeof(buf), PREFIX, in.data(), (int)in.size(), &consumed);
  ASSERT_GT(n, 0);
  ASSERT_EQ((int)in.size(), consumed) << "whole track should have fit";

  uint8_t prefix[POS_PREFIX_LEN];
  PositionSample out[64];
  int got = PositionReport::decode(buf, n, prefix, out, 64);
  ASSERT_EQ((int)in.size(), got);
  for (size_t i = 0; i < in.size(); i++) {
    EXPECT_EQ(in[i].timestamp, out[i].timestamp) << "sample " << i;
    EXPECT_EQ(in[i].lat_e6, out[i].lat_e6) << "sample " << i;
    EXPECT_EQ(in[i].lon_e6, out[i].lon_e6) << "sample " << i;
  }
}

TEST(PositionReport, DeltaEncodingIsWorthIt) {
  // 20 samples in one packet is the entire reason for moving off adverts, where the
  // app_data budget is 32 bytes and holds one position at best.
  std::vector<PositionSample> in;
  uint32_t t = 1750000000;
  for (int i = 0; i < 20; i++) { in.push_back(mk(t, 51500000 + i * 900, -120000)); t += 60; }

  uint8_t buf[BUDGET];
  int consumed = 0;
  int n = PositionReport::encode(buf, sizeof(buf), PREFIX, in.data(), 20, &consumed);
  EXPECT_EQ(20, consumed);
  EXPECT_LE(n, (int)BUDGET);
  EXPECT_LT(n / 20, 12) << "should beat 12 bytes/sample uncompressed";
}

// ---------------------------------------------------------------- batching

TEST(PositionReport, OverlongBacklogIsSplitAcrossReports) {
  std::vector<PositionSample> in;
  uint32_t t = 1750000000;
  for (int i = 0; i < 100; i++) { in.push_back(mk(t, 51500000 + i * 500, -120000)); t += 60; }

  // drain it the way flushTrackReport() does, and check nothing is lost or duplicated
  std::vector<PositionSample> recovered;
  size_t pos = 0;
  int reports = 0;
  while (pos < in.size()) {
    uint8_t buf[BUDGET];
    int consumed = 0;
    int n = PositionReport::encode(buf, sizeof(buf), PREFIX, &in[pos], (int)(in.size() - pos), &consumed);
    ASSERT_GT(n, 0);
    ASSERT_GT(consumed, 0) << "must always make progress, or the backlog would stall";

    uint8_t prefix[POS_PREFIX_LEN];
    PositionSample out[64];
    int got = PositionReport::decode(buf, n, prefix, out, 64);
    ASSERT_EQ(consumed, got);
    for (int k = 0; k < got; k++) recovered.push_back(out[k]);

    pos += consumed;
    reports++;
  }

  EXPECT_GT(reports, 1);
  ASSERT_EQ(in.size(), recovered.size());
  for (size_t i = 0; i < in.size(); i++) {
    EXPECT_EQ(in[i].timestamp, recovered[i].timestamp) << "sample " << i;
    EXPECT_EQ(in[i].lat_e6, recovered[i].lat_e6) << "sample " << i;
  }
}

TEST(PositionReport, ATooLargeJumpStartsANewReport) {
  // Beyond ~3.6km a delta no longer fits, so the encoder must stop and let the next
  // report re-base rather than silently truncating the value.
  PositionSample in[] = {
    mk(1750000000, 51500000, -120000),
    mk(1750000060, 51500900, -120000),
    mk(1750000120, 60000000, -120000),   // hundreds of km away
    mk(1750000180, 60000900, -120000),
  };

  uint8_t buf[BUDGET];
  int consumed = 0;
  int n = PositionReport::encode(buf, sizeof(buf), PREFIX, in, 4, &consumed);
  ASSERT_GT(n, 0);
  EXPECT_EQ(2, consumed) << "should stop at the unrepresentable jump";

  // the remainder encodes cleanly on its own
  int consumed2 = 0;
  int n2 = PositionReport::encode(buf, sizeof(buf), PREFIX, &in[2], 2, &consumed2);
  ASSERT_GT(n2, 0);
  EXPECT_EQ(2, consumed2);
}

TEST(PositionReport, ALongTimeGapAlsoRebases) {
  PositionSample in[] = {
    mk(1750000000, 51500000, -120000),
    mk(1750000000 + 200000, 51500900, -120000),   // ~2 days later
  };
  uint8_t buf[BUDGET];
  int consumed = 0;
  ASSERT_GT(PositionReport::encode(buf, sizeof(buf), PREFIX, in, 2, &consumed), 0);
  EXPECT_EQ(1, consumed);
}

// ---------------------------------------------------------------- robustness

TEST(PositionReport, TruncatedOrCorruptInputIsRejected) {
  std::vector<PositionSample> in;
  uint32_t t = 1750000000;
  for (int i = 0; i < 10; i++) { in.push_back(mk(t, 51500000 + i * 900, -120000)); t += 60; }

  uint8_t buf[BUDGET];
  int consumed = 0;
  int n = PositionReport::encode(buf, sizeof(buf), PREFIX, in.data(), 10, &consumed);
  ASSERT_GT(n, 0);

  uint8_t prefix[POS_PREFIX_LEN];
  PositionSample out[64];

  for (int cut = 0; cut < n; cut++) {
    // every truncation must be refused rather than yielding a partial track
    if (cut >= POS_HEADER_LEN + 10 * POS_DELTA_LEN) continue;
    EXPECT_EQ(0, PositionReport::decode(buf, cut, prefix, out, 64)) << "accepted a " << cut << "-byte report";
  }

  uint8_t bad[BUDGET];
  memcpy(bad, buf, n);
  bad[0] = 0xFF;   // unknown version
  EXPECT_EQ(0, PositionReport::decode(bad, n, prefix, out, 64));
}

TEST(PositionReport, HonoursACallersSampleLimit) {
  std::vector<PositionSample> in;
  uint32_t t = 1750000000;
  for (int i = 0; i < 15; i++) { in.push_back(mk(t, 51500000 + i * 900, -120000)); t += 60; }

  uint8_t buf[BUDGET];
  int consumed = 0;
  int n = PositionReport::encode(buf, sizeof(buf), PREFIX, in.data(), 15, &consumed);
  ASSERT_GT(n, 0);

  uint8_t prefix[POS_PREFIX_LEN];
  PositionSample out[4];
  EXPECT_EQ(4, PositionReport::decode(buf, n, prefix, out, 4));   // no overrun
  EXPECT_EQ(in[0].lat_e6, out[0].lat_e6);
}

TEST(PositionReport, TinyBufferIsRefusedNotOverrun) {
  PositionSample in[] = { mk(1750000000, 51500000, -120000) };
  uint8_t small[POS_HEADER_LEN - 1];
  int consumed = 99;
  EXPECT_EQ(0, PositionReport::encode(small, sizeof(small), PREFIX, in, 1, &consumed));
  EXPECT_EQ(0, consumed);
}

TEST(PositionReport, SouthernAndWesternCoordinatesSurvive) {
  PositionSample in[] = {
    mk(1750000000, -33868800, 151209300),   // Sydney
    mk(1750000060, -33869700, 151208400),
  };
  uint8_t buf[BUDGET];
  int consumed = 0;
  int n = PositionReport::encode(buf, sizeof(buf), PREFIX, in, 2, &consumed);
  ASSERT_GT(n, 0);
  ASSERT_EQ(2, consumed);

  uint8_t prefix[POS_PREFIX_LEN];
  PositionSample out[4];
  ASSERT_EQ(2, PositionReport::decode(buf, n, prefix, out, 4));
  EXPECT_EQ(in[1].lat_e6, out[1].lat_e6);
  EXPECT_EQ(in[1].lon_e6, out[1].lon_e6);
}

TEST(PositionReport, CapacityMatchesWhatEncodeActuallyPacks) {
  std::vector<PositionSample> in;
  uint32_t t = 1750000000;
  for (int i = 0; i < 200; i++) { in.push_back(mk(t, 51500000 + i * 100, -120000)); t += 60; }

  uint8_t buf[BUDGET];
  int consumed = 0;
  int n = PositionReport::encode(buf, sizeof(buf), PREFIX, in.data(), (int)in.size(), &consumed);
  ASSERT_GT(n, 0);
  EXPECT_LE(n, (int)BUDGET);
  EXPECT_EQ(PositionReport::capacityFor(BUDGET), consumed);
}

TEST(PositionReport, WhiteningIsItsOwnInverse) {
  uint8_t secret[POS_SECRET_LEN];
  for (int i = 0; i < POS_SECRET_LEN; i++) secret[i] = (uint8_t)(i * 7 + 1);

  uint8_t body[64], original[64];
  for (int i = 0; i < 64; i++) body[i] = original[i] = (uint8_t)(i * 3);

  uint8_t nonce[POS_NONCE_LEN];
  PositionReport::deriveNonce(secret, body, sizeof(body), nonce);

  PositionReport::whiten(secret, nonce, body, sizeof(body));
  EXPECT_NE(0, memcmp(body, original, sizeof(body))) << "whitening changed nothing";

  PositionReport::whiten(secret, nonce, body, sizeof(body));
  EXPECT_EQ(0, memcmp(body, original, sizeof(body)));
}

TEST(PositionReport, WhiteningSpansEveryBlockNotJustTheFirst) {
  // A nonce that only sat in the header would leave the later blocks untouched, which is
  // the failure this replaces - so check the tail changes as well as the head.
  uint8_t secret[POS_SECRET_LEN] = {0};
  uint8_t a[80] = {0}, b[80] = {0};
  b[2] = 1;   // one bit of difference, early on

  uint8_t na[POS_NONCE_LEN], nb[POS_NONCE_LEN];
  PositionReport::deriveNonce(secret, a, sizeof(a), na);
  PositionReport::deriveNonce(secret, b, sizeof(b), nb);
  EXPECT_NE(0, memcmp(na, nb, POS_NONCE_LEN)) << "nonce did not follow the plaintext";

  PositionReport::whiten(secret, na, a, sizeof(a));
  PositionReport::whiten(secret, nb, b, sizeof(b));

  for (size_t off = 0; off + 16 <= sizeof(a); off += 16) {
    EXPECT_NE(0, memcmp(a + off, b + off, 16)) << "block at " << off << " is unchanged";
  }
}

TEST(PositionReport, ADifferentKeyGivesADifferentKeystream) {
  uint8_t s1[POS_SECRET_LEN] = {0}, s2[POS_SECRET_LEN] = {0};
  s2[0] = 1;
  uint8_t nonce[POS_NONCE_LEN] = {0};
  uint8_t a[48], b[48];
  memset(a, 0xAA, sizeof(a)); memset(b, 0xAA, sizeof(b));

  PositionReport::whiten(s1, nonce, a, sizeof(a));
  PositionReport::whiten(s2, nonce, b, sizeof(b));
  EXPECT_NE(0, memcmp(a, b, sizeof(a)));
}

int main(int argc, char **argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
