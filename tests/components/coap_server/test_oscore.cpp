// Tests for OSCORE (RFC 8613): key derivation, nonce and AAD, protect/unprotect, the persisted sender sequence
// number, and protected requests through the network transport. Key material is from RFC 8613 Appendix C.1.1.

#include "common.h"

#ifdef USE_COAP_OSCORE

namespace esphome::coap_server::testing {
namespace {

const uint8_t MASTER_SECRET[] = {0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08,
                                 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f, 0x10};
const uint8_t MASTER_SALT[] = {0x9e, 0x7c, 0xa9, 0x22, 0x23, 0x78, 0x63, 0x40};
// Sender key for ID h'' and for ID h'01'
const uint8_t KEY_ID_EMPTY[] = {0xf0, 0x91, 0x0e, 0xd7, 0x29, 0x5e, 0x6a, 0xd4,
                                0xb5, 0x4f, 0xc7, 0x93, 0x15, 0x43, 0x02, 0xff};
const uint8_t KEY_ID_01[] = {0xff, 0xb1, 0x4e, 0x09, 0x3c, 0x94, 0xc9, 0xca,
                             0xc9, 0x47, 0x16, 0x48, 0xb4, 0xf9, 0x87, 0x10};
const uint8_t COMMON_IV[] = {0x46, 0x22, 0xd4, 0xdd, 0x6d, 0x94, 0x41, 0x68, 0xee, 0xfb, 0x54, 0x98, 0x7c};

// An empty KID or Partial IV; memcpy and friends need a valid pointer even for 0 bytes
const uint8_t NO_BYTES[1] = {};

class OscoreServer : public TestableCoapServerNet {
 public:
  void set_material(std::vector<uint8_t> sender_id, std::vector<uint8_t> recipient_id) {
    this->set_oscore_master_secret({MASTER_SECRET, MASTER_SECRET + sizeof(MASTER_SECRET)});
    this->set_oscore_master_salt({MASTER_SALT, MASTER_SALT + sizeof(MASTER_SALT)});
    this->set_oscore_sender_id(std::move(sender_id));
    this->set_oscore_recipient_id(std::move(recipient_id));
  }
  bool derive_keys() { return this->oscore_derive_keys_(); }
  bool key_material_cleared() const {
    return this->oscore_master_secret_.empty() && this->oscore_master_salt_.empty() &&
           this->oscore_sender_id_.empty() && this->oscore_recipient_id_.empty();
  }
  const uint8_t *sender_key() const { return this->oscore_sender_key_; }
  const uint8_t *recipient_key() const { return this->oscore_recipient_key_; }
  const uint8_t *common_iv() const { return this->oscore_common_iv_; }

  size_t protect(const uint8_t *inner, size_t inner_len, const OscoreRequestInfo &req, bool is_notification,
                 uint8_t *out, size_t out_len) {
    return this->oscore_protect_response_(inner, inner_len, req, is_notification, out, out_len);
  }
  bool unprotect(const uint8_t *opt, uint8_t opt_len, const uint8_t *ciphertext, uint16_t ciphertext_len,
                 uint8_t *plaintext, size_t plaintext_size, size_t *plaintext_len, OscoreRequestInfo *req) {
    return this->oscore_unprotect_core_(opt, opt_len, ciphertext, ciphertext_len, plaintext, plaintext_size,
                                        plaintext_len, req);
  }
  uint8_t notify_option(uint8_t *buf) { return this->oscore_build_notify_option_(buf); }

  uint32_t sender_seq() const { return this->oscore_sender_seq_no_; }
  void set_sender_seq(uint32_t seq) { this->oscore_sender_seq_no_ = seq; }
  uint32_t seq_threshold() const { return this->oscore_seq_threshold_; }
  void restore_seq() { this->oscore_restore_seq_no_(); }
  void save_seq() { this->oscore_save_seq_no_(); }
  const std::string &pref_dir() const { return this->pref_dir_.dir(); }
  // What a restart would find: a fresh instance reads the preferences file
  static uint32_t stored_seq() {
    host::HostPreferences prefs;
    uint32_t value = 0;
    if (!prefs.load_from_key(OSCORE_SEQ_PREF_HASH, reinterpret_cast<uint8_t *>(&value), sizeof(value)))
      return 0;
    return value;
  }

