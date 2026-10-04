#pragma once
// MD5Builder 的最小实现：微信读书的下载分片带 32 字节十六进制 MD5 头，
// 校验时要用到增量 MD5。原版来自 Arduino core；这里直接用 ROM 里的 MD5
// （本 IDF 的 mbedtls 头已私有化，esp_rom_md5 是最省事且不占 flash 的选择），
// 只提供 WeReadWebApi 用到的 begin/add/calculate/toString。
#include <esp_rom_md5.h>

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string>

class MD5Builder {
 public:
  void begin() {
    esp_rom_md5_init(&ctx_);
    started_ = true;
    value_.clear();
  }

  void add(const uint8_t* data, size_t len) {
    if (!started_ || !data || len == 0) return;
    esp_rom_md5_update(&ctx_, data, static_cast<uint32_t>(len));
  }

  void calculate() {
    if (!started_) return;
    uint8_t digest[16];
    esp_rom_md5_final(digest, &ctx_);
    started_ = false;
    char hex[33];
    for (int i = 0; i < 16; i++) snprintf(hex + i * 2, 3, "%02x", digest[i]);
    value_.assign(hex, 32);
  }

  std::string toString() const { return value_; }

 private:
  md5_context_t ctx_{};
  bool started_ = false;
  std::string value_;
};
