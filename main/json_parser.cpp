#include "json_parser.h"
#include "safe_file.h"
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <cmath>
#include <unistd.h>

static const long MAX_JSON_FILE_SIZE = 256 * 1024;

// ── Value accessors ──────────────────────────────────────────────────────────

std::string JsonValue::asString(const std::string &def) const {
    return isString() ? stringValue : def;
}

double JsonValue::asNumber(double def) const {
    return isNumber() ? numberValue : def;
}

int JsonValue::asInt(int def) const {
    return isNumber() ? static_cast<int>(numberValue) : def;
}

bool JsonValue::asBool(bool def) const {
    return isBool() ? boolValue : def;
}

JsonValue &JsonValue::operator[](const std::string &key) {
    if (type == JsonType::Null) type = JsonType::Object;
    for (size_t i = 0; i < memberKeys.size(); i++)
        if (memberKeys[i] == key) return memberValues[i];
    memberKeys.push_back(key);
    memberValues.push_back(JsonValue());
    return memberValues.back();
}

const JsonValue &JsonValue::operator[](const std::string &key) const {
    for (size_t i = 0; i < memberKeys.size(); i++)
        if (memberKeys[i] == key) return memberValues[i];
    static const JsonValue nullVal;
    return nullVal;
}

bool JsonValue::has(const std::string &key) const {
    for (const auto &k : memberKeys)
        if (k == key) return true;
    return false;
}

void JsonValue::set(const std::string &key, const JsonValue &val) {
    if (type == JsonType::Null) type = JsonType::Object;
    for (size_t i = 0; i < memberKeys.size(); i++) {
        if (memberKeys[i] == key) { memberValues[i] = val; return; }
    }
    memberKeys.push_back(key);
    memberValues.push_back(val);
}

JsonValue &JsonValue::operator[](size_t index) {
    return elements[index];
}

const JsonValue &JsonValue::operator[](size_t index) const {
    return elements[index];
}

size_t JsonValue::size() const {
    if (isArray()) return elements.size();
    if (isObject()) return memberKeys.size();
    return 0;
}

void JsonValue::pushBack(const JsonValue &val) {
    if (type == JsonType::Null) type = JsonType::Array;
    elements.push_back(val);
}

JsonValue JsonValue::object() { JsonValue v; v.type = JsonType::Object; return v; }
JsonValue JsonValue::array()  { JsonValue v; v.type = JsonType::Array; return v; }

// ── Serialization ────────────────────────────────────────────────────────────

static void indentStr(std::string &out, int depth) {
    for (int i = 0; i < depth; i++) out += "  ";
}

static void escapeString(std::string &out, const std::string &s) {
    out += '"';
    for (size_t i = 0; i < s.size(); i++) {
        char c = s[i];
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\b': out += "\\b";  break;
            case '\f': out += "\\f";  break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            default:
                if ((unsigned char)c < 0x20) {
                    char buf[8];
                    snprintf(buf, sizeof(buf), "\\u%04x", (unsigned char)c);
                    out += buf;
                } else {
                    out += c;
                }
        }
    }
    out += '"';
}

std::string JsonValue::serialize(int indent) const {
    std::string out;
    switch (type) {
    case JsonType::Null:
        out += "null"; break;
    case JsonType::Bool:
        out += boolValue ? "true" : "false"; break;
    case JsonType::Number: {
        char buf[64];
        double intpart;
        if (std::modf(numberValue, &intpart) == 0.0 && numberValue >= -1e9 && numberValue <= 1e9)
            snprintf(buf, sizeof(buf), "%.0f", numberValue);
        else
            snprintf(buf, sizeof(buf), "%g", numberValue);
        out += buf;
        break;
    }
    case JsonType::String:
        escapeString(out, stringValue);
        break;
    case JsonType::Array:
        if (elements.empty()) { out += "[]"; break; }
        out += "[\n";
        for (size_t i = 0; i < elements.size(); i++) {
            indentStr(out, indent + 1);
            out += elements[i].serialize(indent + 1);
            if (i + 1 < elements.size()) out += ',';
            out += '\n';
        }
        indentStr(out, indent);
        out += ']';
        break;
    case JsonType::Object:
        if (memberKeys.empty()) { out += "{}"; break; }
        out += "{\n";
        for (size_t i = 0; i < memberKeys.size(); i++) {
            indentStr(out, indent + 1);
            escapeString(out, memberKeys[i]);
            out += ": ";
            out += memberValues[i].serialize(indent + 1);
            if (i + 1 < memberKeys.size()) out += ',';
            out += '\n';
        }
        indentStr(out, indent);
        out += '}';
        break;
    }
    return out;
}

// ── Parser ───────────────────────────────────────────────────────────────────

struct ParseCtx {
    const char *p;
    size_t len;
    size_t pos;
};

static void skipWS(ParseCtx &c) {
    while (c.pos < c.len) {
        char ch = c.p[c.pos];
        if (ch != ' ' && ch != '\t' && ch != '\n' && ch != '\r') break;
        c.pos++;
    }
}

static bool peek(ParseCtx &c, const char *expected) {
    size_t i = 0;
    while (expected[i]) {
        if (c.pos + i >= c.len || c.p[c.pos + i] != expected[i]) return false;
        i++;
    }
    return true;
}

static JsonValue parseValue(ParseCtx &c);

