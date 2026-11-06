// Tests for the .well-known/core link-format payload.
// Entry contents are tested through format_link_entry(), which takes the entity directly. Whole-payload
// tests only rely on the entries build_link_format() always writes, since App is shared by every test.

#include "common.h"
#include "esphome/core/application.h"

namespace esphome::coap_server::testing {
namespace {

class LinkFormatServer : public TestableCoapServerNet {
 public:
  static std::string entry(const char *path, EntityBase *entity, EntityType type, bool observable = true,
                           ActionType action = ACTIONTYPE_NO_ACTION, uint8_t device_index = 0, bool add_comma = false) {
    char buf[256];
    LinkFormatResource res{path, entity_type_domain_name(type), entity, type, action, observable, device_index};
    uint16_t len = format_link_entry(buf, sizeof(buf), res, add_comma);
    return std::string(buf, len);
  }
  static uint16_t entry_into(char *buf, size_t buf_len, EntityBase *entity, EntityType type) {
    LinkFormatResource res{"fp/1/g/1", entity_type_domain_name(type), entity, type, ACTIONTYPE_NO_ACTION, true, 0};
    return format_link_entry(buf, buf_len, res, false);
  }
  static size_t build(uint8_t *buf, size_t buf_len) { return build_link_format(buf, buf_len); }
};

bool contains(const std::string &haystack, const char *needle) { return haystack.find(needle) != std::string::npos; }

uint32_t dc_uom_fields(const char *device_class, const char *unit) {
  uint32_t fields = 0;
  if (device_class != nullptr)
    fields |= static_cast<uint32_t>(string_table_index(entity_device_class_lookup, device_class))
              << ENTITY_FIELD_DC_SHIFT;
  if (unit != nullptr)
    fields |= static_cast<uint32_t>(string_table_index(entity_uom_lookup, unit)) << ENTITY_FIELD_UOM_SHIFT;
  return fields;
}

// ---------------------------------------------------------------------------
// Entity entries
// ---------------------------------------------------------------------------

TEST(LinkFormatEntry, SensorHasRtObsTitleOidUomDcAd) {
  ASSERT_NE(dc_uom_fields("temperature", "°C"), 0u) << "strings not registered by the test codegen";
  TestSensor s;
  s.configure("Temperature", 305419896u, dc_uom_fields("temperature", "°C"));
  s.set_accuracy_decimals(2);

  auto e = LinkFormatServer::entry("fp/1/g/1", &s, ENTITYTYPE_SENSOR);

  EXPECT_EQ(e.rfind("</fp/1/g/1>;rt=\"esphome.sensor\";if=\"if.s\";ct=112;obs", 0), 0u) << e;
  EXPECT_TRUE(contains(e, ";title=\"Temperature\"")) << e;
  EXPECT_TRUE(contains(e, ";oid=305419896")) << e;
  EXPECT_TRUE(contains(e, ";uom=\"°C\"")) << e;
  EXPECT_TRUE(contains(e, ";ad=2")) << e;
  EXPECT_TRUE(contains(e, ";dc=\"temperature\"")) << e;
}

TEST(LinkFormatEntry, SensorWithoutUomOrDcOmitsThem) {
  TestSensor s;
  s.configure("raw", 1u);

  auto e = LinkFormatServer::entry("fp/1/g/1", &s, ENTITYTYPE_SENSOR);

  EXPECT_FALSE(contains(e, ";uom=")) << e;
  EXPECT_FALSE(contains(e, ";dc=")) << e;
}

TEST(LinkFormatEntry, TextSensorHasDcButNoAd) {
  TestTextSensor ts;
  ts.configure("status", 7u, dc_uom_fields("temperature", nullptr));

  auto e = LinkFormatServer::entry("fp/1/g/1", &ts, ENTITYTYPE_TEXT_SENSOR);

  EXPECT_TRUE(contains(e, "rt=\"esphome.text_sensor\"")) << e;
  EXPECT_TRUE(contains(e, ";dc=\"temperature\"")) << e;
  EXPECT_FALSE(contains(e, ";ad=")) << e;
}

TEST(LinkFormatEntry, NonObservableUsesIfAWithoutObs) {
  TestSensor s;
  s.configure("noobs", 5u);

  auto e = LinkFormatServer::entry("fp/1/g/1", &s, ENTITYTYPE_SENSOR, false);

  EXPECT_TRUE(contains(e, "if=\"if.a\"")) << e;
  EXPECT_FALSE(contains(e, ";obs")) << e;
}

TEST(LinkFormatEntry, CommaPrefix) {
  TestSensor s;
  s.configure("x", 42u);

  auto e = LinkFormatServer::entry("fp/1/g/1", &s, ENTITYTYPE_SENSOR, true, ACTIONTYPE_NO_ACTION, 0, true);

  EXPECT_EQ(e.rfind(",</fp/1/g/1>", 0), 0u) << e;
}

TEST(LinkFormatEntry, SwitchToggleIsAction) {
  TestSwitch sw;
  sw.configure("light", 99u);

  auto e = LinkFormatServer::entry("fp/1/g/2", &sw, ENTITYTYPE_SWITCH, false, ACTIONTYPE_TOGGLE);

  EXPECT_EQ(e, "</fp/1/g/2>;rt=\"esphome.action\";if=\"if.a\";ct=112;title=\"toggle\"");
}

TEST(LinkFormatEntry, ValveStopIsAction) {
  TestValve v;
  v.configure("water", 77u);

  auto e = LinkFormatServer::entry("fp/1/g/2", &v, ENTITYTYPE_VALVE, false, ACTIONTYPE_STOP);

  EXPECT_EQ(e, "</fp/1/g/2>;rt=\"esphome.action\";if=\"if.a\";ct=112;title=\"stop\"");
}

TEST(LinkFormatEntry, InfoEntry) {
  auto e = LinkFormatServer::entry("info", nullptr, ENTITYTYPE_DEVICE, false);

  EXPECT_EQ(e, "</info>;rt=\"esphome.device\";if=\"if.a\";ct=60");
}

TEST(LinkFormatEntry, LogEntry) {
  auto e = LinkFormatServer::entry("fp/9/g/1", nullptr, ENTITYTYPE_LOG);

  EXPECT_EQ(e, "</fp/9/g/1>;rt=\"esphome.log\";if=\"if.s\";ct=112;obs");
}

#ifdef USE_DEVICES
TEST(LinkFormatEntry, DvWhenDeviceIndexSet) {
  TestSensor s;
  s.configure("remote", 3u);

  EXPECT_TRUE(
      contains(LinkFormatServer::entry("fp/1/g/1", &s, ENTITYTYPE_SENSOR, true, ACTIONTYPE_NO_ACTION, 2), ";dv=2"));
  EXPECT_TRUE(
      contains(LinkFormatServer::entry("fp/1/g/2", &s, ENTITYTYPE_SWITCH, false, ACTIONTYPE_TOGGLE, 2), ";dv=2"));
}

TEST(LinkFormatEntry, NoDvForMainDevice) {
  TestSensor s;
  s.configure("local", 4u);

  EXPECT_FALSE(contains(LinkFormatServer::entry("fp/1/g/1", &s, ENTITYTYPE_SENSOR), ";dv="));
}
#endif

TEST(LinkFormatEntry, NumberHasMinMaxStep) {
  TestNumber n;
  n.configure("brightness", 42u);
  n.traits.set_min_value(-10.0f);
  n.traits.set_max_value(255.0f);
  n.traits.set_step(0.5f);

  auto e = LinkFormatServer::entry("fp/1/g/1", &n, ENTITYTYPE_NUMBER);

  EXPECT_TRUE(contains(e, ";min=-10;max=255;step=0.5")) << e;
}

TEST(LinkFormatEntry, NumberNaNRangeOmitted) {
  TestNumber n;
  n.configure("setpoint", 43u);

  auto e = LinkFormatServer::entry("fp/1/g/1", &n, ENTITYTYPE_NUMBER);

  EXPECT_FALSE(contains(e, ";min=")) << e;
  EXPECT_FALSE(contains(e, ";max=")) << e;
  EXPECT_FALSE(contains(e, ";step=")) << e;
}

TEST(LinkFormatEntry, NumberOnlyStepSet) {
  TestNumber n;
  n.configure("speed", 44u);
  n.traits.set_step(1.0f);

  auto e = LinkFormatServer::entry("fp/1/g/1", &n, ENTITYTYPE_NUMBER);

  EXPECT_FALSE(contains(e, ";min=")) << e;
  EXPECT_FALSE(contains(e, ";max=")) << e;
  EXPECT_TRUE(contains(e, ";step=1")) << e;
}

TEST(LinkFormatEntry, TruncatedToBuffer) {
  TestSensor s;
  s.configure("a name that does not fit in the buffer", 1u);
  char buf[300];
  memset(buf, 0x55, sizeof(buf));

  uint16_t len = LinkFormatServer::entry_into(buf, 20, &s, ENTITYTYPE_SENSOR);

  EXPECT_EQ(len, 20u);
  EXPECT_EQ(static_cast<uint8_t>(buf[20]), 0x55u);
}

// ---------------------------------------------------------------------------
// Whole payload
// ---------------------------------------------------------------------------

std::string build_link_format_string() {
  size_t size = LinkFormatServer::build(nullptr, 0);
  std::string lf(size, '\0');
  LinkFormatServer::build(reinterpret_cast<uint8_t *>(lf.data()), size);
  return lf;
}

TEST(LinkFormatPayload, StartsWithInfoAndEndsWithPing) {
  auto lf = build_link_format_string();

  EXPECT_EQ(lf.rfind("</info>;rt=\"esphome.device\"", 0), 0u) << lf;
  const std::string ping = ",</ping>;rt=\"esphome.ping\";if=\"if.a\";ct=112";
  ASSERT_GE(lf.size(), ping.size());
  EXPECT_EQ(lf.substr(lf.size() - ping.size()), ping);
}

TEST(LinkFormatPayload, HasLogEntry) {
  auto lf = build_link_format_string();

  EXPECT_TRUE(contains(lf, ";rt=\"esphome.log\"")) << lf;
}

TEST(LinkFormatPayload, SmallBufferStillReturnsFullSize) {
  size_t size = LinkFormatServer::build(nullptr, 0);
  std::vector<uint8_t> buf(16, 0x55);

  size_t reported = LinkFormatServer::build(buf.data(), 8);

  EXPECT_EQ(reported, size);
  for (size_t i = 8; i < buf.size(); i++)
    EXPECT_EQ(buf[i], 0x55u) << "byte " << i;
}

TEST(LinkFormatPayload, WellKnownCoreServesCache) {
  TestableCoapServerNet srv;
  srv.populate_link_format_cache();

  srv.inject(make_coap_get(".well-known/core"), make_peer());

  auto lf = build_link_format_string();
  ASSERT_LE(lf.size(), 1024u) << "more than one block; the first-block check below would need updating";
  EXPECT_EQ(payload_of(srv.last_sent), lf);
}

}  // namespace
}  // namespace esphome::coap_server::testing
