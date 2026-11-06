#pragma once
// ── PDF 词法 + 对象模型 ──────────────────────────────────────────────────
// 值类型只覆盖 PDF 文件里真正会出现的九种。流对象表现为「字典 + 源文件里的
// [offset,length)」——解析阶段绝不把流内容读进内存，由 PdfDocument 按需解码。

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "PdfSource.h"
#include "PdfTypes.h"

enum class PdfType : uint8_t { Null, Bool, Int, Real, Name, String, Array, Dict, Ref };

struct PdfValue {
  PdfType type = PdfType::Null;
  bool boolean = false;
  double num = 0;
  std::string str;   // Name 的已解码名字 / String 的原始字节
  std::vector<PdfValue> arr;
  std::vector<std::pair<std::string, PdfValue>> dict;
  PdfRef ref;

  // 仅 Dict 且是间接流时有意义。streamLenRef 优先于 streamLen。
  bool hasStream = false;
  uint64_t streamOff = 0;
  uint64_t streamLen = 0;
  bool streamLenIndirect = false;
  PdfRef streamLenRef;

  bool isNull() const { return type == PdfType::Null; }
  bool isName(const char *n) const { return type == PdfType::Name && str == n; }
  bool isDict() const { return type == PdfType::Dict; }
  bool isArray() const { return type == PdfType::Array; }
  int asInt(int def = 0) const {
    if (type == PdfType::Int || type == PdfType::Real) return static_cast<int>(num);
    return def;
  }
  double asNum(double def = 0) const {
    if (type == PdfType::Int || type == PdfType::Real) return num;
    return def;
  }

  const PdfValue *get(const char *key) const {
    if (type != PdfType::Dict) return nullptr;
    for (const auto &kv : dict) {
      if (kv.first == key) return &kv.second;
    }
    return nullptr;
  }
  PdfValue *getMutable(const char *key) {
    if (type != PdfType::Dict) return nullptr;
    for (auto &kv : dict) {
      if (kv.first == key) return &kv.second;
    }
    return nullptr;
  }
  void set(const char *key, PdfValue v) {
    if (type != PdfType::Dict) {
      type = PdfType::Dict;
      dict.clear();
    }
    for (auto &kv : dict) {
      if (kv.first == key) {
        kv.second = std::move(v);
        return;
      }
    }
    dict.emplace_back(key, std::move(v));
  }
};

// 词法单元。Number 统一用 double 存，整数性由 Token::isInt 保留。
struct PdfToken {
  enum Kind : uint8_t { Eof, Int, Real, Name, String, ArrOpen, ArrClose, DictOpen, DictClose, Keyword, Error };
  Kind kind = Eof;
  long long i = 0;
  double num = 0;
  std::string text;   // Name/String 内容或 Keyword 拼写
  bool isInt() const { return kind == Int; }
  bool isNum() const { return kind == Int || kind == Real; }
  bool isKeyword(const char *k) const { return kind == Keyword && text == k; }
  bool isName(const char *n) const { return kind == Name && text == n; }
};

class PdfLexer {
 public:
  PdfLexer(const PdfSource &src, uint64_t pos = 0) : src_(src), pos_(pos), base_(pos) {}
  bool next(PdfToken &tok, PdfError *err, std::string *msg);
  // 下一个未读字节的文件偏移。缓冲区可能已读到更远处，pos_ 单独不可用。
  uint64_t pos() const { return base_ + bufPos_; }
  void setPos(uint64_t p) {
    pos_ = p;
    base_ = p;
    inBuf_ = 0;
    bufPos_ = 0;
    eof_ = false;
  }
  const PdfSource &source() const { return src_; }

 private:
  int getByte();
  int peekByte();
  void skipSpace();
  bool readName(PdfToken &tok);
  bool readLiteralString(PdfToken &tok);
  bool readHexString(PdfToken &tok);
  bool readNumber(PdfToken &tok);

  const PdfSource &src_;
  uint64_t pos_ = 0;   // 下一次填充缓冲区的读偏移
  uint64_t base_ = 0;  // buf_[0] 的文件偏移
  static constexpr size_t kBuf = 2048;
  uint8_t buf_[kBuf];
  size_t bufPos_ = 0;
  size_t inBuf_ = 0;
  bool eof_ = false;
};

class PdfParser {
 public:
  explicit PdfParser(const PdfSource &src) : src_(src) {}

  // 从 off 解析 "num gen obj <value> [stream ...]"。
  bool parseIndirect(uint64_t off, int *objNum, int *gen, PdfValue *out, PdfError *err,
                     std::string *msg);
  // 解析一个直接对象（ObjStm 内 / 主机测试）。
  bool parseValueAt(uint64_t off, PdfValue *out, PdfError *err, std::string *msg);

 private:
  bool parseValue(PdfLexer &lex, PdfValue *out, int depth, PdfError *err, std::string *msg);
  bool parseValueFromToken(PdfLexer &lex, PdfToken &tok, PdfValue *out, int depth, PdfError *err,
                           std::string *msg);
  bool parseArrayBody(PdfLexer &lex, PdfValue *out, int depth, PdfError *err, std::string *msg);
  bool parseDictBody(PdfLexer &lex, PdfValue *out, int depth, PdfError *err, std::string *msg);

  const PdfSource &src_;
};
