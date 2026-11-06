#pragma once
// Stand-in for OpenThread's IPv6 address API, as used by the coap_server OpenThread transport.
#include <cstdint>
#include <cstring>

typedef struct {
  union {
    uint8_t m8[16];
    uint16_t m16[8];
    uint32_t m32[4];
  } mFields;
} otIp6Address;

static constexpr uint16_t OT_IP6_ADDRESS_STRING_SIZE = 40;

#ifdef __cplusplus
extern "C" {
#endif
bool otIp6IsAddressEqual(const otIp6Address *a, const otIp6Address *b);
void otIp6AddressToString(const otIp6Address *addr, char *buf, uint16_t size);
#ifdef __cplusplus
}
#endif
