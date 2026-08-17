#include <gtest/gtest.h>
#include <Mesh.h>
#include <helpers/StaticPoolPacketManager.h>
#include <helpers/PositionReport.h>
#include <vector>
#include <string.h>

// Exercises the advert and position-report paths as Mesh actually runs them: build a
// packet, serialise it the way the radio would, parse it back on another node, and check
// what comes out the far end. The Dispatcher send/recv loop is bypassed -
// writeTo()/readFrom() is exactly the transformation the air applies.

// ------------------------------------------------------------------ test doubles

class StubRadio : public mesh::Radio {
public:
  int recvRaw(uint8_t*, int) override { return 0; }
  uint32_t getEstAirtimeFor(int len) override { return len * 8; }
  float packetScore(float, int) override { return 0.5f; }
  bool startSendRaw(const uint8_t*, int) override { return true; }
  bool isSendComplete() override { return true; }
  void onSendFinished() override { }
  bool isInRecvMode() const override { return true; }
};

class StubMillis : public mesh::MillisecondClock {
public:
  unsigned long now = 0;
  unsigned long getMillis() override { return now; }
};

class StubRTC : public mesh::RTCClock {
public:
  uint32_t t = 1750000000;
  uint32_t getCurrentTime() override { return t; }
  void setCurrentTime(uint32_t v) override { t = v; }
};

class StubTables : public mesh::MeshTables {
  std::vector<uint64_t> _seen;
  static uint64_t keyOf(const mesh::Packet* p) {
    uint8_t h[MAX_HASH_SIZE];
    p->calculatePacketHash(h);
    uint64_t k = 0;
    for (int i = 0; i < MAX_HASH_SIZE; i++) k = (k << 8) | h[i];
    return k;
  }
public:
  bool wasSeen(const mesh::Packet* p) override {
    uint64_t k = keyOf(p);
    for (uint64_t s : _seen) if (s == k) return true;
    return false;
  }
  void markSeen(const mesh::Packet* p) override { _seen.push_back(keyOf(p)); }
  void clear(const mesh::Packet* p) override {
    uint64_t k = keyOf(p);
    for (size_t i = 0; i < _seen.size(); i++) {
      if (_seen[i] == k) { _seen.erase(_seen.begin() + i); return; }
    }
  }
};

class TestRNG : public mesh::RNG {
public:
  uint32_t s = 0x12345678;
  void random(uint8_t* dest, size_t sz) override {
    for (size_t i = 0; i < sz; i++) {
      s ^= s << 13; s ^= s >> 17; s ^= s << 5;
      dest[i] = (uint8_t)s;
    }
  }
};

class TestMesh : public mesh::Mesh {
public:
  struct Received {
    uint8_t pub_key[PUB_KEY_SIZE];
    uint32_t timestamp;
    std::vector<uint8_t> app_data;
  };
  std::vector<Received> received;

  TestMesh(mesh::Radio& r, mesh::MillisecondClock& ms, mesh::RNG& rng,
           mesh::RTCClock& rtc, mesh::PacketManager& mgr, mesh::MeshTables& tab)
    : mesh::Mesh(r, ms, rng, rtc, mgr, tab) { }

  void onAdvertRecv(mesh::Packet* pkt, const mesh::Identity& id, uint32_t timestamp,
                    const uint8_t* app_data, size_t app_data_len) override {
    Received r;
    memcpy(r.pub_key, id.pub_key, PUB_KEY_SIZE);
    r.timestamp = timestamp;
    r.app_data.assign(app_data, app_data + app_data_len);
    received.push_back(r);
  }

  using mesh::Mesh::onRecvPacket;

  // --- position tracking ---
  mesh::GroupChannel track_channel;
  bool has_track_key = false;
  std::vector<std::vector<uint8_t>> track_blobs;   // decrypted GRP_DATA payloads

  void useTrackKey(const char* key) {
    mesh::Utils::sha256(track_channel.secret, sizeof(track_channel.secret),
                        (const uint8_t*)key, strlen(key));
    mesh::Utils::sha256(track_channel.hash, sizeof(track_channel.hash), track_channel.secret, 32);
    has_track_key = true;
  }

  int searchChannelsByHash(const uint8_t* hash, mesh::GroupChannel dest[], int max_matches) override {
    if (has_track_key && max_matches > 0 && track_channel.hash[0] == hash[0]) {
      dest[0] = track_channel;
      return 1;
    }
    return 0;
  }

