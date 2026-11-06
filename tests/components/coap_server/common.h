#pragma once

#include <gtest/gtest.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "esphome/components/coap_server/coap_server_net.h"

namespace esphome::coap_server::testing {

// ---------------------------------------------------------------------------
// Entities
// ---------------------------------------------------------------------------

// Names the entity through the same call codegen emits
template<typename Base> class TestEntity : public Base {
 public:
  void configure(const char *name, uint32_t object_id_hash = 0, uint32_t entity_fields = 0) {
    this->configure_entity_(name, object_id_hash, entity_fields);
  }
};

using TestSensor = TestEntity<sensor::Sensor>;
using TestBinarySensor = TestEntity<binary_sensor::BinarySensor>;
using TestTextSensor = TestEntity<text_sensor::TextSensor>;

class TestSwitch : public TestEntity<switch_::Switch> {
 protected:
  void write_state(bool state) override { this->state = state; }
};

class TestNumber : public TestEntity<number::Number> {
 public:
  float last_control{NAN};

 protected:
  void control(float value) override { this->last_control = value; }
};

class TestLock : public TestEntity<lock::Lock> {
 protected:
  void control(const lock::LockCall &call) override {
    if (call.get_state().has_value())
      this->state = *call.get_state();
  }
};

class TestValve : public TestEntity<valve::Valve> {
 public:
  valve::ValveTraits get_traits() override {
    valve::ValveTraits traits;
    traits.set_supports_stop(true);
    return traits;
  }
  optional<float> last_position;
  bool last_stop{false};

 protected:
  void control(const valve::ValveCall &call) override {
    this->last_position = call.get_position();
    this->last_stop = call.get_stop();
  }
};

class TestButton : public TestEntity<button::Button> {
 public:
  int press_count{0};

 protected:
  void press_action() override { this->press_count++; }
};

// Index of value in a generated entity string table (entity_device_class_lookup etc.), 0 if absent
inline uint8_t string_table_index(const char *(*lookup)(uint8_t), const char *value) {
  for (unsigned i = 1; i <= 255; i++) {
    const char *entry = lookup(i);
    if (entry[0] == '\0')
      return 0;
    if (strcmp(entry, value) == 0)
      return static_cast<uint8_t>(i);
  }
  return 0;
}

// ---------------------------------------------------------------------------
// Server
// ---------------------------------------------------------------------------

#ifdef USE_COAP_OSCORE
// Sends the OSCORE sequence number file to a fresh temporary folder instead of ~/.esphome/prefs
class ScopedPrefDir {
 public:
  ScopedPrefDir() {
    std::string tmpl = (std::filesystem::temp_directory_path() / "coap_server_test_XXXXXX").string();
    if (mkdtemp(tmpl.data()) != nullptr) {
      this->dir_ = tmpl;
    } else {
      ADD_FAILURE() << "mkdtemp failed for " << tmpl;
    }
    if (const char *old = getenv("ESPHOME_PREFDIR"); old != nullptr) {
      this->had_old_ = true;
      this->old_ = old;
    }
    // Without a folder of its own, a path that cannot be created, so nothing is written anywhere else
    setenv("ESPHOME_PREFDIR", this->dir_.empty() ? "/proc/coap_server_test" : this->dir_.c_str(), 1);
  }
  ~ScopedPrefDir() {
    if (this->had_old_) {
      setenv("ESPHOME_PREFDIR", this->old_.c_str(), 1);
    } else {
      unsetenv("ESPHOME_PREFDIR");
    }
    if (!this->dir_.empty()) {
      std::error_code ec;
      std::filesystem::remove_all(this->dir_, ec);
    }
  }
  ScopedPrefDir(const ScopedPrefDir &) = delete;
  ScopedPrefDir &operator=(const ScopedPrefDir &) = delete;

  const std::string &dir() const { return this->dir_; }