static std::string parseString(ParseCtx &c) {
    std::string out;
    if (c.pos >= c.len || c.p[c.pos] != '"') return out;
    c.pos++;
    while (c.pos < c.len) {
        char ch = c.p[c.pos];
        if (ch == '"') { c.pos++; return out; }
        if (ch == '\\') {
            c.pos++;
            if (c.pos >= c.len) break;
            switch (c.p[c.pos]) {
                case '"':  out += '"';  break;
                case '\\': out += '\\'; break;
                case '/':  out += '/';  break;
                case 'b':  out += '\b'; break;
                case 'f':  out += '\f'; break;
                case 'n':  out += '\n'; break;
                case 'r':  out += '\r'; break;
                case 't':  out += '\t'; break;
                case 'u': {
                    if (c.pos + 4 < c.len) {
                        char hex[5] = {c.p[c.pos+1], c.p[c.pos+2], c.p[c.pos+3], c.p[c.pos+4], 0};
                        unsigned cp = strtoul(hex, nullptr, 16);
                        if (cp < 0x80) out += (char)cp;
                        else if (cp < 0x800) { out += (char)(0xC0 | (cp>>6)); out += (char)(0x80 | (cp&0x3F)); }
                        else { out += (char)(0xE0 | (cp>>12)); out += (char)(0x80 | ((cp>>6)&0x3F)); out += (char)(0x80 | (cp&0x3F)); }
                        c.pos += 4;
                    }
                    break;
                }
                default: out += ch; break;
            }
            c.pos++;
        } else {
            out += ch;
            c.pos++;
        }
    }
    return out;
}

static JsonValue parseNumber(ParseCtx &c) {
    size_t start = c.pos;
    if (c.p[c.pos] == '-') c.pos++;
    while (c.pos < c.len && c.p[c.pos] >= '0' && c.p[c.pos] <= '9') c.pos++;
    if (c.pos < c.len && c.p[c.pos] == '.') {
        c.pos++;
        while (c.pos < c.len && c.p[c.pos] >= '0' && c.p[c.pos] <= '9') c.pos++;
    }
    if (c.pos < c.len && (c.p[c.pos] == 'e' || c.p[c.pos] == 'E')) {
        c.pos++;
        if (c.pos < c.len && (c.p[c.pos] == '+' || c.p[c.pos] == '-')) c.pos++;
        while (c.pos < c.len && c.p[c.pos] >= '0' && c.p[c.pos] <= '9') c.pos++;
    }
    std::string numStr(c.p + start, c.pos - start);
    char *end = nullptr;
    double val = strtod(numStr.c_str(), &end);
    return JsonValue(val);
}

static JsonValue parseArray(ParseCtx &c) {
    JsonValue arr;
    arr.type = JsonType::Array;
    c.pos++;
    skipWS(c);
    if (c.pos < c.len && c.p[c.pos] == ']') { c.pos++; return arr; }
    while (c.pos < c.len) {
        arr.elements.push_back(parseValue(c));
        skipWS(c);
        if (c.pos >= c.len) break;
        if (c.p[c.pos] == ']') { c.pos++; return arr; }
        if (c.p[c.pos] == ',') { c.pos++; skipWS(c); }
    }
    return arr;
}

static JsonValue parseObject(ParseCtx &c) {
    JsonValue obj;
    obj.type = JsonType::Object;
    c.pos++;
    skipWS(c);
    if (c.pos < c.len && c.p[c.pos] == '}') { c.pos++; return obj; }
    while (c.pos < c.len) {
        skipWS(c);
        if (c.pos >= c.len || c.p[c.pos] != '"') break;
        std::string key = parseString(c);
        skipWS(c);
        if (c.pos < c.len && c.p[c.pos] == ':') c.pos++;
        skipWS(c);
        obj.memberKeys.push_back(key);
        obj.memberValues.push_back(parseValue(c));
        skipWS(c);
        if (c.pos >= c.len) break;
        if (c.p[c.pos] == '}') { c.pos++; return obj; }
        if (c.p[c.pos] == ',') { c.pos++; }
    }
    return obj;
}

static JsonValue parseValue(ParseCtx &c) {
    skipWS(c);
    if (c.pos >= c.len) return JsonValue(nullptr);
    char ch = c.p[c.pos];
    if (ch == '"') return JsonValue(parseString(c));
    if (ch == '{') return parseObject(c);
    if (ch == '[') return parseArray(c);
    if (ch == 't' && peek(c, "true"))  { c.pos += 4; return JsonValue(true); }
    if (ch == 'f' && peek(c, "false")) { c.pos += 5; return JsonValue(false); }
    if (ch == 'n' && peek(c, "null"))  { c.pos += 4; return JsonValue(nullptr); }
    if (ch == '-' || (ch >= '0' && ch <= '9')) return parseNumber(c);
    return JsonValue(nullptr);
}

JsonValue JsonValue::parse(const std::string &json) {
    ParseCtx c;
    c.p = json.c_str();
    c.len = json.size();
    c.pos = 0;
    return parseValue(c);
}

// ── File I/O ─────────────────────────────────────────────────────────────────

JsonValue JsonValue::loadFromFile(const std::string &path) {
    FILE *f = fopen(path.c_str(), "rb");
    if (!f) return JsonValue(nullptr);
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz <= 0) { fclose(f); return JsonValue(nullptr); }
    if (sz > MAX_JSON_FILE_SIZE) {
        fclose(f);
        return JsonValue(nullptr);
    }
    std::string buf(static_cast<size_t>(sz), '\0');
    if (fread(&buf[0], 1, static_cast<size_t>(sz), f) != static_cast<size_t>(sz)) {
        fclose(f);
        return JsonValue(nullptr);
    }
    fclose(f);
    return parse(buf);
}

bool JsonValue::saveToFile(const std::string &path, const JsonValue &val) {
    std::string json = val.serialize();
    return safeWriteFile(path, json);
}
