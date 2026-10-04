#include "ObfuscationUtils.h"

#include <Logging.h>
#include <esp_mac.h>
#include <mbedtls/base64.h>

#include <cstring>
#include <limits>

namespace obfuscation {

namespace {
constexpr size_t HW_KEY_LEN = 6;

struct HwKey {
  uint8_t bytes[HW_KEY_LEN] = {};
  HwKey() {
    if (esp_efuse_mac_get_default(bytes) != ESP_OK) {
      LOG_ERR("OBF", "Using zero credential key because device MAC is unavailable");
    }
  }
};

const uint8_t* getHwKey() {
  // 函数内静态初始化由 C++ 保证线程安全。
  static const HwKey key;
  return key.bytes;
}

// base64 编码（无换行，标准字母表）。
std::string base64Encode(const uint8_t* data, size_t len) {
  size_t outLen = 0;
  if (mbedtls_base64_encode(nullptr, 0, &outLen, data, len) != MBEDTLS_ERR_BASE64_BUFFER_TOO_SMALL && outLen == 0) {
    return std::string();
  }
  std::string out(outLen, '\0');
  if (mbedtls_base64_encode(reinterpret_cast<unsigned char*>(&out[0]), outLen, &outLen, data, len) != 0) {
    return std::string();
  }
  out.resize(outLen);
  return out;
}
}  // namespace

void xorTransform(std::string& data) {
  const uint8_t* key = getHwKey();
  for (size_t i = 0; i < data.size(); i++) {
    data[i] = static_cast<char>(data[i] ^ key[i % HW_KEY_LEN]);
  }
}

void xorTransform(std::string& data, const uint8_t* key, size_t keyLen) {
  if (keyLen == 0 || key == nullptr) return;
  for (size_t i = 0; i < data.size(); i++) {
    data[i] = static_cast<char>(data[i] ^ key[i % keyLen]);
  }
}

std::string obfuscateToBase64(const std::string& plaintext) {
  if (plaintext.empty()) return std::string();
  std::string temp = plaintext;
  xorTransform(temp);
  return base64Encode(reinterpret_cast<const uint8_t*>(temp.data()), temp.size());
}

std::string deobfuscateFromBase64(const char* encoded, bool* ok) {
  return deobfuscateFromBase64(encoded, std::numeric_limits<size_t>::max(), ok, nullptr);
}

std::string deobfuscateFromBase64(const char* encoded, const size_t maxDecodedLength, bool* ok, bool* tooLong) {
  if (tooLong) *tooLong = false;
  if (encoded == nullptr || encoded[0] == '\0') {
    if (ok) *ok = false;
    return std::string();
  }
  if (ok) *ok = true;
  const size_t encodedLen = strlen(encoded);
  size_t decodedLen = 0;
  int ret = mbedtls_base64_decode(nullptr, 0, &decodedLen, reinterpret_cast<const unsigned char*>(encoded), encodedLen);
  if (ret != 0 && ret != MBEDTLS_ERR_BASE64_BUFFER_TOO_SMALL) {
    LOG_ERR("OBF", "Base64 decode size query failed (ret=%d)", ret);
    if (ok) *ok = false;
    return std::string();
  }
  if (decodedLen > maxDecodedLength) {
    if (ok) *ok = false;
    if (tooLong) *tooLong = true;
    return std::string();
  }
  std::string result(decodedLen, '\0');
  ret = mbedtls_base64_decode(reinterpret_cast<unsigned char*>(&result[0]), decodedLen, &decodedLen,
                              reinterpret_cast<const unsigned char*>(encoded), encodedLen);
  if (ret != 0) {
    LOG_ERR("OBF", "Base64 decode failed (ret=%d)", ret);
    if (ok) *ok = false;
    return std::string();
  }
  result.resize(decodedLen);
  xorTransform(result);
  return result;
}

}  // namespace obfuscation