  static uint8_t encode_piv(uint32_t seq, uint8_t *piv) { return oscore_encode_piv(seq, piv); }
  static void build_nonce(const uint8_t *piv, uint8_t piv_len, const uint8_t *kid, uint8_t kid_len,
                          const uint8_t *common_iv, uint8_t *nonce) {
    oscore_build_nonce(piv, piv_len, kid, kid_len, common_iv, nonce);
  }
  static size_t build_aad(const uint8_t *kid, uint8_t kid_len, const uint8_t *piv, uint8_t piv_len, uint8_t *buf,
                          size_t buf_len) {
    return oscore_build_aad(kid, kid_len, piv, piv_len, buf, buf_len);
  }
};

// The device under test: Sender ID h'01', Recipient ID h'' (the client)
class OscoreTest : public ::testing::Test {
 protected:
  void SetUp() override {
    this->srv_.set_material({0x01}, {});
    ASSERT_TRUE(this->srv_.derive_keys());
    this->srv_.restore_seq();
  }
  OscoreServer srv_;
};

// Encrypts as the client (Sender ID h'') would; the server's recipient key is the client's sender key
bool client_encrypt(OscoreServer &srv, const uint8_t *piv, uint8_t piv_len, const uint8_t *plaintext,
                    size_t plaintext_len, uint8_t *out, size_t *out_len) {
  uint8_t nonce[OSCORE_AES_NONCE_LEN];
  OscoreServer::build_nonce(piv, piv_len, NO_BYTES, 0, srv.common_iv(), nonce);
  uint8_t aad[80];
  size_t aad_len = OscoreServer::build_aad(NO_BYTES, 0, piv, piv_len, aad, sizeof(aad));
  return aad_len > 0 &&
         oscore_aes_ccm_encrypt(srv.recipient_key(), nonce, aad, aad_len, plaintext, plaintext_len, out, 256, out_len);
}

// Decrypts a reply as the client would: the reply reuses the request's nonce and AAD
bool client_decrypt_reply(OscoreServer &srv, const uint8_t *piv, uint8_t piv_len, const uint8_t *in, size_t in_len,
                          uint8_t *out, size_t *out_len) {
  uint8_t nonce[OSCORE_AES_NONCE_LEN];
  OscoreServer::build_nonce(piv, piv_len, NO_BYTES, 0, srv.common_iv(), nonce);
  uint8_t aad[80];
  size_t aad_len = OscoreServer::build_aad(NO_BYTES, 0, piv, piv_len, aad, sizeof(aad));
  return aad_len > 0 && oscore_aes_ccm_decrypt(srv.sender_key(), nonce, aad, aad_len, in, in_len, out, 256, out_len);
}

// ---------------------------------------------------------------------------
// Key derivation (RFC 8613 Appendix C.1.1)
// ---------------------------------------------------------------------------

TEST(OscoreKeys, ClientContextMatchesRfc8613) {
  OscoreServer srv;
  srv.set_material({}, {0x01});
  ASSERT_TRUE(srv.derive_keys());
  EXPECT_EQ(memcmp(srv.sender_key(), KEY_ID_EMPTY, sizeof(KEY_ID_EMPTY)), 0);
  EXPECT_EQ(memcmp(srv.recipient_key(), KEY_ID_01, sizeof(KEY_ID_01)), 0);
  EXPECT_EQ(memcmp(srv.common_iv(), COMMON_IV, sizeof(COMMON_IV)), 0);
}

TEST(OscoreKeys, SwappedIdsSwapKeys) {
  OscoreServer srv;
  srv.set_material({0x01}, {});
  ASSERT_TRUE(srv.derive_keys());
  EXPECT_EQ(memcmp(srv.sender_key(), KEY_ID_01, sizeof(KEY_ID_01)), 0);
  EXPECT_EQ(memcmp(srv.recipient_key(), KEY_ID_EMPTY, sizeof(KEY_ID_EMPTY)), 0);
}

TEST(OscoreKeys, KeyMaterialClearedAfterDerivation) {
  OscoreServer srv;
  srv.set_material({}, {0x01});
  ASSERT_TRUE(srv.derive_keys());
  EXPECT_TRUE(srv.key_material_cleared());
}

// ---------------------------------------------------------------------------
// Partial IV, nonce and AAD
// ---------------------------------------------------------------------------

TEST(OscorePiv, ShortestBigEndian) {
  struct Case {
    uint32_t seq;
    std::vector<uint8_t> expected;
  };
  const Case cases[] = {
      {0, {0x00}},
      {0xFF, {0xFF}},
      {0x100, {0x01, 0x00}},
      {0xFFFFFF, {0xFF, 0xFF, 0xFF}},
      {0x1000000, {0x01, 0x00, 0x00, 0x00}},
      {UINT32_MAX, {0xFF, 0xFF, 0xFF, 0xFF}},
  };
  for (const auto &c : cases) {
    uint8_t piv[5]{};
    uint8_t len = OscoreServer::encode_piv(c.seq, piv);
    EXPECT_EQ(std::vector<uint8_t>(piv, piv + len), c.expected) << "seq " << c.seq;
  }
}

// RFC 8613 Appendix C.4 request: Sender ID h'', Partial IV h'14'
TEST(OscoreNonce, MatchesRfc8613RequestVector) {
  const uint8_t piv[] = {0x14};
  const uint8_t expected[] = {0x46, 0x22, 0xd4, 0xdd, 0x6d, 0x94, 0x41, 0x68, 0xee, 0xfb, 0x54, 0x98, 0x68};
  uint8_t nonce[OSCORE_AES_NONCE_LEN];
  OscoreServer::build_nonce(piv, sizeof(piv), NO_BYTES, 0, COMMON_IV, nonce);
  EXPECT_EQ(memcmp(nonce, expected, sizeof(expected)), 0);
}

TEST(OscoreNonce, KidLengthFirstAndKidRightAligned) {
  const uint8_t zero_iv[OSCORE_AES_NONCE_LEN] = {};
  const uint8_t kid[] = {0x02};
  uint8_t nonce[OSCORE_AES_NONCE_LEN];
  OscoreServer::build_nonce(NO_BYTES, 0, kid, sizeof(kid), zero_iv, nonce);
  const uint8_t expected[] = {0x01, 0, 0, 0, 0, 0, 0, 0x02, 0, 0, 0, 0, 0};
  EXPECT_EQ(memcmp(nonce, expected, sizeof(expected)), 0);
}

TEST(OscoreAad, MatchesRfc8613RequestVector) {
  const uint8_t piv[] = {0x14};
  // ["Encrypt0", h'', h'8501810a40411440'], the inner array being [1, [10], h'', h'14', h'']
  const uint8_t expected[] = {0x83, 0x68, 'E',  'n',  'c',  'r',  'y',  'p',  't',  '0',
                              0x40, 0x48, 0x85, 0x01, 0x81, 0x0a, 0x40, 0x41, 0x14, 0x40};
  uint8_t aad[80];
  size_t len = OscoreServer::build_aad(NO_BYTES, 0, piv, sizeof(piv), aad, sizeof(aad));
  ASSERT_EQ(len, sizeof(expected));
  EXPECT_EQ(memcmp(aad, expected, sizeof(expected)), 0);
}

TEST(OscoreAad, BufferTooSmallReturnsZero) {
  const uint8_t kid[] = {0x01};
  const uint8_t piv[] = {0x00};
  uint8_t buf[4];
  EXPECT_EQ(OscoreServer::build_aad(kid, sizeof(kid), piv, sizeof(piv), buf, sizeof(buf)), 0u);
}

// ---------------------------------------------------------------------------
// AES-CCM backend
// ---------------------------------------------------------------------------

// RFC 8613 Appendix C.4: the client's GET for Uri-Path "tv1" with Partial IV h'14'
TEST(OscoreAesCcm, MatchesRfc8613RequestVector) {
  const uint8_t nonce[] = {0x46, 0x22, 0xd4, 0xdd, 0x6d, 0x94, 0x41, 0x68, 0xee, 0xfb, 0x54, 0x98, 0x68};
  const uint8_t aad[] = {0x83, 0x68, 'E',  'n',  'c',  'r',  'y',  'p',  't',  '0',
                         0x40, 0x48, 0x85, 0x01, 0x81, 0x0a, 0x40, 0x41, 0x14, 0x40};
  const uint8_t plaintext[] = {0x01, 0xb3, 0x74, 0x76, 0x31};
  const uint8_t expected[] = {0x61, 0x2f, 0x10, 0x92, 0xf1, 0x77, 0x6f, 0x1c, 0x16, 0x68, 0xb3, 0x82, 0x5e};
  uint8_t out[32];
  size_t out_len = 0;
  ASSERT_TRUE(oscore_aes_ccm_encrypt(KEY_ID_EMPTY, nonce, aad, sizeof(aad), plaintext, sizeof(plaintext), out,
                                     sizeof(out), &out_len));
  ASSERT_EQ(out_len, sizeof(expected));
  EXPECT_EQ(memcmp(out, expected, sizeof(expected)), 0);
}

TEST(OscoreAesCcm, RoundTripAndTamperedTagRejected) {
  const uint8_t nonce[OSCORE_AES_NONCE_LEN] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13};
  const uint8_t aad[] = {0xAA, 0xBB};
  const uint8_t plaintext[] = {0x01, 0xb3, 0x74, 0x76, 0x31};
  uint8_t ciphertext[32];
  size_t cipher_len = 0;
  ASSERT_TRUE(oscore_aes_ccm_encrypt(KEY_ID_EMPTY, nonce, aad, sizeof(aad), plaintext, sizeof(plaintext), ciphertext,
                                     sizeof(ciphertext), &cipher_len));
  ASSERT_EQ(cipher_len, sizeof(plaintext) + OSCORE_AES_TAG_LEN);

