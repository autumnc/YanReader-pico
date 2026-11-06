#pragma once
// ── 交叉引用表 ────────────────────────────────────────────────────────────
// 支持经典 xref 表、xref stream、/Prev 链、混合文件的 /XRefStm，以及
// ObjStm 里的压缩对象（只登记「在哪个对象流、第几项」，不解压）。
// 新段先读、先读的条目优先：/Prev 指向更老的段，不能覆盖新条目。

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include "PdfObject.h"
#include "PdfScratch.h"
#include "PdfSource.h"
#include "PdfTypes.h"

struct PdfXrefEntry {
  enum Type : uint8_t { Free = 0, InUse = 1, Compressed = 2 };
  uint8_t type = Free;
  uint64_t off = 0;   // InUse: 对象在文件中的偏移
  uint32_t gen = 0;
  int32_t streamNum = -1;  // Compressed: 对象流对象号
  int32_t streamIdx = -1;  // Compressed: 在对象流里的序号
};

class PdfXref {
 public:
  // 从文件尾部找 startxref，再从该偏移加载整条链。失败时尝试在尾部找 "xref"。
  bool load(const PdfSource &src, PdfError *err, std::string *msg);
  // 已知 startxref（主机测试 / 恢复点）。
  bool loadAt(const PdfSource &src, uint64_t startXref, PdfError *err, std::string *msg);

  // 返回 nullptr 表示该对象号不存在。
  const PdfXrefEntry *find(int objNum) const;
  int count() const { return static_cast<int>(entries_.size()); }
  // 合并后的 trailer（Root/Size/Encrypt 等，新段优先）。
  const PdfValue &trailer() const { return trailer_; }
  bool hasTrailer() const { return hasTrailer_; }

 private:
  bool loadSection(const PdfSource &src, uint64_t off, int depth, PdfError *err, std::string *msg);
  bool loadClassic(const PdfSource &src, uint64_t off, int depth, PdfError *err, std::string *msg);
  bool loadXrefStream(const PdfSource &src, uint64_t off, int depth, PdfError *err,
                      std::string *msg);
  bool ensureCapacity(int size, PdfError *err, std::string *msg);
  void putEntry(int num, const PdfXrefEntry &e);
  void mergeTrailer(const PdfValue &t);

  // xref 表随对象数增长（5000 页的书可到几万项），必须走 PSRAM。
  pdfscratch::Vec<PdfXrefEntry> entries_;
  pdfscratch::ByteVec seen_;
  PdfValue trailer_;
  bool hasTrailer_ = false;
};
