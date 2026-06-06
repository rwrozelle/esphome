// CBOR parser implementation per RFC 8949.

#include "cbor.h"
#include <cmath>

namespace esphome::coap_server {

// Read the RFC 8949 §3 argument from the initial byte onward, advancing *pp.
// Returns the encoded value (count, length, or integer magnitude) in *val_out.
static CborError read_head(const uint8_t **pp, const uint8_t *end, uint64_t *val_out) {
  const uint8_t *p = *pp;
  if (p >= end)
    return CBOR_ERROR_UNEXPECTED_EOF;
  uint8_t ai = *p++ & 0x1F;
  if (ai <= 23) {
    *val_out = ai;
  } else if (ai == 24) {
    if (p >= end)
      return CBOR_ERROR_UNEXPECTED_EOF;
    *val_out = *p++;
  } else if (ai == 25) {
    if (p + 2 > end)
      return CBOR_ERROR_UNEXPECTED_EOF;
    *val_out = ((uint16_t) p[0] << 8) | p[1];
    p += 2;
  } else if (ai == 26) {
    if (p + 4 > end)
      return CBOR_ERROR_UNEXPECTED_EOF;
    *val_out = ((uint32_t) p[0] << 24) | ((uint32_t) p[1] << 16) | ((uint32_t) p[2] << 8) | p[3];
    p += 4;
  } else if (ai == 27) {
    if (p + 8 > end)
      return CBOR_ERROR_UNEXPECTED_EOF;
    *val_out = ((uint64_t) p[0] << 56) | ((uint64_t) p[1] << 48) | ((uint64_t) p[2] << 40) | ((uint64_t) p[3] << 32) |
               ((uint64_t) p[4] << 24) | ((uint64_t) p[5] << 16) | ((uint64_t) p[6] << 8) | p[7];
    p += 8;
  } else {
    return CBOR_ERROR_UNKNOWN_TYPE;  // indefinite-length or reserved
  }
  *pp = p;
  return CBOR_NO_ERROR;
}

// Skip one complete data item at *pp, including any nested items.
static CborError skip_item(const uint8_t **pp, const uint8_t *end) {
  const uint8_t *p = *pp;
  if (p >= end)
    return CBOR_ERROR_UNEXPECTED_EOF;
  uint8_t mt = (uint8_t) (*p >> 5);
  uint64_t val;
  CborError err = read_head(&p, end, &val);
  if (err != CBOR_NO_ERROR)
    return err;
  switch (mt) {
    case 0:  // unsigned int — argument is the value, fully consumed
    case 1:  // negative int — argument is the magnitude, fully consumed
      break;
    case 2:  // byte string — val payload bytes follow
    case 3:  // text string — val payload bytes follow
      if (p + val > end)
        return CBOR_ERROR_UNEXPECTED_EOF;
      p += val;
      break;
    case 4:  // array — val items follow
      for (uint64_t i = 0; i < val; i++) {
        err = skip_item(&p, end);
        if (err != CBOR_NO_ERROR)
          return err;
      }
      break;
    case 5:  // map — val key-value pairs follow (2*val items)
      for (uint64_t i = 0; i < val * 2; i++) {
        err = skip_item(&p, end);
        if (err != CBOR_NO_ERROR)
          return err;
      }
      break;
    case 7:  // float/simple — additional bytes already consumed by read_head
      break;
    default:
      return CBOR_ERROR_UNKNOWN_TYPE;
  }
  *pp = p;
  return CBOR_NO_ERROR;
}

CborError cbor_parser_init(const uint8_t *buf, size_t len, uint32_t flags, CborParser *parser, CborValue *value) {
  (void) flags;
  parser->end = buf + len;
  value->parser = parser;
  value->ptr = buf;
  value->remaining = 1;
  return CBOR_NO_ERROR;
}

CborError cbor_value_enter_container(const CborValue *value, CborValue *child) {
  const uint8_t *p = value->ptr;
  if (p >= value->parser->end)
    return CBOR_ERROR_UNEXPECTED_EOF;
  uint8_t mt = (uint8_t) (*p >> 5);
  if (mt != 4 && mt != 5)
    return CBOR_ERROR_ILLEGAL_TYPE;
  uint64_t count;
  CborError err = read_head(&p, value->parser->end, &count);
  if (err != CBOR_NO_ERROR)
    return err;
  child->parser = value->parser;
  child->ptr = p;
  // maps have count key-value pairs = 2*count items; arrays have count items
  child->remaining = (mt == 5) ? (size_t) (count * 2) : (size_t) count;
  return CBOR_NO_ERROR;
}

CborError cbor_value_advance(CborValue *value) {
  if (value->remaining == 0)
    return CBOR_ERROR_UNEXPECTED_EOF;
  CborError err = skip_item(&value->ptr, value->parser->end);
  if (err != CBOR_NO_ERROR)
    return err;
  value->remaining--;
  return CBOR_NO_ERROR;
}

CborError cbor_value_get_int64(const CborValue *value, int64_t *result) {
  const uint8_t *p = value->ptr;
  if (p >= value->parser->end)
    return CBOR_ERROR_UNEXPECTED_EOF;
  uint8_t mt = (uint8_t) (*p >> 5);
  if (mt != 0 && mt != 1)
    return CBOR_ERROR_ILLEGAL_TYPE;
  uint64_t v;
  CborError err = read_head(&p, value->parser->end, &v);
  if (err != CBOR_NO_ERROR)
    return err;
  *result = (mt == 0) ? (int64_t) v : (-1 - (int64_t) v);
  return CBOR_NO_ERROR;
}

CborError cbor_value_get_int(const CborValue *value, int *result) {
  int64_t v;
  CborError err = cbor_value_get_int64(value, &v);
  if (err != CBOR_NO_ERROR)
    return err;
  *result = (int) v;
  return CBOR_NO_ERROR;
}

CborError cbor_value_get_boolean(const CborValue *value, bool *result) {
  if (value->ptr >= value->parser->end)
    return CBOR_ERROR_UNEXPECTED_EOF;
  if (*value->ptr == 0xF5) {
    *result = true;
    return CBOR_NO_ERROR;
  }
  if (*value->ptr == 0xF4) {
    *result = false;
    return CBOR_NO_ERROR;
  }
  return CBOR_ERROR_ILLEGAL_TYPE;
}

// RFC 8949 §3.3 and Appendix D of RFC 7049: decode 16-bit half-precision to double.
// exp=0 -> subnormal; exp=31 -> infinity/NaN; otherwise normalised.
static double decode_half_float(uint16_t h) {
  int exp = (h >> 10) & 0x1F;
  int mant = h & 0x3FF;
  double val;
  if (exp == 0) {
    val = std::ldexp((double) mant, -24);
  } else if (exp != 31) {
    val = std::ldexp((double) (mant + 1024), exp - 25);
  } else {
    val = mant == 0 ? (double) INFINITY : (double) NAN;
  }
  return (h & 0x8000) ? -val : val;
}

CborError cbor_value_get_double(const CborValue *value, double *result) {
  const uint8_t *p = value->ptr;
  const uint8_t *end = value->parser->end;
  if (p >= end)
    return CBOR_ERROR_UNEXPECTED_EOF;
  if (*p == 0xF9) {  // half-precision: 0xF9 + 2 bytes
    if (p + 3 > end)
      return CBOR_ERROR_UNEXPECTED_EOF;
    uint16_t bits = ((uint16_t) p[1] << 8) | p[2];
    *result = decode_half_float(bits);
  } else if (*p == 0xFA) {  // single-precision: 0xFA + 4 bytes
    if (p + 5 > end)
      return CBOR_ERROR_UNEXPECTED_EOF;
    uint32_t bits = ((uint32_t) p[1] << 24) | ((uint32_t) p[2] << 16) | ((uint32_t) p[3] << 8) | p[4];
    float f;
    memcpy(&f, &bits, 4);
    *result = (double) f;
  } else if (*p == 0xFB) {  // double-precision: 0xFB + 8 bytes
    if (p + 9 > end)
      return CBOR_ERROR_UNEXPECTED_EOF;
    uint64_t bits = ((uint64_t) p[1] << 56) | ((uint64_t) p[2] << 48) | ((uint64_t) p[3] << 40) |
                    ((uint64_t) p[4] << 32) | ((uint64_t) p[5] << 24) | ((uint64_t) p[6] << 16) |
                    ((uint64_t) p[7] << 8) | p[8];
    memcpy(result, &bits, 8);
  } else {
    return CBOR_ERROR_ILLEGAL_TYPE;
  }
  return CBOR_NO_ERROR;
}

}  // namespace esphome::coap_server
