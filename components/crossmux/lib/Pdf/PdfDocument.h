#pragma once
// ── PDF 文档 ───────────────────────────────────────────────────────────────
// xref + 对象缓存 + ObjStm 展开 + 页面树继承。对上层只暴露「第 i 页的合并后
// 页字典」和「流信息/流字节」，不暴露解析细节。所有缓存都有上限。

#include <cstdint>
#include <deque>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "PdfFilters.h"
#include "PdfObject.h"
#include "PdfSource.h"
#include "PdfTypes.h"
#include "PdfXref.h"

struct PdfStreamInfo {
  uint64_t off = 0;
  uint64_t len = 0;
  std::vector<PdfFilterSpec> filters;
  bool hasDct() const {
    if (filters.size() != 1) return false;
    return filters[0].isDct();
  }
  bool hasJbig2() const {
    if (filters.size() != 1) return false;
    return filters[0].isJbig2();
  }
};

class PdfDocument {
 public:
  PdfDocument() = default;
  ~PdfDocument();

  bool open(const PdfSource &src, PdfError *err, std::string *msg);
  void close();
  bool isOpen() const { return src_ != nullptr; }

  const PdfSource *source() const { return src_; }
  const PdfXref &xref() const { return xref_; }
  const PdfValue *catalog() const { return catalog_.get(); }

  // 按对象号取对象；指针在下一次 object()/close() 前有效。
  const PdfValue *object(int num, PdfError *err, std::string *msg);
  // Ref → 对象；Dict → 原样；其它 → nullptr。
  const PdfValue *resolveRef(const PdfValue &v, PdfError *err, std::string *msg);

  int pageCount() const { return static_cast<int>(pages_.size()); }
  // 已合并继承属性的页字典副本。
  const PdfValue *pageDict(int index) const;

  bool streamInfo(const PdfValue &dict, PdfStreamInfo *info, PdfError *err, std::string *msg);
  // 把流完整解码进内存（DCT 不在此解码，调用方用 streamInfo + 原始区间）。
  bool streamToMemory(const PdfValue &dict, pdfscratch::ByteVec *out, uint64_t maxOut,
                      PdfError *err, std::string *msg);

 private:
  bool buildPages(PdfError *err, std::string *msg);
  bool walkNode(const PdfValue &node, const std::shared_ptr<const PdfValue> &resources,
                const PdfRect &mediaBox, bool hasMediaBox, const PdfRect &cropBox,
                bool hasCropBox, int rotate, int depth, std::unordered_set<int> *active,
                PdfError *err, std::string *msg);
  bool loadFromObjStm(int num, int streamNum, int streamIdx, PdfValue *out, PdfError *err,
                      std::string *msg);
  bool objStmBytes(int streamNum, std::shared_ptr<pdfscratch::ByteVec> *out, PdfError *err,
                   std::string *msg);
  void cacheObject(int num, PdfValue &&v);

  const PdfSource *src_ = nullptr;
  PdfXref xref_;
  std::shared_ptr<PdfValue> catalog_;

  std::unordered_map<int, PdfValue> cache_;
  std::deque<int> cacheOrder_;

  // 对象流解压缓存最大 16 MB，必须走 PSRAM。
  std::unordered_map<int, std::shared_ptr<pdfscratch::ByteVec>> objStm_;
  std::deque<int> objStmOrder_;
  size_t objStmBytes_ = 0;

  std::vector<std::shared_ptr<const PdfValue>> pages_;
};
