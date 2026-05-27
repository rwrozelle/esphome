#include "coap_server.h"
#include "esphome/core/application.h"
#include "esphome/core/log.h"
#include "cbor.h"

#ifndef CBOR_CHECK
#define CBOR_CHECK(expr) \
  do { \
    CborError cbor_err_ = (expr); \
    if (cbor_err_ != CborNoError) \
      return cbor_err_; \
  } while (0)
#endif

namespace esphome::coap_server {

static const char *const TAG = "coap_server";

// SenML CBOR integer key assignments (RFC 8428):
//   1 = unit (u), 2 = float value (v), 3 = string value (vs), 4 = boolean value (vb)

#ifdef USE_SENSOR
static CborError encode_entity(uint8_t *buffer, size_t buf_len, size_t *encoded_len, sensor::Sensor *entity) {
  CborEncoder encoder, map_encoder;
  const auto uom_ref = entity->get_unit_of_measurement_ref();
  const bool has_uom = uom_ref.size() > 0;
  const bool is_nan = std::isnan(entity->state);
  cbor_encoder_init(&encoder, buffer, buf_len, 0);
  CBOR_CHECK(cbor_encoder_create_map(&encoder, &map_encoder, has_uom ? 2 : 1));
  if (is_nan) {
    CBOR_CHECK(cbor_encode_int(&map_encoder, 3));  // vs
    CBOR_CHECK(cbor_encode_text_stringz(&map_encoder, "NA"));
  } else {
    CBOR_CHECK(cbor_encode_int(&map_encoder, 2));  // v
    CBOR_CHECK(cbor_encode_float(&map_encoder, entity->state));
  }
  if (has_uom) {
    CBOR_CHECK(cbor_encode_int(&map_encoder, 1));  // u
    CBOR_CHECK(cbor_encode_text_string(&map_encoder, uom_ref.c_str(), uom_ref.size()));
  }
  CBOR_CHECK(cbor_encoder_close_container(&encoder, &map_encoder));
  *encoded_len = cbor_encoder_get_buffer_size(&encoder, buffer);
  return CborNoError;
}
#endif  // USE_SENSOR

#if defined(USE_SWITCH) || defined(USE_BINARY_SENSOR)
static CborError encode_bool_entity(uint8_t *buffer, size_t buf_len, size_t *encoded_len, bool state) {
  CborEncoder encoder, map_encoder;
  cbor_encoder_init(&encoder, buffer, buf_len, 0);
  CBOR_CHECK(cbor_encoder_create_map(&encoder, &map_encoder, 1));
  CBOR_CHECK(cbor_encode_int(&map_encoder, 4));  // vb
  CBOR_CHECK(cbor_encode_boolean(&map_encoder, state));
  CBOR_CHECK(cbor_encoder_close_container(&encoder, &map_encoder));
  *encoded_len = cbor_encoder_get_buffer_size(&encoder, buffer);
  return CborNoError;
}
#endif

#ifdef USE_SWITCH
static CborError encode_entity(uint8_t *buffer, size_t buf_len, size_t *encoded_len, switch_::Switch *entity) {
  return encode_bool_entity(buffer, buf_len, encoded_len, entity->state);
}
#endif  // USE_SWITCH

#ifdef USE_BINARY_SENSOR
static CborError encode_entity(uint8_t *buffer, size_t buf_len, size_t *encoded_len,
                               binary_sensor::BinarySensor *entity) {
  return encode_bool_entity(buffer, buf_len, encoded_len, entity->state);
}
#endif  // USE_BINARY_SENSOR

#ifdef USE_TEXT_SENSOR
static CborError encode_entity(uint8_t *buffer, size_t buf_len, size_t *encoded_len, text_sensor::TextSensor *entity) {
  CborEncoder encoder, map_encoder;
  cbor_encoder_init(&encoder, buffer, buf_len, 0);
  CBOR_CHECK(cbor_encoder_create_map(&encoder, &map_encoder, 1));
  CBOR_CHECK(cbor_encode_int(&map_encoder, 3));  // vs
  if (entity->state.empty()) {
    CBOR_CHECK(cbor_encode_text_stringz(&map_encoder, "NA"));
  } else {
    CBOR_CHECK(cbor_encode_text_string(&map_encoder, entity->state.c_str(), entity->state.size()));
  }
  CBOR_CHECK(cbor_encoder_close_container(&encoder, &map_encoder));
  *encoded_len = cbor_encoder_get_buffer_size(&encoder, buffer);
  return CborNoError;
}
#endif  // USE_TEXT_SENSOR

#ifdef USE_NUMBER
static CborError encode_entity(uint8_t *buffer, size_t buf_len, size_t *encoded_len, number::Number *entity) {
  CborEncoder encoder, map_encoder;
  const bool is_nan = std::isnan(entity->state);
  cbor_encoder_init(&encoder, buffer, buf_len, 0);
  CBOR_CHECK(cbor_encoder_create_map(&encoder, &map_encoder, 1));
  if (is_nan) {
    CBOR_CHECK(cbor_encode_int(&map_encoder, 3));  // vs
    CBOR_CHECK(cbor_encode_text_stringz(&map_encoder, "NA"));
  } else {
    CBOR_CHECK(cbor_encode_int(&map_encoder, 2));  // v
    CBOR_CHECK(cbor_encode_float(&map_encoder, entity->state));
  }
  CBOR_CHECK(cbor_encoder_close_container(&encoder, &map_encoder));
  *encoded_len = cbor_encoder_get_buffer_size(&encoder, buffer);
  return CborNoError;
}
#endif  // USE_NUMBER

#ifdef USE_LOCK
static CborError encode_entity(uint8_t *buffer, size_t buf_len, size_t *encoded_len, lock::Lock *entity) {
  CborEncoder encoder, map_encoder;
  cbor_encoder_init(&encoder, buffer, buf_len, 0);
  CBOR_CHECK(cbor_encoder_create_map(&encoder, &map_encoder, 1));
  CBOR_CHECK(cbor_encode_int(&map_encoder, 2));  // v (LockState as uint)
  CBOR_CHECK(cbor_encode_uint(&map_encoder, static_cast<uint8_t>(entity->state)));
  CBOR_CHECK(cbor_encoder_close_container(&encoder, &map_encoder));
  *encoded_len = cbor_encoder_get_buffer_size(&encoder, buffer);
  return CborNoError;
}
#endif  // USE_LOCK

#ifdef USE_VALVE
static CborError encode_entity(uint8_t *buffer, size_t buf_len, size_t *encoded_len, valve::Valve *entity) {
  CborEncoder encoder, map_encoder;
  cbor_encoder_init(&encoder, buffer, buf_len, 0);
  CBOR_CHECK(cbor_encoder_create_map(&encoder, &map_encoder, 1));
  CBOR_CHECK(cbor_encode_int(&map_encoder, 2));  // v (position 0.0-1.0)
  CBOR_CHECK(cbor_encode_float(&map_encoder, entity->position));
  CBOR_CHECK(cbor_encoder_close_container(&encoder, &map_encoder));
  *encoded_len = cbor_encoder_get_buffer_size(&encoder, buffer);
  return CborNoError;
}
#endif  // USE_VALVE

static CborError encode_entity(uint8_t *buffer, size_t buf_len, size_t *encoded_len, EntityBase *entity) {
  CborEncoder encoder, map_encoder;
  cbor_encoder_init(&encoder, buffer, buf_len, 0);
  CBOR_CHECK(cbor_encoder_create_map(&encoder, &map_encoder, 0));
  CBOR_CHECK(cbor_encoder_close_container(&encoder, &map_encoder));
  *encoded_len = cbor_encoder_get_buffer_size(&encoder, buffer);
  return CborNoError;
}

#ifdef USE_OPENTHREAD
size_t CoapServer::cbor_output_(uint8_t *buffer, ehCoapResource *resource) {
  size_t encoded_len = 0;
  CborError err = CborNoError;
  switch (resource->type) {
#ifdef USE_SENSOR
    case EntityType::ENTITYTYPE_SENSOR:
      err = encode_entity(buffer, COAP_PAYLOAD_MAX_SIZE, &encoded_len, static_cast<sensor::Sensor *>(resource->entity));
      break;
#endif
#ifdef USE_SWITCH
    case EntityType::ENTITYTYPE_SWITCH:
      err =
          encode_entity(buffer, COAP_PAYLOAD_MAX_SIZE, &encoded_len, static_cast<switch_::Switch *>(resource->entity));
      break;
#endif
#ifdef USE_BINARY_SENSOR
    case EntityType::ENTITYTYPE_BINARY_SENSOR:
      err = encode_entity(buffer, COAP_PAYLOAD_MAX_SIZE, &encoded_len,
                          static_cast<binary_sensor::BinarySensor *>(resource->entity));
      break;
#endif
#ifdef USE_TEXT_SENSOR
    case EntityType::ENTITYTYPE_TEXT_SENSOR:
      err = encode_entity(buffer, COAP_PAYLOAD_MAX_SIZE, &encoded_len,
                          static_cast<text_sensor::TextSensor *>(resource->entity));
      break;
#endif
#ifdef USE_NUMBER
    case EntityType::ENTITYTYPE_NUMBER:
      err = encode_entity(buffer, COAP_PAYLOAD_MAX_SIZE, &encoded_len, static_cast<number::Number *>(resource->entity));
      break;
#endif
#ifdef USE_LOCK
    case EntityType::ENTITYTYPE_LOCK:
      err = encode_entity(buffer, COAP_PAYLOAD_MAX_SIZE, &encoded_len, static_cast<lock::Lock *>(resource->entity));
      break;
#endif
#ifdef USE_VALVE
    case EntityType::ENTITYTYPE_VALVE:
      err = encode_entity(buffer, COAP_PAYLOAD_MAX_SIZE, &encoded_len, static_cast<valve::Valve *>(resource->entity));
      break;
#endif
    case EntityType::ENTITYTYPE_BUTTON:
    case EntityType::ENTITYTYPE_DEVICE:
    case EntityType::ENTITYTYPE_LOG:
      return 0;
    case EntityType::ENTITYTYPE_UNKNOWN:
    default:
      err = encode_entity(buffer, COAP_PAYLOAD_MAX_SIZE, &encoded_len, resource->entity);
      break;
  }
  if (err != CborNoError) {
    ESP_LOGE(TAG, "tinycbor error: %d", err);
    return 0;
  }
  return encoded_len;
}
#endif  // USE_OPENTHREAD

static CborError encode_device_info_impl(uint8_t *buf, size_t buf_len, size_t *encoded_len, CoapServer *server) {
  CborEncoder enc, map;
  cbor_encoder_init(&enc, buf, buf_len, 0);
  const StringRef &friendly = App.get_friendly_name();
  const bool has_friendly = friendly.size() > 0;
#ifdef USE_DEVICES
  const uint8_t device_count = (uint8_t) App.get_devices().size();
#else
  const uint8_t device_count = 0;
#endif
#ifdef USE_AREAS
  const uint8_t area_count = (uint8_t) App.get_areas().size();
#else
  const uint8_t area_count = 0;
#endif
  CBOR_CHECK(cbor_encoder_create_map(&enc, &map,
                                     (has_friendly ? 11 : 10) + (area_count > 0 ? 1 : 0) + (device_count > 0 ? 1 : 0)));
  CBOR_CHECK(cbor_encode_text_stringz(&map, "name"));
  const StringRef &name = App.get_name();
  CBOR_CHECK(cbor_encode_text_string(&map, name.c_str(), name.size()));
  CBOR_CHECK(cbor_encode_text_stringz(&map, "version"));
  CBOR_CHECK(cbor_encode_text_stringz(&map, ESPHOME_VERSION));
  char build_time[Application::BUILD_TIME_STR_SIZE];
  App.get_build_time_string(build_time);
  CBOR_CHECK(cbor_encode_text_stringz(&map, "build_time"));
  CBOR_CHECK(cbor_encode_text_stringz(&map, build_time));
  CBOR_CHECK(cbor_encode_text_stringz(&map, "model"));
  CBOR_CHECK(cbor_encode_text_stringz(&map, ESPHOME_BOARD));
  CBOR_CHECK(cbor_encode_text_stringz(&map, "ping_interval"));
  CBOR_CHECK(cbor_encode_uint(&map, server->get_server_ping_interval() / 1000));
  CBOR_CHECK(cbor_encode_text_stringz(&map, "ping_timeout"));
  CBOR_CHECK(cbor_encode_uint(&map, (uint32_t) std::max((server->get_server_ping_interval() / 1000.0f *
                                                         server->get_server_ping_timeout_ratio()),
                                                        (float) 1.0f)));
  CBOR_CHECK(cbor_encode_text_stringz(&map, "ping_retry"));
  CBOR_CHECK(cbor_encode_uint(&map, server->get_server_ping_retry()));
  CBOR_CHECK(cbor_encode_text_stringz(&map, "oscore"));
#ifdef USE_COAP_OSCORE
  CBOR_CHECK(cbor_encode_boolean(&map, true));
#else
  CBOR_CHECK(cbor_encode_boolean(&map, false));
#endif
  CBOR_CHECK(cbor_encode_text_stringz(&map, "subscription_confirm"));
  CBOR_CHECK(cbor_encode_boolean(&map, server->get_subscription_confirm()));
  CBOR_CHECK(cbor_encode_text_stringz(&map, "observe_retry"));
  CBOR_CHECK(cbor_encode_uint(&map, server->get_observe_retry()));
  if (has_friendly) {
    CBOR_CHECK(cbor_encode_text_stringz(&map, "friendly_name"));
    CBOR_CHECK(cbor_encode_text_string(&map, friendly.c_str(), friendly.size()));
  }
#ifdef USE_AREAS
  if (area_count > 0) {
    CBOR_CHECK(cbor_encode_text_stringz(&map, "areas"));
    CborEncoder areas_arr;
    CBOR_CHECK(cbor_encoder_create_array(&map, &areas_arr, area_count));
    for (Area *area : App.get_areas()) {
      CborEncoder area_map;
      CBOR_CHECK(cbor_encoder_create_map(&areas_arr, &area_map, 1));
      CBOR_CHECK(cbor_encode_text_stringz(&area_map, "name"));
      CBOR_CHECK(cbor_encode_text_stringz(&area_map, area->get_name()));
      CBOR_CHECK(cbor_encoder_close_container(&areas_arr, &area_map));
    }
    CBOR_CHECK(cbor_encoder_close_container(&map, &areas_arr));
  }
#endif
#ifdef USE_DEVICES
  if (device_count > 0) {
    CBOR_CHECK(cbor_encode_text_stringz(&map, "devices"));
    CborEncoder arr;
    CBOR_CHECK(cbor_encoder_create_array(&map, &arr, device_count));
    for (Device *dev : App.get_devices()) {
      uint8_t area_idx = 0;
#ifdef USE_AREAS
      {
        uint8_t idx = 1;
        for (Area *area : App.get_areas()) {
          if (area->get_area_id() == dev->get_area_id()) {
            area_idx = idx;
            break;
          }
          idx++;
        }
      }
#endif
      CborEncoder dev_map;
      CBOR_CHECK(cbor_encoder_create_map(&arr, &dev_map, area_idx > 0 ? 2 : 1));
      CBOR_CHECK(cbor_encode_text_stringz(&dev_map, "name"));
      CBOR_CHECK(cbor_encode_text_stringz(&dev_map, dev->get_name()));
      if (area_idx > 0) {
        CBOR_CHECK(cbor_encode_text_stringz(&dev_map, "area"));
        CBOR_CHECK(cbor_encode_uint(&dev_map, area_idx));
      }
      CBOR_CHECK(cbor_encoder_close_container(&arr, &dev_map));
    }
    CBOR_CHECK(cbor_encoder_close_container(&map, &arr));
  }
#endif
  CBOR_CHECK(cbor_encoder_close_container(&enc, &map));
  *encoded_len = cbor_encoder_get_buffer_size(&enc, buf);
  return CborNoError;
}

size_t CoapServer::encode_device_info_(uint8_t *buf, size_t buf_len, CoapServer *server) {
  size_t encoded_len = 0;
  CborError err = encode_device_info_impl(buf, buf_len, &encoded_len, server);
  if (err != CborNoError) {
    ESP_LOGE(TAG, "encode_device_info error: %d", err);
    return 0;
  }
  return encoded_len;
}

}  // namespace esphome::coap_server
