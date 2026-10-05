#pragma once

#include <Memory.h>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <utility>

#define FOOTNOTE_NUMBER_LEN 32
// 还留在固定长度上的几处（PageLink::href、TextBlock 里的行内链接 span）用这个上限。
// FootnoteEntry::href 已经不走它了，见下面。
#define FOOTNOTE_HREF_LEN 256
// FootnoteEntry::href 的上限，只在反序列化时当**损坏数据的闸**用，不是截断点。
// anchor 形态的 href 只有几十字节；"alt:" 哨兵那种整段注文的实测最长 ~2.8 KB。
#define FOOTNOTE_MAX_TEXT_BYTES 8192

struct FootnoteEntry {
  char number[FOOTNOTE_NUMBER_LEN];
  // 变长。两种形态：
  //   ① 普通脚注：锚点，如 "OEBPS/x.html#note_1"（calibre 的 URL 编码长路径轻松过 150 字节）
  //   ② "alt:<正文>" 哨兵：注文整段就压在这里，读端直接取 substr(4) 弹注，不去锚点表里找
  //      （斐洞那种"注号是一张小图、正文在 <img alt> 里"的书，一章 800 多条）。
  // 原先这里是 char[256]，②这种把 33% 的注文拦腰截断；改 std::string 后按实际长度付费，
  // 顺带把 FootnoteList 从每页固定 16×288=4608 字节降到 16×~64 字节 + 实际正文。
  std::string href;

  FootnoteEntry() { number[0] = '\0'; }
};

class FootnoteList {
  std::unique_ptr<FootnoteEntry[]> entries_;
  uint8_t capacity_ = 0;
  uint8_t size_ = 0;
  bool allocationFailed_ = false;

  bool allocateStorage(const size_t capacity) {
    if (allocationFailed_) return false;

    // 16 条上限、每条 ~64 字节（href 是 std::string，正文另在堆上按实际长度占），
    // 一页的小账；但仍在堆上，因为 inline storage 会让每一页都背上它（绝大多数页没脚注）。
    entries_ = makeUniqueNoThrow<FootnoteEntry[]>(capacity);
    if (!entries_) {
      allocationFailed_ = true;
      return false;
    }
    capacity_ = static_cast<uint8_t>(capacity);
    return true;
  }

 public:
  // 每页最多记多少条脚注。原来是 16 —— 对"一页三两条脚注"的书够用，但斐洞那种整章
  // 800+ 枚注号混排进正文的书，平均就有 7~18 枚/页（实测单个 <p> 里最多 20 枚，
  // 而一个 <p> 可以整个落在一页上），超出的会被 Page::addFootnote **静默丢掉**：
  // 上标序号还画在正文里，点它却没反应。
  // 提到 64 之后每条 ~56 字节（href 是 std::string，正文按实际长度另算），满页也只有
  // 几 KB，而且只有真的带脚注的页才分配，没脚注的页一分不花。
  static constexpr size_t MAX_SIZE = 64;

  FootnoteList() = default;
  FootnoteList(const FootnoteList&) = delete;
  FootnoteList& operator=(const FootnoteList&) = delete;

  FootnoteList(FootnoteList&& other) noexcept { *this = std::move(other); }
  FootnoteList& operator=(FootnoteList&& other) noexcept {
    if (this == &other) return *this;
    entries_ = std::move(other.entries_);
    capacity_ = std::exchange(other.capacity_, 0);
    size_ = std::exchange(other.size_, 0);
    allocationFailed_ = std::exchange(other.allocationFailed_, false);
    return *this;
  }

  FootnoteEntry* append() {
    if (size_ >= MAX_SIZE || (!entries_ && !allocateStorage(MAX_SIZE)) || size_ >= capacity_) return nullptr;
    return &entries_[size_++];
  }

  bool resize(size_t size) {
    if (size > MAX_SIZE) return false;
    if (size == 0) {
      entries_.reset();
      capacity_ = 0;
      size_ = 0;
      allocationFailed_ = false;
      return true;
    }
    if ((!entries_ && !allocateStorage(size)) || size > capacity_) return false;
    size_ = static_cast<uint8_t>(size);
    return true;
  }

  size_t size() const { return size_; }
  bool empty() const { return size_ == 0; }
  bool allocationFailed() const { return allocationFailed_; }

  FootnoteEntry* data() { return entries_.get(); }
  const FootnoteEntry* data() const { return entries_.get(); }
  FootnoteEntry* begin() { return data(); }
  const FootnoteEntry* begin() const { return data(); }
  FootnoteEntry* end() { return size_ ? data() + size_ : data(); }
  const FootnoteEntry* end() const { return size_ ? data() + size_ : data(); }
  FootnoteEntry& operator[](size_t index) { return entries_[index]; }
  const FootnoteEntry& operator[](size_t index) const { return entries_[index]; }
};
