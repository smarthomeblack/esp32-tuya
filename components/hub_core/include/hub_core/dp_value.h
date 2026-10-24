#pragma once

#include <cstdint>
#include <cstdio>
#include <string>
#include <string_view>

namespace hub_core {

/// A tagged Tuya DP value. JSON typing: true/false -> kBool, integral
/// int64 -> kInt, other numbers -> kDouble, strings -> kString; null -> "".
struct DpValue {
    enum class Type {
        kBool,
        kInt,
        kDouble,
        kString,
    };

    Type type = Type::kString;
    bool b = false;
    int64_t i = 0;
    double d = 0.0;
    // SSO keeps short values inline in PSRAM map nodes; only long ones hit the internal heap.
    std::string s;

    DpValue() = default;
    explicit DpValue(bool value) : type(Type::kBool), b(value) {}
    explicit DpValue(int64_t value) : type(Type::kInt), i(value) {}
    explicit DpValue(double value) : type(Type::kDouble), d(value) {}
    explicit DpValue(std::string value) : type(Type::kString), s(std::move(value)) {}
    explicit DpValue(const char* value) : type(Type::kString), s(value ? value : "") {}

    static DpValue FromBool(bool value) { return DpValue(value); }
    static DpValue FromInt(int64_t value) { return DpValue(value); }
    static DpValue FromDouble(double value) { return DpValue(value); }
    static DpValue FromString(std::string_view value) { return DpValue(std::string(value)); }

    /// Type-strict equality (kBool(true) != kInt(1); NaN != NaN) — used
    /// by the dedup filter.
    bool operator==(const DpValue& other) const {
        if (type != other.type) {
            return false;
        }
        switch (type) {
            case Type::kBool: return b == other.b;
            case Type::kInt: return i == other.i;
            case Type::kDouble: return d == other.d;
            case Type::kString: return s == other.s;
        }
        return false;
    }

    /// JSON-literal text: true / 42 / 2.5 / "text" (quoted + escaped).
    std::string ToString() const {
        switch (type) {
            case Type::kBool: return b ? "true" : "false";
            case Type::kInt: return std::to_string(i);
            case Type::kDouble: {
                // Shortest round-trip form ("2.5").
                char buf[32];
                const int n = std::snprintf(buf, sizeof(buf), "%.17g", d);
                if (n > 0 && n < static_cast<int>(sizeof(buf))) {
                    return std::string(buf, static_cast<size_t>(n));
                }
                return "0";
            }
            case Type::kString: {
                std::string out;
                out.reserve(s.size() + 2);
                out.push_back('"');
                for (const unsigned char c : s) {
                    switch (c) {
                        case '"': out += "\\\""; break;
                        case '\\': out += "\\\\"; break;
                        case '\n': out += "\\n"; break;
                        case '\r': out += "\\r"; break;
                        case '\t': out += "\\t"; break;
                        default:
                            if (c < 0x20) {
                                char esc[8];
                                std::snprintf(esc, sizeof(esc), "\\u%04x",
                                              static_cast<unsigned>(c));
                                out += esc;
                            } else {
                                out.push_back(static_cast<char>(c));
                            }
                    }
                }
                out.push_back('"');
                return out;
            }
        }
        return "";
    }

    /// Dictionary-key form: "true"/"false", decimal int, "%.17g", or the string.
    std::string AsKeyString() const {
        switch (type) {
            case Type::kBool: return b ? "true" : "false";
            case Type::kInt: return std::to_string(i);
            case Type::kDouble: {
                char buf[32];
                const int n = std::snprintf(buf, sizeof(buf), "%.17g", d);
                if (n > 0 && n < static_cast<int>(sizeof(buf))) {
                    return std::string(buf, static_cast<size_t>(n));
                }
                return "0";
            }
            case Type::kString: return s;
        }
        return "";
    }

    /// True for the JSON-null / empty-string form (the default-strategy fallback).
    bool IsNullOrEmpty() const { return type == Type::kString && s.empty(); }

    /// Same as IsNullOrEmpty(); command validation rejects this form.
    bool IsEmpty() const { return IsNullOrEmpty(); }
};

}  // namespace hub_core