 private:
  std::string dir_;
  std::string old_;
  bool had_old_{false};
};
#endif

// Captures outbound packets instead of using a socket, and exposes the protected members the tests drive
class TestableCoapServerNet : public CoapServerNet {
 public:
  // The scheduler outlives this object, and the ping timers capture this
  ~TestableCoapServerNet() {
    for (auto &client : this->active_clients_) {
      if (client.active)
        this->cancel_ping_client_(&client);
    }
    this->teardown();
    if (global_coap_server == this)
      global_coap_server = nullptr;
  }

  void init_resources(size_t count) { this->resources_.init(count); }

  void inject(const uint8_t *buf, size_t len, const sockaddr_storage &peer) {
    this->process_datagram_(buf, len, &peer);
  }
  void inject(const std::vector<uint8_t> &pkt, const sockaddr_storage &peer) {
    this->inject(pkt.data(), pkt.size(), peer);
  }

  // Adds the resource(s) for one entity; exempt resources take plain requests when OSCORE is compiled in
  void add_resource(EntityType type, EntityBase *entity, bool observable, uint16_t &senml_index,
                    bool oscore_exempt = true) {
    size_t first = this->resources_.size();
    this->add_net_resource_(type, entity, observable, senml_index);
    for (size_t i = first; i < this->resources_.size(); i++)
      this->resources_[i].oscore_exempt = oscore_exempt;
  }

  // Builds the .well-known/core cache from the App entity lists, as setup() does
  void populate_link_format_cache() {
    size_t size = build_link_format(nullptr, 0);
    this->link_format_buf_ = std::make_unique<uint8_t[]>(size);
    build_link_format(this->link_format_buf_.get(), size);
    this->link_format_size_ = size;
  }

  void set_link_format(const std::string &content) {
    this->link_format_buf_ = std::make_unique<uint8_t[]>(content.size());
    memcpy(this->link_format_buf_.get(), content.data(), content.size());
    this->link_format_size_ = content.size();
  }

  uint16_t peek_next_msg_id() const { return this->next_msg_id_; }

  NetCoapClient *connect_client(const sockaddr_storage &peer) { return this->new_client_(peer); }
  void disconnect_client(NetCoapClient *client) { this->free_client_(client); }
  NetCoapClient *first_active_client() {
    for (auto &client : this->active_clients_) {
      if (client.active)
        return &client;
    }
    return nullptr;
  }

  NetCoapObserver *first_active_observer() { return this->active_observers_; }
  size_t active_observer_count() const {
    size_t count = 0;
    for (auto *obs = this->active_observers_; obs != nullptr; obs = obs->next)
      count++;
    return count;
  }
  NetCoapResource *first_observable_resource() {
    for (auto &res : this->resources_) {
      if (res.observable && res.entity != nullptr)
        return &res;
    }
    return nullptr;
  }
  void trigger_notify(NetCoapResource *resource, const uint8_t *payload, size_t len) {
    this->notify_observers_(resource, payload, len);
  }

  std::vector<uint8_t> last_sent;

 protected:
#ifdef USE_COAP_OSCORE
  // Notifications can save the sequence number
  ScopedPrefDir pref_dir_;
#endif