  void onGroupDataRecv(mesh::Packet* pkt, uint8_t type, const mesh::GroupChannel& ch,
                       uint8_t* data, size_t len) override {
    track_blobs.push_back(std::vector<uint8_t>(data, data + len));
  }

  bool allowPacketForward(const mesh::Packet* pkt) override { return true; }
};

// One node's worth of plumbing.
struct Node {
  StubRadio radio;
  StubMillis ms;
  StubRTC rtc;
  TestRNG rng;
  StubTables tables;
  StaticPoolPacketManager mgr;
  TestMesh mesh;

  // NOTE: the seed must differ per node, or both ends generate the same keypair and the
  // receiver discards every advert as its own.
  explicit Node(uint32_t seed) : mgr(16), mesh(radio, ms, rng, rtc, mgr, tables) {
    rng.s = seed;
    mesh.self_id = mesh::LocalIdentity(&rng);
  }
};

// Move a packet from one node to another exactly as the radio would: flatten to bytes,
// then rebuild. Returns the wire bytes so tests can inspect what actually travelled.
static std::vector<uint8_t> transmit(mesh::Packet* pkt, TestMesh& to) {
  uint8_t raw[MAX_TRANS_UNIT];
  pkt->header &= ~PH_ROUTE_MASK;
  pkt->header |= ROUTE_TYPE_FLOOD;
  uint8_t len = pkt->writeTo(raw);

  mesh::Packet in;
  EXPECT_TRUE(in.readFrom(raw, len));
  to.onRecvPacket(&in);

  return std::vector<uint8_t>(raw, raw + len);
}

static size_t buildAppData(uint8_t* out, const char* name) {
  out[0] = 0x01;                       // type/flags
  size_t n = strlen(name);
  memcpy(&out[1], name, n);
  return n + 1;
}

// ------------------------------------------------------------------ tests

TEST(AdvertMesh, AnAdvertRoundTripsBetweenNodes) {
  Node tx(0xAAAA1111), rx(0xBBBB2222);
  uint8_t app[MAX_ADVERT_DATA_SIZE];
  size_t app_len = buildAppData(app, "plain");

  mesh::Packet* pkt = tx.mesh.createAdvert(tx.mesh.self_id, app, app_len);
  ASSERT_NE(nullptr, pkt);
  transmit(pkt, rx.mesh);

  ASSERT_EQ(1u, rx.mesh.received.size());
  EXPECT_EQ(0, memcmp(tx.mesh.self_id.pub_key, rx.mesh.received[0].pub_key, PUB_KEY_SIZE));
}

TEST(AdvertMesh, BackToBackAdvertsGetDistinctTimestamps) {
  // Same wall-clock second: without getCurrentTimeUnique() both would carry the same
  // timestamp and a receiver would discard the second as a replay.
  Node tx(0xAAAA1111);
  uint8_t app[MAX_ADVERT_DATA_SIZE];
  size_t app_len = buildAppData(app, "n");

  mesh::Packet* a = tx.mesh.createAdvert(tx.mesh.self_id, app, app_len);
  mesh::Packet* b = tx.mesh.createAdvert(tx.mesh.self_id, app, app_len);
  ASSERT_NE(nullptr, a);
  ASSERT_NE(nullptr, b);

  uint32_t ts_a, ts_b;
  memcpy(&ts_a, &a->payload[PUB_KEY_SIZE], 4);
  memcpy(&ts_b, &b->payload[PUB_KEY_SIZE], 4);
  EXPECT_NE(ts_a, ts_b);
}

TEST(AdvertMesh, RepeatedAdvertsAreNotDeduplicatedAgainstEachOther) {
  // Each advert carries its own timestamp, so two adverts of the same content must
  // remain distinct packets - otherwise the dedup tables would suppress the second
  // everywhere.
  Node tx(0xAAAA1111), rx(0xBBBB2222);

  uint8_t app[MAX_ADVERT_DATA_SIZE];
  size_t app_len = buildAppData(app, "beacon");

  for (int i = 0; i < 3; i++) {
    mesh::Packet* pkt = tx.mesh.createAdvert(tx.mesh.self_id, app, app_len);
    ASSERT_NE(nullptr, pkt) << "advert " << i;
    transmit(pkt, rx.mesh);
  }
  EXPECT_EQ(3u, rx.mesh.received.size());
}

