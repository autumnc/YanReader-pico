#pragma once
// 随机读抽象。解析器不认识 HalStorage / FILE*，设备端与主机端各给一个实现。
// readAt 返回实际读到的字节数；出错返回 -1。越界读由调用方先按 size() 检查。

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <vector>

class PdfSource {
 public:
  virtual ~PdfSource() = default;
  virtual uint64_t size() const = 0;
  virtual int readAt(uint64_t off, void *buf, size_t len) const = 0;
};

// 内存源：ObjStm 里的对象、以及主机测试的小夹具都用它。
class PdfMemSource final : public PdfSource {
 public:
  PdfMemSource(const uint8_t *data, size_t len) : data_(data), len_(len) {}
  explicit PdfMemSource(std::shared_ptr<std::vector<uint8_t>> owner)
      : owner_(std::move(owner)), data_(owner_->data()), len_(owner_->size()) {}
  uint64_t size() const override { return len_; }
  int readAt(uint64_t off, void *buf, size_t len) const override {
    if (off > len_) return -1;
    const size_t n = static_cast<size_t>(len);
    if (n > len_ - static_cast<size_t>(off)) return -1;
    memcpy(buf, data_ + off, n);
    return static_cast<int>(n);
  }

 private:
  std::shared_ptr<std::vector<uint8_t>> owner_;
  const uint8_t *data_ = nullptr;
  size_t len_ = 0;
};
