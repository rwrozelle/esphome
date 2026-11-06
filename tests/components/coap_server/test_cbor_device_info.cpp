// Tests for CoapServer::encode_device_info (the /info payload).

#include "common.h"
#include "esphome/core/application.h"

namespace esphome::coap_server::testing {
namespace {

class InfoServer : public TestableCoapServerNet {
 public:
  size_t encode_info(uint8_t *buf, size_t len) { return encode_device_info(buf, len, this); }
};

// Reads the text-string key at buf[i]; returns its length and sets hdr, or SIZE_MAX if it is not one
size_t text_header(const uint8_t *buf, size_t len, size_t i, size_t *hdr) {
  if ((buf[i] & 0xe0u) != 0x60u)
    return SIZE_MAX;
  if ((buf[i] & 0x1fu) <= 23u) {
    *hdr = 1;
    return buf[i] & 0x1fu;
  }
  if (buf[i] == 0x78u && i + 1 < len) {
    *hdr = 2;
    return buf[i + 1];
  }
  return SIZE_MAX;
}

// Position of the value that follows text key, or SIZE_MAX. Only meant for the flat keys of this map.
size_t find_value(const uint8_t *buf, size_t len, const char *key) {
  size_t key_len = strlen(key);
  for (size_t i = 0; i < len; i++) {
    size_t hdr = 0;
    size_t klen = text_header(buf, len, i, &hdr);
    if (klen == key_len && i + hdr + klen < len && memcmp(buf + i + hdr, key, key_len) == 0)
      return i + hdr + klen;
  }
  return SIZE_MAX;
}

bool find_text(const uint8_t *buf, size_t len, const char *key, std::string *out) {
  size_t vi = find_value(buf, len, key);
  if (vi == SIZE_MAX)
    return false;
  size_t hdr = 0;
  size_t vlen = text_header(buf, len, vi, &hdr);
  if (vlen == SIZE_MAX || vi + hdr + vlen > len)
    return false;
  out->assign(reinterpret_cast<const char *>(buf + vi + hdr), vlen);
  return true;
}

bool find_uint(const uint8_t *buf, size_t len, const char *key, uint32_t *out) {
  size_t vi = find_value(buf, len, key);
  if (vi == SIZE_MAX || (buf[vi] & 0xe0u) != 0x00u)
    return false;
  uint8_t info = buf[vi] & 0x1fu;
  if (info <= 23u) {
    *out = info;
  } else if (info == 0x18u && vi + 1 < len) {
    *out = buf[vi + 1];
  } else if (info == 0x19u && vi + 2 < len) {
    *out = (static_cast<uint32_t>(buf[vi + 1]) << 8) | buf[vi + 2];
  } else {
    return false;
  }
  return true;
}

class DeviceInfoTest : public ::testing::Test {
 protected:
  void SetUp() override {
    this->saved_name_ = App.get_name();
    this->saved_friendly_name_ = App.get_friendly_name();
    set_names("My Device");
  }
  // App is shared with every other test in the binary
  void TearDown() override {
    App.pre_setup(this->saved_name_.c_str(), this->saved_name_.size(), this->saved_friendly_name_.c_str(),
                  this->saved_friendly_name_.size());
  }

  static void set_names(const char *friendly_name) {
    App.pre_setup("my_device", strlen("my_device"), friendly_name, strlen(friendly_name));
  }

  size_t encode() { return this->srv_.encode_info(this->buf_, sizeof(this->buf_)); }

  StringRef saved_name_;
  StringRef saved_friendly_name_;
  InfoServer srv_;
  uint8_t buf_[512]{};
};

TEST_F(DeviceInfoTest, ContainsName) {
  size_t len = this->encode();
  std::string value;
  ASSERT_TRUE(find_text(this->buf_, len, "name", &value));
  EXPECT_EQ(value, "my_device");
}

TEST_F(DeviceInfoTest, ContainsFriendlyName) {
  size_t len = this->encode();
  std::string value;
  ASSERT_TRUE(find_text(this->buf_, len, "friendly_name", &value));
  EXPECT_EQ(value, "My Device");
}

TEST_F(DeviceInfoTest, FriendlyNameOmittedWhenEmpty) {
  set_names("");
  size_t len = this->encode();
  ASSERT_GT(len, 0u);
  EXPECT_EQ(find_value(this->buf_, len, "friendly_name"), SIZE_MAX);
}

TEST_F(DeviceInfoTest, ContainsVersionModelAndBuildTime) {
  size_t len = this->encode();
  std::string value;
  ASSERT_TRUE(find_text(this->buf_, len, "version", &value));
  EXPECT_EQ(value, ESPHOME_VERSION);
  ASSERT_TRUE(find_text(this->buf_, len, "model", &value));
  EXPECT_EQ(value, ESPHOME_BOARD);
  ASSERT_TRUE(find_text(this->buf_, len, "build_time", &value));
  EXPECT_FALSE(value.empty());
}

TEST_F(DeviceInfoTest, MapHasOneEntryPerKey) {
  // 10 fixed keys plus friendly_name, plus areas and devices when App has any
  uint8_t expected = 11;
#ifdef USE_AREAS
  expected += App.get_areas().empty() ? 0 : 1;
#endif
#ifdef USE_DEVICES
  expected += App.get_devices().empty() ? 0 : 1;
#endif
  size_t len = this->encode();
  ASSERT_GT(len, 0u);
  EXPECT_EQ(this->buf_[0], 0xa0u | expected);
}

TEST_F(DeviceInfoTest, PingFieldsReflectServerSettings) {
  this->srv_.set_server_ping_interval(30000);
  this->srv_.set_server_ping_timeout_ratio(2.0f);
  this->srv_.set_server_ping_retry(3);
  size_t len = this->encode();
  uint32_t value = 0;
  ASSERT_TRUE(find_uint(this->buf_, len, "ping_interval", &value));
  EXPECT_EQ(value, 30u);
  ASSERT_TRUE(find_uint(this->buf_, len, "ping_timeout", &value));
  EXPECT_EQ(value, 60u);
  ASSERT_TRUE(find_uint(this->buf_, len, "ping_retry", &value));
  EXPECT_EQ(value, 3u);
}

TEST_F(DeviceInfoTest, ObserveRetryDefaultIsZero) {
  size_t len = this->encode();
  uint32_t value = 99;
  ASSERT_TRUE(find_uint(this->buf_, len, "observe_retry", &value));
  EXPECT_EQ(value, 0u);
}

TEST_F(DeviceInfoTest, ObserveRetryReflectsConfiguredValue) {
  this->srv_.set_observe_retry(3);
  size_t len = this->encode();
  uint32_t value = 99;
  ASSERT_TRUE(find_uint(this->buf_, len, "observe_retry", &value));
  EXPECT_EQ(value, 3u);
}

TEST_F(DeviceInfoTest, OscoreFlagMatchesBuild) {
  size_t len = this->encode();
  size_t vi = find_value(this->buf_, len, "oscore");
  ASSERT_NE(vi, SIZE_MAX);
#ifdef USE_COAP_OSCORE
  EXPECT_EQ(this->buf_[vi], 0xf5u);
#else
  EXPECT_EQ(this->buf_[vi], 0xf4u);
#endif
}

TEST_F(DeviceInfoTest, BufferTooSmallReturnsZero) {
  uint8_t small[8];
  EXPECT_EQ(this->srv_.encode_info(small, sizeof(small)), 0u);
}

}  // namespace
}  // namespace esphome::coap_server::testing
