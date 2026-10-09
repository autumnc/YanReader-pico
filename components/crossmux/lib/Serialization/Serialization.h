#pragma once
#include <HalStorage.h>

#include <iostream>
#include <utility>

namespace serialization {
constexpr uint32_t MAX_PATH_BYTES = 4096;
constexpr uint32_t MAX_TEXT_BYTES = 16384;

template <typename T>
void writePod(std::ostream& os, const T& value) {
  os.write(reinterpret_cast<const char*>(&value), sizeof(T));
}

template <typename T>
void writePod(HalFile& file, const T& value) {
  file.write(reinterpret_cast<const uint8_t*>(&value), sizeof(T));
}

template <typename T>
[[nodiscard]] bool readPod(std::istream& is, T& value) {
  T next{};
  if (!is.read(reinterpret_cast<char*>(&next), sizeof(T))) return false;
  value = next;
  return true;
}

template <typename T>
[[nodiscard]] bool readPod(HalFile& file, T& value) {
  T next{};
  if (file.read(reinterpret_cast<uint8_t*>(&next), sizeof(T)) != static_cast<int>(sizeof(T))) return false;
  value = next;
  return true;
}

inline void writeString(std::ostream& os, const std::string& s) {
  const uint32_t len = s.size();
  writePod(os, len);
  os.write(s.data(), len);
}

inline void writeString(HalFile& file, const std::string& s) {
  const uint32_t len = s.size();
  writePod(file, len);
  file.write(reinterpret_cast<const uint8_t*>(s.data()), len);
}

[[nodiscard]] inline bool readString(std::istream& is, std::string& s, const uint32_t maxLength) {
  uint32_t len = 0;
  if (!readPod(is, len) || len > maxLength) return false;
  std::string next;
  next.resize(len);
  if (len > 0 && !is.read(next.data(), len)) return false;
  s = std::move(next);
  return true;
}

[[nodiscard]] inline bool readString(HalFile& file, std::string& s, const uint32_t maxLength) {
  uint32_t len = 0;
  if (!readPod(file, len) || len > maxLength) return false;
  // 别在这里问 file.position()/file.size()。HalFile::size() 是 ftell → fseek(END) → ftell →
  // fseek(回)，一次就把 stdio 的读缓冲打掉；这个函数在 TextBlock::deserialize 里是**逐词**
  // 调的（一页几百个词，逐词读 ruby 数据），于是每词都退化成一次随机扇区读，一页白花 300~615ms。
  // 那个边界检查本来也是冗余的：长度上限 maxLength 已经挡住畸形值，截断则被 file.read 的短读
  // 返回值抓住（它返回实际读到的字节数）。
  std::string next;
  next.resize(len);
  if (len > 0 && file.read(next.data(), len) != static_cast<int>(len)) return false;
  s = std::move(next);
  return true;
}
}  // namespace serialization
