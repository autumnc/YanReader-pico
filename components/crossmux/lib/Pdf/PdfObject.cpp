#include "PdfObject.h"

#include <cmath>
#include <cstdlib>
#include <cstring>

namespace {

bool isWs(int c) { return c == 0 || c == 9 || c == 10 || c == 12 || c == 13 || c == 32; }
bool isDelim(int c) {
  return isWs(c) || c == '(' || c == ')' || c == '<' || c == '>' || c == '[' || c == ']' ||
         c == '{' || c == '}' || c == '/' || c == '%';
}
int hexVal(int c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}
void setErr(PdfError *err, std::string *msg, PdfError e, const char *m) {
  if (err) *err = e;
  if (msg) *msg = m ? m : "";
}

}  // namespace

int PdfLexer::getByte() {
  if (bufPos_ < inBuf_) return buf_[bufPos_++];
  if (eof_) return -1;
  const uint64_t remain = src_.size() > pos_ ? src_.size() - pos_ : 0;
  const size_t want = remain < kBuf ? static_cast<size_t>(remain) : kBuf;
  if (want == 0) {
    eof_ = true;
    return -1;
  }
  const int n = src_.readAt(pos_, buf_, want);
  if (n <= 0) {
    eof_ = true;
    return -1;
  }
  base_ = pos_;
  pos_ += static_cast<uint64_t>(n);
  bufPos_ = 0;
  inBuf_ = static_cast<size_t>(n);
  return buf_[bufPos_++];
}

int PdfLexer::peekByte() {
  if (bufPos_ < inBuf_) return buf_[bufPos_];
  if (eof_) return -1;
  const uint64_t remain = src_.size() > pos_ ? src_.size() - pos_ : 0;
  const size_t want = remain < kBuf ? static_cast<size_t>(remain) : kBuf;
  if (want == 0) {
    eof_ = true;
    return -1;
  }
  const int n = src_.readAt(pos_, buf_, want);
  if (n <= 0) {
    eof_ = true;
    return -1;
  }
  base_ = pos_;
  pos_ += static_cast<uint64_t>(n);
  bufPos_ = 0;
  inBuf_ = static_cast<size_t>(n);
  return buf_[bufPos_];
}

void PdfLexer::skipSpace() {
  for (;;) {
    const int c = peekByte();
    if (c < 0) return;
    if (isWs(c)) {
      getByte();
      continue;
    }
    if (c == '%') {
      getByte();
      while (true) {
        const int d = peekByte();
        if (d < 0) return;
        getByte();
        if (d == '\r' || d == '\n') break;
      }
      continue;
    }
    return;
  }
}

bool PdfLexer::readName(PdfToken &tok) {
  tok.kind = PdfToken::Name;
  std::string out;
  for (;;) {
    const int c = peekByte();
    if (c < 0 || isDelim(c)) break;
    getByte();
    if (c == '#') {
      const int h1 = hexVal(peekByte());
      if (h1 < 0) {
        out.push_back('#');
        continue;
      }
      getByte();
      const int h2 = hexVal(peekByte());
      if (h2 < 0) {
        out.push_back(static_cast<char>(h1 << 4));
        continue;
      }
      getByte();
      out.push_back(static_cast<char>((h1 << 4) | h2));
    } else {
      out.push_back(static_cast<char>(c));
    }
    if (out.size() > 4096) return false;
  }
  tok.text = std::move(out);
  return true;
}