  void send_response(const uint8_t *buf, size_t len, const sockaddr_storage * /*peer*/) override {
    this->last_sent.assign(buf, buf + len);
  }
};

// ---------------------------------------------------------------------------
// Packets
// ---------------------------------------------------------------------------

inline sockaddr_storage make_peer(uint16_t port = 12345) {
  sockaddr_storage ss{};
  auto &p = reinterpret_cast<sockaddr_in6 &>(ss);
  p.sin6_family = AF_INET6;
  p.sin6_port = htons(port);
  inet_pton(AF_INET6, "::1", &p.sin6_addr);
  return ss;
}

inline sockaddr_storage make_peer_ipv4(const char *addr = "127.0.0.1", uint16_t port = 12345) {
  sockaddr_storage ss{};
  auto &p = reinterpret_cast<sockaddr_in &>(ss);
  p.sin_family = AF_INET;
  p.sin_port = htons(port);
  inet_pton(AF_INET, addr, &p.sin_addr);
  return ss;
}

// Appends Uri-Path options (11) for each '/'-separated segment; segments must be shorter than 13 bytes
inline void append_uri_path(std::vector<uint8_t> &pkt, const char *path, uint16_t last_opt) {
  std::string p(path);
  size_t start = 0;
  while (start <= p.size()) {
    size_t slash = p.find('/', start);
    if (slash == std::string::npos)
      slash = p.size();
    std::string seg = p.substr(start, slash - start);
    if (!seg.empty()) {
      uint16_t delta = 11 - last_opt;
      last_opt = 11;
      pkt.push_back(static_cast<uint8_t>((delta << 4) | (seg.size() & 0x0F)));
      for (char c : seg)
        pkt.push_back(static_cast<uint8_t>(c));
    }
    start = slash + 1;
  }
}

// GET with token 0xAB; type 1 = NON, 0 = CON. observe < 0 leaves the Observe option out.
inline std::vector<uint8_t> make_coap_get(const char *path, uint16_t msg_id = 1, int observe = -1, uint8_t type = 1) {
  std::vector<uint8_t> pkt{static_cast<uint8_t>(0x41 | (type << 4)), 0x01, static_cast<uint8_t>(msg_id >> 8),
                           static_cast<uint8_t>(msg_id & 0xFF), 0xAB};
  uint16_t last_opt = 0;
  if (observe >= 0) {
    pkt.push_back(0x61);  // Observe (6), length 1
    pkt.push_back(static_cast<uint8_t>(observe));
    last_opt = 6;
  }
  append_uri_path(pkt, path, last_opt);
  return pkt;
}

// NON POST with token 0xCD
inline std::vector<uint8_t> make_coap_post(const char *path, const uint8_t *payload, size_t payload_len,
                                           uint16_t msg_id = 2) {
  std::vector<uint8_t> pkt{0x51, 0x02, static_cast<uint8_t>(msg_id >> 8), static_cast<uint8_t>(msg_id & 0xFF), 0xCD};
  append_uri_path(pkt, path, 0);
  pkt.push_back(0xFF);
  pkt.insert(pkt.end(), payload, payload + payload_len);
  return pkt;
}

inline std::vector<uint8_t> make_coap_ack(uint16_t msg_id) {
  return {0x60, 0x00, static_cast<uint8_t>(msg_id >> 8), static_cast<uint8_t>(msg_id & 0xFF)};
}

inline std::vector<uint8_t> make_coap_rst(uint16_t msg_id) {
  return {0x70, 0x00, static_cast<uint8_t>(msg_id >> 8), static_cast<uint8_t>(msg_id & 0xFF)};
}

inline uint16_t message_id(const std::vector<uint8_t> &pkt) { return static_cast<uint16_t>((pkt[2] << 8) | pkt[3]); }

// Bytes after the 0xFF payload marker, walking the options so a 0xFF option value is not mistaken for it
inline std::string payload_of(const std::vector<uint8_t> &pkt) {
  if (pkt.size() < 4)
    return {};
  size_t pos = 4 + (pkt[0] & 0x0F);
  while (pos < pkt.size()) {
    uint8_t b = pkt[pos++];
    if (b == 0xFF)
      return std::string(pkt.begin() + pos, pkt.end());
    uint16_t delta = (b >> 4) & 0x0F;
    uint16_t len = b & 0x0F;
    if (delta == 13) {
      pos += 1;
    } else if (delta == 14) {
      pos += 2;
    }
    if (len == 13) {
      len = pkt[pos++] + 13;
    } else if (len == 14) {
      len = static_cast<uint16_t>(((pkt[pos] << 8) | pkt[pos + 1]) + 269);
      pos += 2;
    }
    pos += len;
  }
  return {};
}

}  // namespace esphome::coap_server::testing
