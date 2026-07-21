#include "Utils.h"
#include "mbedtls/sha256.h"
#include "mbedtls/aes.h"
#include "mbedtls/md.h"
#include <stdio.h>
#include <string.h>

namespace mesh {

uint32_t RNG::nextInt(uint32_t _min, uint32_t _max) {
  uint32_t num;
  random((uint8_t *) &num, sizeof(num));
  return (num % (_max - _min)) + _min;
}

void Utils::sha256(uint8_t *hash, size_t hash_len, const uint8_t* msg, int msg_len) {
  uint8_t full_hash[32];
  mbedtls_sha256(msg, msg_len, full_hash, 0); // 0 = SHA-256
  memcpy(hash, full_hash, hash_len);
}

void Utils::sha256(uint8_t *hash, size_t hash_len, const uint8_t* frag1, int frag1_len, const uint8_t* frag2, int frag2_len) {
  mbedtls_sha256_context ctx;
  mbedtls_sha256_init(&ctx);
  mbedtls_sha256_starts(&ctx, 0);
  mbedtls_sha256_update(&ctx, frag1, frag1_len);
  mbedtls_sha256_update(&ctx, frag2, frag2_len);
  uint8_t full_hash[32];
  mbedtls_sha256_finish(&ctx, full_hash);
  mbedtls_sha256_free(&ctx);
  memcpy(hash, full_hash, hash_len);
}

int Utils::encrypt(const uint8_t* shared_secret, uint8_t* dest, const uint8_t* src, int src_len) {
  mbedtls_aes_context aes;
  mbedtls_aes_init(&aes);
  mbedtls_aes_setkey_enc(&aes, shared_secret, 128); // 128-bit key
  
  uint8_t* dp = dest;
  while (src_len >= 16) {
    mbedtls_aes_crypt_ecb(&aes, MBEDTLS_AES_ENCRYPT, src, dp);
    dp += 16; src += 16; src_len -= 16;
  }
  if (src_len > 0) {  // remaining partial block
    uint8_t tmp[16];
    memset(tmp, 0, 16);
    memcpy(tmp, src, src_len);
    mbedtls_aes_crypt_ecb(&aes, MBEDTLS_AES_ENCRYPT, tmp, dp);
    dp += 16;
  }
  mbedtls_aes_free(&aes);
  return dp - dest;
}

int Utils::decrypt(const uint8_t* shared_secret, uint8_t* dest, const uint8_t* src, int src_len) {
  mbedtls_aes_context aes;
  mbedtls_aes_init(&aes);
  mbedtls_aes_setkey_dec(&aes, shared_secret, 128);
  
  uint8_t* dp = dest;
  const uint8_t* sp = src;
  while (sp - src < src_len) {
    mbedtls_aes_crypt_ecb(&aes, MBEDTLS_AES_DECRYPT, sp, dp);
    dp += 16; sp += 16;
  }
  mbedtls_aes_free(&aes);
  return sp - src;
}

int Utils::encryptThenMAC(const uint8_t* shared_secret, uint8_t* dest, const uint8_t* src, int src_len) {
  int enc_len = encrypt(shared_secret, dest + CIPHER_MAC_SIZE, src, src_len);
  
  uint8_t hmac[32];
  mbedtls_md_context_t md_ctx;
  mbedtls_md_init(&md_ctx);
  mbedtls_md_setup(&md_ctx, mbedtls_md_info_from_type(MBEDTLS_MD_SHA256), 1);
  mbedtls_md_hmac_starts(&md_ctx, shared_secret, PUB_KEY_SIZE);
  mbedtls_md_hmac_update(&md_ctx, dest + CIPHER_MAC_SIZE, enc_len);
  mbedtls_md_hmac_finish(&md_ctx, hmac);
  mbedtls_md_free(&md_ctx);
  
  memcpy(dest, hmac, CIPHER_MAC_SIZE);
  return CIPHER_MAC_SIZE + enc_len;
}

int Utils::MACThenDecrypt(const uint8_t* shared_secret, uint8_t* dest, const uint8_t* src, int src_len) {
  if (src_len <= CIPHER_MAC_SIZE) return 0;
  
  uint8_t hmac[32];
  mbedtls_md_context_t md_ctx;
  mbedtls_md_init(&md_ctx);
  mbedtls_md_setup(&md_ctx, mbedtls_md_info_from_type(MBEDTLS_MD_SHA256), 1);
  mbedtls_md_hmac_starts(&md_ctx, shared_secret, PUB_KEY_SIZE);
  mbedtls_md_hmac_update(&md_ctx, src + CIPHER_MAC_SIZE, src_len - CIPHER_MAC_SIZE);
  mbedtls_md_hmac_finish(&md_ctx, hmac);
  mbedtls_md_free(&md_ctx);
  
  if (memcmp(hmac, src, CIPHER_MAC_SIZE) == 0) {
    return decrypt(shared_secret, dest, src + CIPHER_MAC_SIZE, src_len - CIPHER_MAC_SIZE);
  }
  return 0; // invalid HMAC
}

static const char hex_chars[] = "0123456789ABCDEF";

void Utils::toHex(char* dest, const uint8_t* src, size_t len) {
  while (len > 0) {
    uint8_t b = *src++;
    *dest++ = hex_chars[b >> 4];
    *dest++ = hex_chars[b & 0x0F];
    len--;
  }
  *dest = 0;
}

void Utils::printHex(const uint8_t* src, size_t len) {
  while (len > 0) {
    uint8_t b = *src++;
    printf("%c%c", hex_chars[b >> 4], hex_chars[b & 0x0F]);
    len--;
  }
}

static uint8_t hexVal(char c) {
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= '0' && c <= '9') return c - '0';
  return 0;
}

bool Utils::isHexChar(char c) {
  return c == '0' || hexVal(c) > 0;
}

bool Utils::fromHex(uint8_t* dest, int dest_size, const char *src_hex) {
  int len = strlen(src_hex);
  if (len != dest_size*2) return false;

  uint8_t* dp = dest;
  while (dp - dest < dest_size) {
    char ch = *src_hex++;
    char cl = *src_hex++;
    *dp++ = (hexVal(ch) << 4) | hexVal(cl);
  }
  return true;
}

int Utils::parseTextParts(char* text, const char* parts[], int max_num, char separator) {
  int num = 0;
  char* sp = text;
  while (*sp && num < max_num) {
    parts[num++] = sp;
    while (*sp && *sp != separator) sp++;
    if (*sp) {
       *sp++ = 0;
    }
  }
  while (*sp && *sp != separator) sp++;
  if (*sp) {
    *sp = 0;
  }
  return num;
}

}