TEST(AdvertMesh, ARepeatedPacketIsSuppressedAsBefore) {
  Node tx(0xAAAA1111), rx(0xBBBB2222);

  uint8_t app[MAX_ADVERT_DATA_SIZE];
  size_t app_len = buildAppData(app, "beacon");

  mesh::Packet* pkt = tx.mesh.createAdvert(tx.mesh.self_id, app, app_len);
  ASSERT_NE(nullptr, pkt);

  uint8_t raw[MAX_TRANS_UNIT];
  pkt->header &= ~PH_ROUTE_MASK;
  pkt->header |= ROUTE_TYPE_FLOOD;
  uint8_t len = pkt->writeTo(raw);

  for (int i = 0; i < 3; i++) {   // same bytes arriving by three different routes
    mesh::Packet in;
    ASSERT_TRUE(in.readFrom(raw, len));
    rx.mesh.onRecvPacket(&in);
  }
  EXPECT_EQ(1u, rx.mesh.received.size());
}

// ------------------------------------------------------------------ position reports

static const char* TRACK_KEY = "a-different-key-from-the-advert-one";

// Build the GRP_DATA blob exactly as flushTrackReport() does, whitening included.
static int buildTrackBlob(uint8_t* blob, size_t cap, const uint8_t* secret, const uint8_t* pubkey,
                          const PositionSample* samples, int n, int* consumed) {
  int i = 0;
  blob[i++] = (uint8_t)(POSITION_REPORT_DATA_TYPE & 0xFF);
  blob[i++] = (uint8_t)(POSITION_REPORT_DATA_TYPE >> 8);
  int len_pos = i++;
  uint8_t* nonce = &blob[i]; i += POS_NONCE_LEN;
  int written = PositionReport::encode(&blob[i], cap - i, pubkey, samples, n, consumed);
  if (written <= 0) return 0;
  blob[len_pos] = (uint8_t)(POS_NONCE_LEN + written);
  PositionReport::deriveNonce(secret, &blob[i], written, nonce);
  PositionReport::whiten(secret, nonce, &blob[i], written);
  return i + written;
}

// Undo it the way the receive path does - including BaseChatMesh's framing, which hands
// on exactly blob[2] bytes from blob[3]. Reading that length rather than "the rest of the
// packet" is the point: a length byte that forgot the nonce truncates every report, and a
// harness that helps itself to the whole buffer never notices.
static int readTrackBlob(const std::vector<uint8_t>& got, uint8_t prefix[POS_PREFIX_LEN],
                         const uint8_t* secret, PositionSample* out, int max_samples) {
  if (got.size() < (size_t)3) return 0;
  size_t data_len = got[2];
  if (data_len > got.size() - 3) return 0;      // malformed framing
  if (data_len <= POS_NONCE_LEN) return 0;

  std::vector<uint8_t> plain(got.begin() + 3 + POS_NONCE_LEN, got.begin() + 3 + data_len);
  PositionReport::whiten(secret, &got[3], plain.data(), plain.size());
  return PositionReport::decode(plain.data(), plain.size(), prefix, out, max_samples);
}

TEST(PositionTracking, ATrackRoundTripsBetweenNodesSharingTheTrackingKey) {
  Node tx(0xAAAA1111), rx(0xBBBB2222);
  tx.mesh.useTrackKey(TRACK_KEY);
  rx.mesh.useTrackKey(TRACK_KEY);

  PositionSample samples[8];
  uint32_t t = 1750000000;
  for (int i = 0; i < 8; i++) {
    samples[i].timestamp = t + i * 60;
    samples[i].lat_e6 = 51500000 + i * 900;
    samples[i].lon_e6 = -120000;
  }

  uint8_t blob[MAX_GROUP_DATA_LENGTH];
  int consumed = 0;
  int blob_len = buildTrackBlob(blob, sizeof(blob), tx.mesh.track_channel.secret, tx.mesh.self_id.pub_key, samples, 8, &consumed);
  ASSERT_GT(blob_len, 0);
  ASSERT_EQ(8, consumed);

  mesh::Packet* pkt = tx.mesh.createGroupDatagram(PAYLOAD_TYPE_GRP_DATA, tx.mesh.track_channel,
                                                  blob, blob_len);
  ASSERT_NE(nullptr, pkt);
  transmit(pkt, rx.mesh);

  ASSERT_EQ(1u, rx.mesh.track_blobs.size());
  auto& got = rx.mesh.track_blobs[0];
  ASSERT_GE(got.size(), (size_t)3);
  EXPECT_EQ(POSITION_REPORT_DATA_TYPE, (uint16_t)(got[0] | (got[1] << 8)));

  uint8_t prefix[POS_PREFIX_LEN];
  PositionSample out[16];
  int n = readTrackBlob(got, prefix, rx.mesh.track_channel.secret, out, 16);
  ASSERT_EQ(8, n);
  EXPECT_EQ(0, memcmp(prefix, tx.mesh.self_id.pub_key, POS_PREFIX_LEN));
  EXPECT_EQ(samples[7].lat_e6, out[7].lat_e6);   // newest position survived intact
}

