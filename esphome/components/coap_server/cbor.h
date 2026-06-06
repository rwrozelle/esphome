// Minimal CBOR encoder and parser for coap_server.
// Implements the subset of RFC 8949 used by coap_server.
// API is source-compatible with the intel/tinycbor subset used here.

#pragma once
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace esphome::coap_server {

// RFC 8949 §3: error conditions
enum CborError {
  CBOR_NO_ERROR = 0,
  CBOR_ERROR_UNKNOWN_TYPE,
  CBOR_ERROR_UNEXPECTED_EOF,
  CBOR_ERROR_ILLEGAL_TYPE,
  CBOR_ERROR_OUT_OF_MEMORY,
};

// ---------------------------------------------------------------------------
// Encoder
// ---------------------------------------------------------------------------

struct CborEncoder {
  uint8_t *ptr;
  const uint8_t *end;
};

// Write the RFC 8949 §3 initial byte + argument for major type mt and value v.
static inline CborError cbor_write_head(uint8_t mt, uint64_t v, CborEncoder *enc) {
  uint8_t b = (uint8_t) (mt << 5);
  if (v <= 23) {
    if (enc->ptr >= enc->end)
      return CBOR_ERROR_OUT_OF_MEMORY;
    *enc->ptr++ = b | (uint8_t) v;
  } else if (v <= 0xFFu) {
    if (enc->ptr + 2 > enc->end)
      return CBOR_ERROR_OUT_OF_MEMORY;
    *enc->ptr++ = b | 24;
    *enc->ptr++ = (uint8_t) v;
  } else if (v <= 0xFFFFu) {
    if (enc->ptr + 3 > enc->end)
      return CBOR_ERROR_OUT_OF_MEMORY;
    *enc->ptr++ = b | 25;
    *enc->ptr++ = (uint8_t) (v >> 8);
    *enc->ptr++ = (uint8_t) v;
  } else if (v <= 0xFFFFFFFFu) {
    if (enc->ptr + 5 > enc->end)
      return CBOR_ERROR_OUT_OF_MEMORY;
    *enc->ptr++ = b | 26;
    *enc->ptr++ = (uint8_t) (v >> 24);
    *enc->ptr++ = (uint8_t) (v >> 16);
    *enc->ptr++ = (uint8_t) (v >> 8);
    *enc->ptr++ = (uint8_t) v;
  } else {
    if (enc->ptr + 9 > enc->end)
      return CBOR_ERROR_OUT_OF_MEMORY;
    *enc->ptr++ = b | 27;
    *enc->ptr++ = (uint8_t) (v >> 56);
    *enc->ptr++ = (uint8_t) (v >> 48);
    *enc->ptr++ = (uint8_t) (v >> 40);
    *enc->ptr++ = (uint8_t) (v >> 32);
    *enc->ptr++ = (uint8_t) (v >> 24);
    *enc->ptr++ = (uint8_t) (v >> 16);
    *enc->ptr++ = (uint8_t) (v >> 8);
    *enc->ptr++ = (uint8_t) v;
  }
  return CBOR_NO_ERROR;
}

static inline void cbor_encoder_init(CborEncoder *enc, uint8_t *buf, size_t size, int flags) {
  (void) flags;
  enc->ptr = buf;
  enc->end = buf + size;
}

// RFC 8949 §3.1 major type 0: unsigned integer
static inline CborError cbor_encode_uint(CborEncoder *enc, uint64_t v) { return cbor_write_head(0, v, enc); }

// RFC 8949 §3.1 major types 0/1: non-negative uses type 0, negative uses type 1 (n = -1 - value)
static inline CborError cbor_encode_int(CborEncoder *enc, int64_t v) {
  if (v >= 0)
    return cbor_write_head(0, (uint64_t) v, enc);
  return cbor_write_head(1, (uint64_t) (-1 - v), enc);
}

// RFC 8949 §3.3 major type 7: false=0xF4, true=0xF5
static inline CborError cbor_encode_boolean(CborEncoder *enc, bool v) {
  if (enc->ptr >= enc->end)
    return CBOR_ERROR_OUT_OF_MEMORY;
  *enc->ptr++ = v ? 0xF5 : 0xF4;
  return CBOR_NO_ERROR;
}

// RFC 8949 §3.3 major type 7, additional 22: null (0xF6)
static inline CborError cbor_encode_null(CborEncoder *enc) {
  if (enc->ptr >= enc->end)
    return CBOR_ERROR_OUT_OF_MEMORY;
  *enc->ptr++ = 0xF6;
  return CBOR_NO_ERROR;
}

// RFC 8949 §3.3 major type 7, additional 26: IEEE 754 single-precision float (0xFA + 4 bytes)
static inline CborError cbor_encode_float(CborEncoder *enc, float v) {
  if (enc->ptr + 5 > enc->end)
    return CBOR_ERROR_OUT_OF_MEMORY;
  uint32_t bits;
  memcpy(&bits, &v, 4);
  *enc->ptr++ = 0xFA;
  *enc->ptr++ = (uint8_t) (bits >> 24);
  *enc->ptr++ = (uint8_t) (bits >> 16);
  *enc->ptr++ = (uint8_t) (bits >> 8);
  *enc->ptr++ = (uint8_t) bits;
  return CBOR_NO_ERROR;
}

// RFC 8949 §3.1 major type 2: byte string
static inline CborError cbor_encode_byte_string(CborEncoder *enc, const uint8_t *data, size_t len) {
  CborError err = cbor_write_head(2, (uint64_t) len, enc);
  if (err != CBOR_NO_ERROR)
    return err;
  if (enc->ptr + len > enc->end)
    return CBOR_ERROR_OUT_OF_MEMORY;
  memcpy(enc->ptr, data, len);
  enc->ptr += len;
  return CBOR_NO_ERROR;
}

// RFC 8949 §3.1 major type 3: text string
static inline CborError cbor_encode_text_string(CborEncoder *enc, const char *str, size_t len) {
  CborError err = cbor_write_head(3, (uint64_t) len, enc);
  if (err != CBOR_NO_ERROR)
    return err;
  if (enc->ptr + len > enc->end)
    return CBOR_ERROR_OUT_OF_MEMORY;
  memcpy(enc->ptr, str, len);
  enc->ptr += len;
  return CBOR_NO_ERROR;
}

static inline CborError cbor_encode_text_stringz(CborEncoder *enc, const char *str) {
  return cbor_encode_text_string(enc, str, strlen(str));
}

// RFC 8949 §3.1 major type 5: map; count = number of key-value pairs
static inline CborError cbor_encoder_create_map(CborEncoder *enc, CborEncoder *child, size_t count) {
  CborError err = cbor_write_head(5, (uint64_t) count, enc);
  if (err != CBOR_NO_ERROR)
    return err;
  child->ptr = enc->ptr;
  child->end = enc->end;
  return CBOR_NO_ERROR;
}

// RFC 8949 §3.1 major type 4: array
static inline CborError cbor_encoder_create_array(CborEncoder *enc, CborEncoder *child, size_t count) {
  CborError err = cbor_write_head(4, (uint64_t) count, enc);
  if (err != CBOR_NO_ERROR)
    return err;
  child->ptr = enc->ptr;
  child->end = enc->end;
  return CBOR_NO_ERROR;
}

// Propagate child's write position back to parent.
static inline CborError cbor_encoder_close_container(CborEncoder *enc, const CborEncoder *child) {
  enc->ptr = child->ptr;
  return CBOR_NO_ERROR;
}

static inline size_t cbor_encoder_get_buffer_size(const CborEncoder *enc, const uint8_t *buf) {
  return (size_t) (enc->ptr - buf);
}

// ---------------------------------------------------------------------------
// Parser
// ---------------------------------------------------------------------------

struct CborParser {
  const uint8_t *end;
};

struct CborValue {
  const CborParser *parser;
  const uint8_t *ptr;
  size_t remaining;  // items left to read in this container
};

// Implemented in cbor.cpp
CborError cbor_parser_init(const uint8_t *buf, size_t len, uint32_t flags, CborParser *parser, CborValue *value);
CborError cbor_value_enter_container(const CborValue *value, CborValue *child);
CborError cbor_value_advance(CborValue *value);
CborError cbor_value_get_int(const CborValue *value, int *result);
CborError cbor_value_get_int64(const CborValue *value, int64_t *result);
CborError cbor_value_get_boolean(const CborValue *value, bool *result);
CborError cbor_value_get_double(const CborValue *value, double *result);

// RFC 8949 §3.1: major type 5 = map
static inline bool cbor_value_is_map(const CborValue *value) {
  return value->ptr < value->parser->end && (*value->ptr >> 5) == 5;
}

static inline bool cbor_value_at_end(const CborValue *value) {
  return value->remaining == 0 || value->ptr >= value->parser->end;
}

// RFC 8949 §3.1: major types 0 and 1 are integers
static inline bool cbor_value_is_integer(const CborValue *value) {
  if (value->ptr >= value->parser->end)
    return false;
  uint8_t mt = (uint8_t) (*value->ptr >> 5);
  return mt == 0 || mt == 1;
}

// RFC 8949 §3.3: false=0xF4, true=0xF5
static inline bool cbor_value_is_boolean(const CborValue *value) {
  return value->ptr < value->parser->end && (*value->ptr == 0xF4 || *value->ptr == 0xF5);
}

// RFC 8949 §3.3: 0xFA = single-precision float (major type 7, additional 26)
static inline bool cbor_value_is_float(const CborValue *value) {
  return value->ptr < value->parser->end && *value->ptr == 0xFA;
}

// RFC 8949 §3.3: 0xFB = double-precision float (major type 7, additional 27)
static inline bool cbor_value_is_double(const CborValue *value) {
  return value->ptr < value->parser->end && *value->ptr == 0xFB;
}

// RFC 8949 §3.3: 0xF9 = half-precision float (major type 7, additional 25)
static inline bool cbor_value_is_half_float(const CborValue *value) {
  return value->ptr < value->parser->end && *value->ptr == 0xF9;
}

}  // namespace esphome::coap_server
