#include <gtest/gtest.h>
#include <helpers/ChannelConfig.h>
#include <string.h>

// the two keys used throughout, as the build would emit them and as they should come out
static const char* HEX_A = "000102030405060708090a0b0c0d0e0f";
static const uint8_t RAW_A[16] = { 0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15 };

static const char* HEX_B = "8b3387e9c5cdea6ac9e5edbaa115cd72";   // the Public channel PSK
static const uint8_t RAW_B[16] = { 0x8b,0x33,0x87,0xe9,0xc5,0xcd,0xea,0x6a,
                                   0xc9,0xe5,0xed,0xba,0xa1,0x15,0xcd,0x72 };

// ------------------------------------------------------------------ what the build emits

TEST(ChannelConfig, ParsesASingleChannel) {
  ChannelConfigEntry got[4];
  char spec[128];
  snprintf(spec, sizeof(spec), "ops:%s", HEX_A);

  ASSERT_EQ(1, ChannelConfig::parse(spec, got, 4));
  EXPECT_STREQ("ops", got[0].name);
  EXPECT_EQ(0, memcmp(RAW_A, got[0].secret, 16));
}

TEST(ChannelConfig, KeepsTheOrderTheNamesWereGivenIn) {
  ChannelConfigEntry got[4];
  char spec[256];
  snprintf(spec, sizeof(spec), "ops:%s,sar:%s", HEX_A, HEX_B);

  ASSERT_EQ(2, ChannelConfig::parse(spec, got, 4));
  EXPECT_STREQ("ops", got[0].name);
  EXPECT_EQ(0, memcmp(RAW_A, got[0].secret, 16));
  EXPECT_STREQ("sar", got[1].name);
  EXPECT_EQ(0, memcmp(RAW_B, got[1].secret, 16));
}

TEST(ChannelConfig, AcceptsUppercaseHex) {
  ChannelConfigEntry got[2];
  ASSERT_EQ(1, ChannelConfig::parse("ops:000102030405060708090A0B0C0D0E0F", got, 2));
  EXPECT_EQ(0, memcmp(RAW_A, got[0].secret, 16));
}

TEST(ChannelConfig, AcceptsTheLongestNameThatFits) {
  char name[CHANNEL_CONFIG_NAME_LEN];       // 31 bytes plus the terminator
  memset(name, 'x', sizeof(name) - 1);
  name[sizeof(name) - 1] = 0;

  char spec[128];
  snprintf(spec, sizeof(spec), "%s:%s", name, HEX_A);

  ChannelConfigEntry got[2];
  ASSERT_EQ(1, ChannelConfig::parse(spec, got, 2));
  EXPECT_STREQ(name, got[0].name);
}

TEST(ChannelConfig, NamesAreNullTerminatedNotJustCopied) {
  ChannelConfigEntry got[2];
  memset(got, 0xAA, sizeof(got));            // caller's buffer is not assumed clean

  ASSERT_EQ(1, ChannelConfig::parse("ops:000102030405060708090a0b0c0d0e0f", got, 2));
  EXPECT_EQ(3u, strlen(got[0].name));
  for (size_t i = strlen(got[0].name); i < sizeof(got[0].name); i++) {
    EXPECT_EQ(0, got[0].name[i]) << "name not zero-filled at " << i;
  }
}

// ------------------------------------------------------------------------- malformed
//
// All of these mean the generator is wrong rather than one channel being off, so the
// whole string is rejected - half a channel list on a device looks right and isn't.

TEST(ChannelConfig, RejectsMalformedSpecs) {
  ChannelConfigEntry got[4];

  // an empty string is no channels rather than a malformed one - the build leaves the
  // flag out entirely when there are none, so this is only reachable by hand
  EXPECT_EQ(0, ChannelConfig::parse("", got, 4)) << "empty";

  EXPECT_EQ(-1, ChannelConfig::parse("ops", got, 4)) << "no key";
  EXPECT_EQ(-1, ChannelConfig::parse("ops:", got, 4)) << "empty key";
  EXPECT_EQ(-1, ChannelConfig::parse(":000102030405060708090a0b0c0d0e0f", got, 4)) << "no name";
  EXPECT_EQ(-1, ChannelConfig::parse("ops:000102030405060708090a0b0c0d0e", got, 4)) << "short key";
  EXPECT_EQ(-1, ChannelConfig::parse("ops:000102030405060708090a0b0c0d0e0f00", got, 4)) << "long key";
  EXPECT_EQ(-1, ChannelConfig::parse("ops:g00102030405060708090a0b0c0d0e0f", got, 4)) << "not hex";
  EXPECT_EQ(-1, ChannelConfig::parse("ops:000102030405060708090a0b0c0d0e0f,", got, 4)) << "trailing comma";
  EXPECT_EQ(-1, ChannelConfig::parse("ops:000102030405060708090a0b0c0d0e0f,sar", got, 4)) << "second has no key";
  EXPECT_EQ(-1, ChannelConfig::parse(NULL, got, 4)) << "null spec";
  EXPECT_EQ(-1, ChannelConfig::parse("ops:000102030405060708090a0b0c0d0e0f", got, 0)) << "no capacity";
}

TEST(ChannelConfig, RejectsANameTooLongForTheField) {
  char name[CHANNEL_CONFIG_NAME_LEN + 1];    // 32 bytes: one past what the field holds
  memset(name, 'x', sizeof(name) - 1);
  name[sizeof(name) - 1] = 0;

  char spec[128];
  snprintf(spec, sizeof(spec), "%s:%s", name, HEX_A);

  ChannelConfigEntry got[2];
  EXPECT_EQ(-1, ChannelConfig::parse(spec, got, 2));
}

// ---------------------------------------------------------------------------- capacity

TEST(ChannelConfig, StopsWhenTheDestinationIsFull) {
  char spec[256];
  snprintf(spec, sizeof(spec), "a:%s,b:%s,c:%s", HEX_A, HEX_B, HEX_A);

  ChannelConfigEntry got[2];
  ASSERT_EQ(2, ChannelConfig::parse(spec, got, 2));
  EXPECT_STREQ("a", got[0].name);
  EXPECT_STREQ("b", got[1].name);
}

int main(int argc, char **argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