bool PdfLexer::readLiteralString(PdfToken &tok) {
  tok.kind = PdfToken::String;
  std::string out;
  int depth = 1;
  for (;;) {
    int c = getByte();
    if (c < 0) return false;
    if (c == '\\') {
      int e = getByte();
      if (e < 0) return false;
      switch (e) {
        case 'n': out.push_back('\n'); break;
        case 'r': out.push_back('\r'); break;
        case 't': out.push_back('\t'); break;
        case 'b': out.push_back('\b'); break;
        case 'f': out.push_back('\f'); break;
        case '(': out.push_back('('); break;
        case ')': out.push_back(')'); break;
        case '\\': out.push_back('\\'); break;
        case '\r':
          if (peekByte() == '\n') getByte();
          break;
        case '\n': break;
        default:
          if (e >= '0' && e <= '7') {
            int v = e - '0';
            for (int i = 0; i < 2; i++) {
              const int d = peekByte();
              if (d < '0' || d > '7') break;
              getByte();
              v = v * 8 + (d - '0');
            }
            out.push_back(static_cast<char>(v & 0xFF));
          } else {
            out.push_back(static_cast<char>(e));
          }
      }
    } else if (c == '(') {
      depth++;
      out.push_back(static_cast<char>(c));
    } else if (c == ')') {
      if (--depth == 0) break;
      out.push_back(static_cast<char>(c));
    } else {
      out.push_back(static_cast<char>(c));
    }
    if (out.size() > PdfLimits::kMaxStreamBytes) return false;
  }
  tok.text = std::move(out);
  return true;
}

bool PdfLexer::readHexString(PdfToken &tok) {
  tok.kind = PdfToken::String;
  std::string out;
  int hi = -1;
  for (;;) {
    const int c = getByte();
    if (c < 0) return false;
    if (c == '>') break;
    if (isWs(c)) continue;
    const int v = hexVal(c);
    if (v < 0) continue;
    if (hi < 0) {
      hi = v;
    } else {
      out.push_back(static_cast<char>((hi << 4) | v));
      hi = -1;
    }
    if (out.size() > PdfLimits::kMaxStreamBytes) return false;
  }
  if (hi >= 0) out.push_back(static_cast<char>(hi << 4));
  tok.text = std::move(out);
  return true;
}

bool PdfLexer::readNumber(PdfToken &tok) {
  std::string s;
  const int first = getByte();
  if (first < 0) return false;
  s.push_back(static_cast<char>(first));
  for (;;) {
    const int c = peekByte();
    if (c < 0) break;
    if ((c >= '0' && c <= '9') || c == '.' || c == '+' || c == '-' || c == 'e' || c == 'E') {
      getByte();
      s.push_back(static_cast<char>(c));
    } else {
      break;
    }
    if (s.size() > 64) return false;
  }
  char *end = nullptr;
  const double v = strtod(s.c_str(), &end);
  if (end == s.c_str()) return false;
  tok.num = v;
  const bool integral = s.find('.') == std::string::npos && s.find('e') == std::string::npos &&
                        s.find('E') == std::string::npos;
  if (integral && v >= -9.2e18 && v <= 9.2e18) {
    tok.kind = PdfToken::Int;
    tok.i = static_cast<long long>(llround(v));
  } else {
    tok.kind = PdfToken::Real;
  }
  return true;
}