  uint8_t out[32];
  size_t out_len = 0;
  ASSERT_TRUE(oscore_aes_ccm_decrypt(KEY_ID_EMPTY, nonce, aad, sizeof(aad), ciphertext, cipher_len, out, sizeof(out),
                                     &out_len));
  ASSERT_EQ(out_len, sizeof(plaintext));
  EXPECT_EQ(memcmp(out, plaintext, sizeof(plaintext)), 0);

  ciphertext[cipher_len - 1] ^= 0x01;
  EXPECT_FALSE(oscore_aes_ccm_decrypt(KEY_ID_EMPTY, nonce, aad, sizeof(aad), ciphertext, cipher_len, out, sizeof(out),
                                      &out_len));
}

TEST(OscoreAesCcm, OutputBufferTooSmall) {
  const uint8_t nonce[OSCORE_AES_NONCE_LEN] = {};
  const uint8_t plaintext[] = {1, 2, 3};
  uint8_t out[sizeof(plaintext) + OSCORE_AES_TAG_LEN - 1];
  size_t out_len = 0;
  EXPECT_FALSE(oscore_aes_ccm_encrypt(KEY_ID_EMPTY, nonce, NO_BYTES, 0, plaintext, sizeof(plaintext), out, sizeof(out),
                                      &out_len));
}

// ---------------------------------------------------------------------------
// Protect and unprotect
// ---------------------------------------------------------------------------

TEST_F(OscoreTest, ReplyReusesRequestNonceAndKeepsSequence) {
  const uint8_t inner[] = {0x45, 0xFF, 0xDE, 0xAD};
  CoapServer::OscoreRequestInfo req{};
  req.piv[0] = 0x05;
  req.piv_len = 1;
  uint8_t ciphertext[64];
  uint32_t seq_before = this->srv_.sender_seq();

  size_t cipher_len = this->srv_.protect(inner, sizeof(inner), req, false, ciphertext, sizeof(ciphertext));

  ASSERT_EQ(cipher_len, sizeof(inner) + OSCORE_AES_TAG_LEN);
  EXPECT_EQ(this->srv_.sender_seq(), seq_before);
  uint8_t plain[256];
  size_t plain_len = 0;
  ASSERT_TRUE(client_decrypt_reply(this->srv_, req.piv, req.piv_len, ciphertext, cipher_len, plain, &plain_len));
  EXPECT_EQ(std::vector<uint8_t>(plain, plain + plain_len), std::vector<uint8_t>(inner, inner + sizeof(inner)));
}

// A notification above 0xFFFFFF must carry all 4 bytes of its sequence number, in the nonce and in the option
TEST_F(OscoreTest, NotificationWithFourBytePivDecrypts) {
  const uint32_t seq = 0x01020304;
  this->srv_.set_sender_seq(seq);
  const uint8_t inner[] = {0x45, 0xFF, 0x01};
  CoapServer::OscoreRequestInfo req{};  // AAD of the registering request
  uint8_t ciphertext[64];
  size_t cipher_len = this->srv_.protect(inner, sizeof(inner), req, true, ciphertext, sizeof(ciphertext));
  ASSERT_GT(cipher_len, 0u);
  EXPECT_EQ(this->srv_.sender_seq(), seq + 1);

  uint8_t opt[16];
  uint8_t opt_len = this->srv_.notify_option(opt);
  ASSERT_EQ(opt[0] & 0x07, 4);  // PIV length
  EXPECT_EQ(std::vector<uint8_t>(opt + 1, opt + 5), (std::vector<uint8_t>{0x01, 0x02, 0x03, 0x04}));
  EXPECT_EQ(opt_len, 1 + 4 + 1);  // flags, PIV, Sender ID h'01'

  const uint8_t kid[] = {0x01};
  uint8_t nonce[OSCORE_AES_NONCE_LEN];
  OscoreServer::build_nonce(opt + 1, 4, kid, sizeof(kid), this->srv_.common_iv(), nonce);
  uint8_t aad[80];
  size_t aad_len = OscoreServer::build_aad(req.kid, req.kid_len, req.piv, req.piv_len, aad, sizeof(aad));
  uint8_t plain[64];
  size_t plain_len = 0;
  ASSERT_TRUE(oscore_aes_ccm_decrypt(this->srv_.sender_key(), nonce, aad, aad_len, ciphertext, cipher_len, plain,
                                     sizeof(plain), &plain_len));
  EXPECT_EQ(std::vector<uint8_t>(plain, plain + plain_len), std::vector<uint8_t>(inner, inner + sizeof(inner)));
}

TEST_F(OscoreTest, UnprotectDecryptsRequest) {
  const uint8_t piv[] = {0x01};
  const uint8_t opt[] = {0x09, 0x01};  // PIV length 1, k flag with an empty KID
  const uint8_t inner[] = {0x01, 0xFF, 0xDE, 0xAD};
  uint8_t ciphertext[256];
  size_t cipher_len = 0;
  ASSERT_TRUE(client_encrypt(this->srv_, piv, sizeof(piv), inner, sizeof(inner), ciphertext, &cipher_len));

  uint8_t plain[256];
  size_t plain_len = 0;
  CoapServer::OscoreRequestInfo req{};
  ASSERT_TRUE(this->srv_.unprotect(opt, sizeof(opt), ciphertext, (uint16_t) cipher_len, plain, sizeof(plain),
                                   &plain_len, &req));
  EXPECT_EQ(std::vector<uint8_t>(plain, plain + plain_len), std::vector<uint8_t>(inner, inner + sizeof(inner)));
  EXPECT_EQ(req.piv_len, 1u);
  EXPECT_EQ(req.piv[0], 0x01);
  EXPECT_EQ(req.kid_len, 0u);
}

TEST_F(OscoreTest, UnprotectRejectsReplay) {
  const uint8_t piv[] = {0x07};
  const uint8_t opt[] = {0x01, 0x07};
  const uint8_t inner[] = {0x01, 0xFF, 0xAA};
  uint8_t ciphertext[256];
  size_t cipher_len = 0;
  ASSERT_TRUE(client_encrypt(this->srv_, piv, sizeof(piv), inner, sizeof(inner), ciphertext, &cipher_len));
  uint8_t plain[256];
  size_t plain_len = 0;
  CoapServer::OscoreRequestInfo req{};

  ASSERT_TRUE(this->srv_.unprotect(opt, sizeof(opt), ciphertext, (uint16_t) cipher_len, plain, sizeof(plain),
                                   &plain_len, &req));
  EXPECT_FALSE(this->srv_.unprotect(opt, sizeof(opt), ciphertext, (uint16_t) cipher_len, plain, sizeof(plain),
                                    &plain_len, &req));
}

TEST_F(OscoreTest, UnprotectRejectsTamperedCiphertext) {
  const uint8_t piv[] = {0x02};
  const uint8_t opt[] = {0x01, 0x02};
  const uint8_t inner[] = {0x01, 0xFF, 0xAA};
  uint8_t ciphertext[256];
  size_t cipher_len = 0;
  ASSERT_TRUE(client_encrypt(this->srv_, piv, sizeof(piv), inner, sizeof(inner), ciphertext, &cipher_len));
  ciphertext[0] ^= 0x80;
  uint8_t plain[256];
  size_t plain_len = 0;
  CoapServer::OscoreRequestInfo req{};
  EXPECT_FALSE(this->srv_.unprotect(opt, sizeof(opt), ciphertext, (uint16_t) cipher_len, plain, sizeof(plain),
                                    &plain_len, &req));
}

// Without a Partial IV there is no replay check, so such a request could be replayed forever
TEST_F(OscoreTest, UnprotectRejectsRequestWithoutPiv) {
  const uint8_t opt[] = {0x00};
  const uint8_t inner[] = {0x02, 0xFF, 0xA1, 0x04, 0xF5};
  uint8_t ciphertext[256];
  size_t cipher_len = 0;
  ASSERT_TRUE(client_encrypt(this->srv_, NO_BYTES, 0, inner, sizeof(inner), ciphertext, &cipher_len));
  uint8_t plain[256];
  size_t plain_len = 0;
  CoapServer::OscoreRequestInfo req{};
  EXPECT_FALSE(this->srv_.unprotect(opt, sizeof(opt), ciphertext, (uint16_t) cipher_len, plain, sizeof(plain),
                                    &plain_len, &req));
}

// PIV lengths 6 and 7 are reserved and used to overflow the 5-byte PIV buffer
TEST_F(OscoreTest, UnprotectRejectsReservedPivLength) {
  const uint8_t opt[] = {0x07, 1, 2, 3, 4, 5, 6, 7};
  const uint8_t ciphertext[16] = {};
  uint8_t plain[64];
  size_t plain_len = 0;
  CoapServer::OscoreRequestInfo req{};
  EXPECT_FALSE(
      this->srv_.unprotect(opt, sizeof(opt), ciphertext, sizeof(ciphertext), plain, sizeof(plain), &plain_len, &req));
}

// ---------------------------------------------------------------------------
// Persisted sender sequence number
// ---------------------------------------------------------------------------

TEST_F(OscoreTest, SaveStoresThresholdAheadOfSequence) {
  this->srv_.set_sender_seq(100);
  this->srv_.save_seq();

  EXPECT_EQ(this->srv_.seq_threshold(), 100u + 1024u);
  EXPECT_EQ(OscoreServer::stored_seq(), 100u + 1024u);
}

TEST_F(OscoreTest, RestoreContinuesFromStoredThreshold) {
  this->srv_.set_sender_seq(100);
  this->srv_.save_seq();

  // As after a restart: the next server starts at the stored threshold and stores the block after it
  this->srv_.set_sender_seq(0);
  this->srv_.restore_seq();

  EXPECT_EQ(this->srv_.sender_seq(), 1124u);
  EXPECT_EQ(OscoreServer::stored_seq(), 1124u + 1024u);
}

TEST_F(OscoreTest, NotificationReachingThresholdSavesNextBlock) {
  this->srv_.set_sender_seq(0);
  this->srv_.save_seq();
  this->srv_.set_sender_seq(1023);
  const uint8_t inner[] = {0x45};
  CoapServer::OscoreRequestInfo req{};
  uint8_t out[64];

  ASSERT_GT(this->srv_.protect(inner, sizeof(inner), req, true, out, sizeof(out)), 0u);

  EXPECT_EQ(OscoreServer::stored_seq(), 1024u + 1024u);
}

TEST_F(OscoreTest, FailedSaveWarnsRetriesAndRecovers) {
  this->srv_.set_sender_seq(0);
  this->srv_.save_seq();
  ASSERT_FALSE(this->srv_.status_has_warning());

  // Without its folder the preferences file cannot be written
  std::filesystem::remove_all(this->srv_.pref_dir());
  this->srv_.set_sender_seq(5000);
  this->srv_.save_seq();
  EXPECT_TRUE(this->srv_.status_has_warning());
  EXPECT_EQ(this->srv_.seq_threshold(), 1024u);  // unchanged, so the next notification retries

  std::filesystem::create_directories(this->srv_.pref_dir());
  this->srv_.save_seq();
  EXPECT_FALSE(this->srv_.status_has_warning());
  EXPECT_EQ(OscoreServer::stored_seq(), 5000u + 1024u);
}

// ---------------------------------------------------------------------------
// Through the network transport
// ---------------------------------------------------------------------------

// NON POST with token 0xCD, an OSCORE option and the ciphertext as payload
std::vector<uint8_t> make_oscore_post(const char *path, const uint8_t *opt, uint8_t opt_len, const uint8_t *ciphertext,
                                      size_t cipher_len) {
  std::vector<uint8_t> pkt{0x51, 0x02, 0x00, 0x07, 0xCD};
  pkt.push_back(static_cast<uint8_t>((9 << 4) | opt_len));  // OSCORE (9)
  pkt.insert(pkt.end(), opt, opt + opt_len);
  append_uri_path(pkt, path, 9);
  pkt.push_back(0xFF);
  pkt.insert(pkt.end(), ciphertext, ciphertext + cipher_len);
  return pkt;
}

TEST_F(OscoreTest, ProtectedResourceRejectsPlainRequest) {
  this->srv_.init_resources(4);
  TestSwitch sw;
  sw.configure("relay");
  uint16_t idx = 1;
  this->srv_.add_resource(ENTITYTYPE_SWITCH, &sw, true, idx, false);

  uint8_t cbor[] = {0xA1, 0x04, 0xF5};
  this->srv_.inject(make_coap_post("fp/1/g/1", cbor, sizeof(cbor)), make_peer());

  ASSERT_GE(this->srv_.last_sent.size(), 2u);
  EXPECT_EQ(this->srv_.last_sent[1], 0x81u);  // 4.01 Unauthorized
  EXPECT_FALSE(sw.state);
}

TEST_F(OscoreTest, ProtectedPostIsAppliedAndAnsweredEncrypted) {
  this->srv_.init_resources(4);
  TestSwitch sw;
  sw.configure("relay");
  uint16_t idx = 1;
  this->srv_.add_resource(ENTITYTYPE_SWITCH, &sw, true, idx, false);

  const uint8_t piv[] = {0x2A};
  const uint8_t opt[] = {0x09, 0x2A};                      // PIV length 1, k flag with an empty KID
  const uint8_t inner[] = {0x02, 0xFF, 0xA1, 0x04, 0xF5};  // POST, {4: true}
  uint8_t ciphertext[256];
  size_t cipher_len = 0;
  ASSERT_TRUE(client_encrypt(this->srv_, piv, sizeof(piv), inner, sizeof(inner), ciphertext, &cipher_len));

  this->srv_.inject(make_oscore_post("fp/1/g/1", opt, sizeof(opt), ciphertext, cipher_len), make_peer());

  EXPECT_TRUE(sw.state);
  ASSERT_GE(this->srv_.last_sent.size(), 2u);
  EXPECT_EQ(this->srv_.last_sent[1], 0x44u);
  std::string reply = payload_of(this->srv_.last_sent);
  uint8_t plain[256];
  size_t plain_len = 0;
  ASSERT_TRUE(client_decrypt_reply(this->srv_, piv, sizeof(piv), reinterpret_cast<const uint8_t *>(reply.data()),
                                   reply.size(), plain, &plain_len));
  ASSERT_GE(plain_len, 4u);
  EXPECT_EQ(plain[0], 0x44u);  // inner code 2.04
  EXPECT_EQ(plain[1], 0xC1u);  // Content-Format, 1 byte
  EXPECT_EQ(plain[2], 60u);    // application/cbor
  const uint8_t state[] = {0xA1, 0x04, 0xF5};
  EXPECT_EQ(std::vector<uint8_t>(plain + plain_len - sizeof(state), plain + plain_len),
            std::vector<uint8_t>(state, state + sizeof(state)));
}

}  // namespace
}  // namespace esphome::coap_server::testing

#endif  // USE_COAP_OSCORE
