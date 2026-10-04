#pragma once

// 极简 JSON 值/解析器。移植自 ../Flomo/src/json_parser.{h,cpp}（C1 Flomo 客户端），
// 除了文件 I/O 改成本地 stdio + 原子写（见 .cpp 末尾）之外逐字保留：
// Flomo 的接口一律返回 `{"code":0,"data":…}` 这种一层嵌套，用它足够，
// 不必把整套 JSON 库搬进来。

#include <cstddef>
#include <string>
#include <vector>

enum class JsonType { Null, Bool, Number, String, Array, Object };

class JsonValue {
public:
    JsonType type = JsonType::Null;
    std::string stringValue;
    double numberValue = 0;
    bool boolValue = false;

    // Object —— 并行数组（memberKeys[i] ↔ memberValues[i]）；笔记只有几十个字段，
    // 线性查找比哈希表省内存也够快。
    std::vector<std::string> memberKeys;
    std::vector<JsonValue> memberValues;

    // Array
    std::vector<JsonValue> elements;

    JsonValue() = default;
    JsonValue(std::nullptr_t) : type(JsonType::Null) {}
    JsonValue(const std::string &s) : type(JsonType::String), stringValue(s) {}
    JsonValue(const char *s) : type(JsonType::String), stringValue(s) {}
    JsonValue(double n) : type(JsonType::Number), numberValue(n) {}
    JsonValue(int n) : type(JsonType::Number), numberValue(static_cast<double>(n)) {}
    JsonValue(bool b) : type(JsonType::Bool), boolValue(b) {}

    bool isNull()   const { return type == JsonType::Null; }
    bool isBool()   const { return type == JsonType::Bool; }
    bool isNumber() const { return type == JsonType::Number; }
    bool isString() const { return type == JsonType::String; }
    bool isArray()  const { return type == JsonType::Array; }
    bool isObject() const { return type == JsonType::Object; }

    std::string asString(const std::string &def = "") const;
    double asNumber(double def = 0) const;
    int asInt(int def = 0) const;
    bool asBool(bool def = false) const;

    JsonValue &operator[](const std::string &key);
    const JsonValue &operator[](const std::string &key) const;
    bool has(const std::string &key) const;
    void set(const std::string &key, const JsonValue &val);

    JsonValue &operator[](size_t index);
    const JsonValue &operator[](size_t index) const;
    size_t size() const;
    void pushBack(const JsonValue &val);

    std::string serialize(int indent = 0) const;

    static JsonValue parse(const std::string &json);
    static JsonValue loadFromFile(const std::string &path);
    static bool saveToFile(const std::string &path, const JsonValue &val);
    static JsonValue object();
    static JsonValue array();
};