bool PdfLexer::next(PdfToken &tok, PdfError *err, std::string *msg) {
  tok = PdfToken{};
  skipSpace();
  const int pc = peekByte();
  if (pc < 0) {
    tok.kind = PdfToken::Eof;
    return true;
  }
  // 数字不先吃掉首字节：readNumber 自己从缓冲区开头取。
  if ((pc >= '0' && pc <= '9') || pc == '+' || pc == '-' || pc == '.') {
    if (!readNumber(tok)) {
      setErr(err, msg, PdfError::Syntax, "PDF 数字格式错误");
      tok.kind = PdfToken::Error;
      return false;
    }
    return true;
  }
  const int c = getByte();
  switch (c) {
    case '[': tok.kind = PdfToken::ArrOpen; return true;
    case ']': tok.kind = PdfToken::ArrClose; return true;
    case '{': tok.kind = PdfToken::ArrOpen; return true;
    case '}': tok.kind = PdfToken::ArrClose; return true;
    case '<': {
      const int d = peekByte();
      if (d == '<') {
        getByte();
        tok.kind = PdfToken::DictOpen;
        return true;
      }
      return readHexString(tok);
    }
    case '>': {
      const int d = peekByte();
      if (d == '>') {
        getByte();
        tok.kind = PdfToken::DictClose;
        return true;
      }
      setErr(err, msg, PdfError::Syntax, "PDF 语法错误: 孤立的 '>'");
      tok.kind = PdfToken::Error;
      return false;
    }
    case '(': return readLiteralString(tok);
    case '/': {
      if (!readName(tok)) {
        setErr(err, msg, PdfError::Syntax, "PDF 名字过长");
        tok.kind = PdfToken::Error;
        return false;
      }
      return true;
    }
    case ')':
      setErr(err, msg, PdfError::Syntax, "PDF 语法错误: 孤立的 ')'");
      tok.kind = PdfToken::Error;
      return false;
    default: break;
  }
  if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z')) {
    std::string kw;
    kw.push_back(static_cast<char>(c));
    for (;;) {
      const int d = peekByte();
      if (d < 0 || isDelim(d)) break;
      getByte();
      kw.push_back(static_cast<char>(d));
      if (kw.size() > 32) break;
    }
    tok.kind = PdfToken::Keyword;
    tok.text = std::move(kw);
    return true;
  }
  setErr(err, msg, PdfError::Syntax, "PDF 语法错误: 无法识别的字节");
  tok.kind = PdfToken::Error;
  return false;
}

bool PdfParser::parseArrayBody(PdfLexer &lex, PdfValue *out, int depth, PdfError *err,
                               std::string *msg) {
  for (;;) {
    PdfToken t;
    if (!lex.next(t, err, msg)) return false;
    if (t.kind == PdfToken::ArrClose) return true;
    if (t.kind == PdfToken::Eof) {
      setErr(err, msg, PdfError::Syntax, "PDF 数组未闭合");
      return false;
    }
    PdfValue v;
    if (!parseValueFromToken(lex, t, &v, depth + 1, err, msg)) return false;
    out->arr.push_back(std::move(v));
    if (out->arr.size() > 65536) {
      setErr(err, msg, PdfError::TooLarge, "PDF 数组过大");
      return false;
    }
  }
}

bool PdfParser::parseDictBody(PdfLexer &lex, PdfValue *out, int depth, PdfError *err,
                              std::string *msg) {
  out->type = PdfType::Dict;
  for (;;) {
    PdfToken k;
    if (!lex.next(k, err, msg)) return false;
    if (k.kind == PdfToken::DictClose) return true;
    if (k.kind == PdfToken::Eof) {
      setErr(err, msg, PdfError::Syntax, "PDF 字典未闭合");
      return false;
    }
    if (k.kind != PdfToken::Name) {
      setErr(err, msg, PdfError::Syntax, "PDF 字典键不是名字");
      return false;
    }
    PdfToken vt;
    if (!lex.next(vt, err, msg)) return false;
    PdfValue v;
    if (!parseValueFromToken(lex, vt, &v, depth + 1, err, msg)) return false;
    out->set(k.text.c_str(), std::move(v));
    if (out->dict.size() > 65536) {
      setErr(err, msg, PdfError::TooLarge, "PDF 字典过大");
      return false;
    }
  }
}

