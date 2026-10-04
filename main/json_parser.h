#pragma once

#include <string>
#include <vector>
#include <cstddef>

enum class JsonType { Null, Bool, Number, String, Array, Object };

class JsonValue {
public:
    JsonType type = JsonType::Null;
    std::string stringValue;
    double numberValue = 0;
    bool boolValue = false;

    // For Object — parallel vectors (memberKeys[i] ↔ memberValues[i])
    std::vector<std::string> memberKeys;
    std::vector<JsonValue> memberValues;

    // For Array
    std::vector<JsonValue> elements;

    JsonValue() = default;
    JsonValue(std::nullptr_t) : type(JsonType::Null) {}
    JsonValue(const std::string &s) : type(JsonType::String), stringValue(s) {}
    JsonValue(const char *s) : type(JsonType::String), stringValue(s) {}
    JsonValue(double n) : type(JsonType::Number), numberValue(n) {}
    JsonValue(int n) : type(JsonType::Number), numberValue(static_cast<double>(n)) {}
    JsonValue(bool b) : type(JsonType::Bool), boolValue(b) {}

    // Type queries
    bool isNull()   const { return type == JsonType::Null; }
    bool isBool()   const { return type == JsonType::Bool; }
    bool isNumber() const { return type == JsonType::Number; }
    bool isString() const { return type == JsonType::String; }
    bool isArray()  const { return type == JsonType::Array; }
    bool isObject() const { return type == JsonType::Object; }

    // Value access with defaults
    std::string asString(const std::string &def = "") const;
    double asNumber(double def = 0) const;
    int asInt(int def = 0) const;
    bool asBool(bool def = false) const;

    // Object access
    JsonValue &operator[](const std::string &key);
    const JsonValue &operator[](const std::string &key) const;
    bool has(const std::string &key) const;
    void set(const std::string &key, const JsonValue &val);

    // Array access
    JsonValue &operator[](size_t index);
    const JsonValue &operator[](size_t index) const;
    size_t size() const;
    void pushBack(const JsonValue &val);

    // Serialize to JSON string
    std::string serialize(int indent = 0) const;

    // --- Static helpers ---
    static JsonValue parse(const std::string &json);
    static JsonValue loadFromFile(const std::string &path);
    static bool saveToFile(const std::string &path, const JsonValue &val);
    static JsonValue object();
    static JsonValue array();
};
