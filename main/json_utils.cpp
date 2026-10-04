#include "json_utils.h"

// Helper: escape string for JSON (handles quotes, backslashes, and control chars)
std::string json_escape(const std::string &s) {
    std::string out;
    for (size_t i = 0; i < s.size(); i++) {
        unsigned char c = s[i];
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\b': out += "\\b"; break;
            case '\f': out += "\\f"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                // Control characters (0x00-0x1F) need \uXXXX
                if (c < 0x20) {
                    char buf[8];
                    snprintf(buf, sizeof(buf), "\\u%04x", c);
                    out += buf;
                } else {
                    out += c;
                }
        }
    }
    return out;
}