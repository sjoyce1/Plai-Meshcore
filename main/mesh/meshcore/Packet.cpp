#include "Packet.h"
#include <string.h>
#include "mbedtls/sha256.h"

namespace mesh {

Packet::Packet() {
  header = 0;
  path_len = 0;
  payload_len = 0;
}

int Packet::getRawLength() const {
  return 2 + path_len + payload_len + (hasTransportCodes() ? 4 : 0);
}

void Packet::calculatePacketHash(uint8_t* dest_hash) const {
  mbedtls_sha256_context ctx;
  mbedtls_sha256_init(&ctx);
  mbedtls_sha256_starts(&ctx, 0); // 0 = SHA-256
  
  uint8_t t = getPayloadType();
  mbedtls_sha256_update(&ctx, &t, 1);
  if (t == PAYLOAD_TYPE_TRACE) {
    mbedtls_sha256_update(&ctx, (const unsigned char*)&path_len, sizeof(path_len));
  }
  mbedtls_sha256_update(&ctx, (const unsigned char*)payload, payload_len);
  
  uint8_t full_hash[32];
  mbedtls_sha256_finish(&ctx, full_hash);
  mbedtls_sha256_free(&ctx);
  
  memcpy(dest_hash, full_hash, MAX_HASH_SIZE);
}

uint8_t Packet::writeTo(uint8_t dest[]) const {
  uint8_t i = 0;
  dest[i++] = header;
  if (hasTransportCodes()) {
    memcpy(&dest[i], &transport_codes[0], 2); i += 2;
    memcpy(&dest[i], &transport_codes[1], 2); i += 2;
  }
  dest[i++] = path_len;
  memcpy(&dest[i], path, path_len); i += path_len;
  memcpy(&dest[i], payload, payload_len); i += payload_len;
  return i;
}

bool Packet::readFrom(const uint8_t src[], uint8_t len) {
  uint8_t i = 0;
  header = src[i++];
  if (hasTransportCodes()) {
    memcpy(&transport_codes[0], &src[i], 2); i += 2;
    memcpy(&transport_codes[1], &src[i], 2); i += 2;
  } else {
    transport_codes[0] = transport_codes[1] = 0;
  }
  path_len = src[i++];
  if (path_len > sizeof(path)) return false;   // bad encoding
  memcpy(path, &src[i], path_len); i += path_len;
  if (i >= len) return false;   // bad encoding
  payload_len = len - i;
  if (payload_len > sizeof(payload)) return false;  // bad encoding
  memcpy(payload, &src[i], payload_len); //i += payload_len;
  return true;   // success
}

}