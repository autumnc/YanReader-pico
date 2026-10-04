#pragma once
#include <cstddef>
#include <cstdint>

// 极简 Print 基类：HalFile / 流式输出目标的最小公共接口。
class Print {
 public:
  virtual ~Print() = default;
  virtual size_t write(uint8_t b) = 0;
  virtual size_t write(const uint8_t* buffer, size_t size) {
    size_t n = 0;
    while (size--) {
      if (write(*buffer++)) n++;
      else break;
    }
    return n;
  }
  virtual void flush() {}
};