bool PdfParser::parseValueFromToken(PdfLexer &lex, PdfToken &t, PdfValue *out, int depth,
                                    PdfError *err, std::string *msg) {
  if (depth > 32) {
    setErr(err, msg, PdfError::Syntax, "PDF 对象嵌套过深");
    return false;
  }
  switch (t.kind) {
    case PdfToken::Int: {
      const uint64_t save = lex.pos();
      PdfToken t2, t3;
      if (lex.next(t2, err, msg) && t2.kind == PdfToken::Int && lex.next(t3, err, msg) &&
          t3.isKeyword("R")) {
        out->type = PdfType::Ref;
        out->ref.num = static_cast<int>(t.i);
        out->ref.gen = static_cast<int>(t2.i);
        return true;
      }
      lex.setPos(save);
      out->type = PdfType::Int;
      out->num = static_cast<double>(t.i);
      return true;
    }
    case PdfToken::Real:
      out->type = PdfType::Real;
      out->num = t.num;
      return true;
    case PdfToken::Name:
      out->type = PdfType::Name;
      out->str = std::move(t.text);
      return true;
    case PdfToken::String:
      out->type = PdfType::String;
      out->str = std::move(t.text);
      return true;
    case PdfToken::ArrOpen:
      out->type = PdfType::Array;
      return parseArrayBody(lex, out, depth, err, msg);
    case PdfToken::DictOpen:
      return parseDictBody(lex, out, depth, err, msg);
    case PdfToken::Keyword:
      if (t.isKeyword("true")) { out->type = PdfType::Bool; out->boolean = true; return true; }
      if (t.isKeyword("false")) { out->type = PdfType::Bool; out->boolean = false; return true; }
      if (t.isKeyword("null")) { out->type = PdfType::Null; return true; }
      setErr(err, msg, PdfError::Syntax, "PDF 值位置出现关键字");
      return false;
    default:
      setErr(err, msg, PdfError::Syntax, "PDF 值无法解析");
      return false;
  }
}

bool PdfParser::parseValue(PdfLexer &lex, PdfValue *out, int depth, PdfError *err,
                           std::string *msg) {
  PdfToken t;
  if (!lex.next(t, err, msg)) return false;
  return parseValueFromToken(lex, t, out, depth, err, msg);
}

bool PdfParser::parseIndirect(uint64_t off, int *objNum, int *gen, PdfValue *out, PdfError *err,
                              std::string *msg) {
  PdfLexer lex(src_, off);
  PdfToken t;
  if (!lex.next(t, err, msg) || t.kind != PdfToken::Int) {
    setErr(err, msg, PdfError::Syntax, "PDF 间接对象缺少对象号");
    return false;
  }
  const int num = static_cast<int>(t.i);
  if (!lex.next(t, err, msg) || t.kind != PdfToken::Int) {
    setErr(err, msg, PdfError::Syntax, "PDF 间接对象缺少代号");
    return false;
  }
  const int g = static_cast<int>(t.i);
  if (!lex.next(t, err, msg) || !t.isKeyword("obj")) {
    setErr(err, msg, PdfError::Syntax, "PDF 间接对象缺少 obj 关键字");
    return false;
  }
  if (!parseValue(lex, out, 0, err, msg)) return false;

  if (out->isDict()) {
    const uint64_t save = lex.pos();
    PdfToken nt;
    if (lex.next(nt, err, msg) && nt.isKeyword("stream")) {
      out->hasStream = true;
      uint64_t p = lex.pos();
      for (int i = 0; i < 4; i++) {
        uint8_t c = 0;
        if (src_.readAt(p, &c, 1) != 1) break;
        if (c == ' ' || c == '\t') {
          p++;
          continue;
        }
        if (c == '\r') {
          p++;
          uint8_t c2 = 0;
          if (src_.readAt(p, &c2, 1) == 1 && c2 == '\n') p++;
        } else if (c == '\n') {
          p++;
        }
        break;
      }
      out->streamOff = p;
      const PdfValue *len = out->get("Length");
      if (len && (len->type == PdfType::Int || len->type == PdfType::Real)) {
        out->streamLen = static_cast<uint64_t>(len->asNum());
      } else if (len && len->type == PdfType::Ref) {
        out->streamLenIndirect = true;
        out->streamLenRef = len->ref;
      }
    } else {
      lex.setPos(save);
    }
  }
  if (objNum) *objNum = num;
  if (gen) *gen = g;
  return true;
}

bool PdfParser::parseValueAt(uint64_t off, PdfValue *out, PdfError *err, std::string *msg) {
  PdfLexer lex(src_, off);
  return parseValue(lex, out, 0, err, msg);
}