TEST(PositionTracking, ARelayWithoutTheTrackingKeyStillForwardsIt) {
  // The reason for putting tracking on group datagrams rather than adverts: Mesh calls
  // routeRecvPacket() outside the decrypt loop, so a repeater relays position reports
  // it cannot read. That lets the tracking key stay off every repeater in the network.
  Node tx(0xAAAA1111), relay(0xCCCC3333);
  tx.mesh.useTrackKey(TRACK_KEY);
  // relay deliberately has NO tracking key

  PositionSample s = { 1750000000, 51500000, -120000 };
  uint8_t blob[MAX_GROUP_DATA_LENGTH];
  int consumed = 0;
  int blob_len = buildTrackBlob(blob, sizeof(blob), tx.mesh.track_channel.secret, tx.mesh.self_id.pub_key, &s, 1, &consumed);
  ASSERT_GT(blob_len, 0);

  mesh::Packet* pkt = tx.mesh.createGroupDatagram(PAYLOAD_TYPE_GRP_DATA, tx.mesh.track_channel,
                                                  blob, blob_len);
  ASSERT_NE(nullptr, pkt);

  uint8_t raw[MAX_TRANS_UNIT];
  pkt->header &= ~PH_ROUTE_MASK;
  pkt->header |= ROUTE_TYPE_FLOOD;
  uint8_t len = pkt->writeTo(raw);

  mesh::Packet in;
  ASSERT_TRUE(in.readFrom(raw, len));
  mesh::DispatcherAction action = relay.mesh.onRecvPacket(&in);

  EXPECT_TRUE(relay.mesh.track_blobs.empty()) << "relay must not be able to read the track";
  EXPECT_NE(ACTION_RELEASE, action) << "relay dropped a report it should have forwarded";
}

TEST(PositionTracking, TheWrongTrackingKeyRevealsNothing) {
  Node tx(0xAAAA1111), rx(0xBBBB2222);
  tx.mesh.useTrackKey(TRACK_KEY);
  rx.mesh.useTrackKey("some-other-network");

  PositionSample s = { 1750000000, 51500000, -120000 };
  uint8_t blob[MAX_GROUP_DATA_LENGTH];
  int consumed = 0;
  int blob_len = buildTrackBlob(blob, sizeof(blob), tx.mesh.track_channel.secret, tx.mesh.self_id.pub_key, &s, 1, &consumed);

  mesh::Packet* pkt = tx.mesh.createGroupDatagram(PAYLOAD_TYPE_GRP_DATA, tx.mesh.track_channel,
                                                  blob, blob_len);
  ASSERT_NE(nullptr, pkt);
  transmit(pkt, rx.mesh);

  EXPECT_TRUE(rx.mesh.track_blobs.empty());
}

TEST(PositionTracking, NoCoordinateAppearsOnTheWire) {
  Node tx(0xAAAA1111), rx(0xBBBB2222);
  tx.mesh.useTrackKey(TRACK_KEY);

  PositionSample s = { 1750000000, 51500000, -120000 };
  uint8_t blob[MAX_GROUP_DATA_LENGTH];
  int consumed = 0;
  int blob_len = buildTrackBlob(blob, sizeof(blob), tx.mesh.track_channel.secret, tx.mesh.self_id.pub_key, &s, 1, &consumed);

  mesh::Packet* pkt = tx.mesh.createGroupDatagram(PAYLOAD_TYPE_GRP_DATA, tx.mesh.track_channel,
                                                  blob, blob_len);
  ASSERT_NE(nullptr, pkt);

  uint8_t raw[MAX_TRANS_UNIT];
  uint8_t len = pkt->writeTo(raw);

  // neither the coordinates nor the reporter's key prefix may be readable
  for (int i = 0; i + 4 <= len; i++) {
    EXPECT_NE(0, memcmp(&raw[i], &s.lat_e6, 4)) << "latitude in the clear at " << i;
    EXPECT_NE(0, memcmp(&raw[i], &s.lon_e6, 4)) << "longitude in the clear at " << i;
  }
  for (int i = 0; i + POS_PREFIX_LEN <= len; i++) {
    EXPECT_NE(0, memcmp(&raw[i], tx.mesh.self_id.pub_key, POS_PREFIX_LEN))
        << "reporter identity in the clear at " << i;
  }
}

// The regression this whole nonce exists for. Group datagrams are AES-ECB, so identical
// plaintext blocks give identical ciphertext blocks. A parked node samples at a fixed
// interval and never moves, so its report is a long run of identical (dt, 0, 0) deltas -
// a repeating 6-byte pattern, which lines up with the 16-byte blocks every 48 bytes.
// Without whitening those blocks recur verbatim inside one packet and across successive
// packets, and an observer with no key can read "not moving" straight off the wire. That
// is exactly what the fixed transmit cadence is there to hide.
TEST(PositionTracking, AParkedNodeRepeatsNoCiphertextBlock) {
  Node tx(0xAAAA1111);
  tx.mesh.useTrackKey(TRACK_KEY);

  const int kSamples = PositionReport::capacityFor(MAX_GROUP_DATA_LENGTH - 3 - POS_NONCE_LEN);
  ASSERT_GE(kSamples, 12) << "need a long enough delta run for blocks to recur";

  std::vector<std::vector<uint8_t>> blocks;
  for (int report = 0; report < 3; report++) {
    std::vector<PositionSample> samples(kSamples);
    for (int i = 0; i < kSamples; i++) {
      // parked: same coordinates throughout, sampled on a fixed interval
      samples[i].timestamp = 1750000000 + (uint32_t)report * 36000 + (uint32_t)i * 3600;
      samples[i].lat_e6 = 51500000;
      samples[i].lon_e6 = -120000;
    }

    uint8_t blob[MAX_GROUP_DATA_LENGTH];
    int consumed = 0;
    int blob_len = buildTrackBlob(blob, sizeof(blob), tx.mesh.track_channel.secret,
                                  tx.mesh.self_id.pub_key, samples.data(), kSamples, &consumed);
    ASSERT_GT(blob_len, 0);
    ASSERT_EQ(kSamples, consumed);

    mesh::Packet* pkt = tx.mesh.createGroupDatagram(PAYLOAD_TYPE_GRP_DATA, tx.mesh.track_channel,
                                                    blob, blob_len);
    ASSERT_NE(nullptr, pkt);
    uint8_t raw[MAX_TRANS_UNIT];
    uint8_t len = pkt->writeTo(raw);
    tx.mesh.releasePacket(pkt);

    // The channel hash and MAC at the head of the payload are not ciphertext, and the
    // packet header ahead of them is not either - walk back from the end instead, which
    // is all ciphertext and block-aligned.
    for (int off = (int)len - CIPHER_BLOCK_SIZE; off >= (int)(len % CIPHER_BLOCK_SIZE) + CIPHER_BLOCK_SIZE;
         off -= CIPHER_BLOCK_SIZE) {
      blocks.push_back(std::vector<uint8_t>(raw + off, raw + off + CIPHER_BLOCK_SIZE));
    }
  }
  ASSERT_GE(blocks.size(), (size_t)18) << "expected several blocks per report to compare";

  for (size_t a = 0; a < blocks.size(); a++) {
    for (size_t b = a + 1; b < blocks.size(); b++) {
      EXPECT_NE(blocks[a], blocks[b]) << "cipher blocks " << a << " and " << b << " match: a "
                                      << "parked node is identifiable without the key";
    }
  }
}

TEST(PositionTracking, TheLengthByteCoversTheNonce) {
  // BaseChatMesh hands on exactly blob[2] bytes. A length byte counting only the report
  // truncates every one of them by the width of the nonce, and the feature is silently
  // dead on real hardware while every test that skips the framing still passes.
  Node tx(0xAAAA1111);
  tx.mesh.useTrackKey(TRACK_KEY);

  PositionSample s = { 1750000000, 51500000, -120000 };
  uint8_t blob[MAX_GROUP_DATA_LENGTH];
  int consumed = 0;
  int blob_len = buildTrackBlob(blob, sizeof(blob), tx.mesh.track_channel.secret,
                                tx.mesh.self_id.pub_key, &s, 1, &consumed);
  ASSERT_GT(blob_len, 0);
  EXPECT_EQ(blob_len - 3, (int)blob[2]) << "length byte does not cover the whole payload";
}

int main(int argc, char **argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
