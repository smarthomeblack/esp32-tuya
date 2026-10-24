#include "tuya_crypto/json_writer.h"

#include <esp_log.h>

#include <cfloat>
#include <climits>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>
#include <string_view>

static const char* TAG = "JsonWriter";

namespace tuya_crypto {

namespace {

constexpr char kHexDigits[] = "0123456789abcdef";

void AppendUnicodeEscape(std::string& out, uint32_t cp) {
    char buf[6];
    buf[0] = '\\';
    buf[1] = 'u';
    buf[2] = kHexDigits[(cp >> 12) & 0xF];
    buf[3] = kHexDigits[(cp >> 8) & 0xF];
    buf[4] = kHexDigits[(cp >> 4) & 0xF];
    buf[5] = kHexDigits[cp & 0xF];
    out.append(buf, sizeof(buf));
}

/// Strict UTF-8 decoder (Python-compatible): rejects overlong encodings,
/// surrogates, code points above U+10FFFF, truncated and stray sequences.
bool DecodeUtf8(std::string_view s, size_t i, uint32_t* cp, size_t* len) {
    const size_t avail = s.size() - i;
    const auto c = [s, i](size_t k) { return static_cast<unsigned char>(s[i + k]); };
    const auto isCont = [&c](size_t k) {
        const unsigned char b = c(k);
        return b >= 0x80 && b <= 0xBF;
    };

    const unsigned char c0 = c(0);
    if (c0 >= 0xC2 && c0 <= 0xDF) {
        if (avail < 2 || !isCont(1))
            return false;
        *cp = (static_cast<uint32_t>(c0 & 0x1F) << 6) | (c(1) & 0x3F);
        *len = 2;
    } else if (c0 == 0xE0) {
        if (avail < 3 || c(1) < 0xA0 || c(1) > 0xBF || !isCont(2))
            return false;
        *cp = (static_cast<uint32_t>(c0 & 0x0F) << 12) | (static_cast<uint32_t>(c(1) & 0x3F) << 6) |
              (c(2) & 0x3F);
        *len = 3;
    } else if ((c0 >= 0xE1 && c0 <= 0xEC) || (c0 >= 0xEE && c0 <= 0xEF)) {
        if (avail < 3 || !isCont(1) || !isCont(2))
            return false;
        *cp = (static_cast<uint32_t>(c0 & 0x0F) << 12) | (static_cast<uint32_t>(c(1) & 0x3F) << 6) |
              (c(2) & 0x3F);
        *len = 3;
    } else if (c0 == 0xED) {
        // 0xED 0xA0..0xBF would encode a UTF-16 surrogate: rejected.
        if (avail < 3 || c(1) < 0x80 || c(1) > 0x9F || !isCont(2))
            return false;
        *cp = (static_cast<uint32_t>(c0 & 0x0F) << 12) | (static_cast<uint32_t>(c(1) & 0x3F) << 6) |
              (c(2) & 0x3F);
        *len = 3;
    } else if (c0 == 0xF0) {
        if (avail < 4 || c(1) < 0x90 || c(1) > 0xBF || !isCont(2) || !isCont(3))
            return false;
        *cp = (static_cast<uint32_t>(c0 & 0x07) << 18) |
              (static_cast<uint32_t>(c(1) & 0x3F) << 12) |
              (static_cast<uint32_t>(c(2) & 0x3F) << 6) | (c(3) & 0x3F);
        *len = 4;
    } else if (c0 >= 0xF1 && c0 <= 0xF3) {
        if (avail < 4 || !isCont(1) || !isCont(2) || !isCont(3))
            return false;
        *cp = (static_cast<uint32_t>(c0 & 0x07) << 18) |
              (static_cast<uint32_t>(c(1) & 0x3F) << 12) |
              (static_cast<uint32_t>(c(2) & 0x3F) << 6) | (c(3) & 0x3F);
        *len = 4;
    } else if (c0 == 0xF4) {
        // 0xF4 0x90+ would exceed U+10FFFF: rejected.
        if (avail < 4 || c(1) < 0x80 || c(1) > 0x8F || !isCont(2) || !isCont(3))
            return false;
        *cp = (static_cast<uint32_t>(c0 & 0x07) << 18) |
              (static_cast<uint32_t>(c(1) & 0x3F) << 12) |
              (static_cast<uint32_t>(c(2) & 0x3F) << 6) | (c(3) & 0x3F);
        *len = 4;
    } else {
        // 0x80..0xC1 (stray continuation / overlong 2-byte), 0xF5..0xFF.
        return false;
    }
    return true;
}

/// JSON-escaped form of `s`, matching Python json.dumps(ensure_ascii=True).
/// Invalid UTF-8 bytes each become one \ufffd (defensive).
void AppendEscaped(std::string& out, std::string_view s) {
    const size_t n = s.size();
    size_t i = 0;
    while (i < n) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        if (c == '"') {
            out += "\\\"";
            ++i;
            continue;
        }
        if (c == '\\') {
            out += "\\\\";
            ++i;
            continue;
        }
        if (c == '\b' || c == '\f' || c == '\n' || c == '\r' || c == '\t') {
            switch (c) {
                case '\b': out += "\\b"; break;
                case '\f': out += "\\f"; break;
                case '\n': out += "\\n"; break;
                case '\r': out += "\\r"; break;
                default: out += "\\t"; break;
            }
            ++i;
            continue;
        }
        if (c < 0x20 || c == 0x7F) {  // C0 controls and DEL -> \u00xx
            AppendUnicodeEscape(out, c);
            ++i;
            continue;
        }
        if (c < 0x80) {
            out.push_back(static_cast<char>(c));
            ++i;
            continue;
        }

        uint32_t cp = 0;
        size_t len = 0;
        if (!DecodeUtf8(s, i, &cp, &len)) {
            AppendUnicodeEscape(out, 0xFFFD);
            ++i;
            continue;
        }
        i += len;
        if (cp >= 0x10000) {
            const uint32_t v = cp - 0x10000;
            AppendUnicodeEscape(out, 0xD800 + (v >> 10));
            AppendUnicodeEscape(out, 0xDC00 + (v & 0x3FF));
        } else {
            AppendUnicodeEscape(out, cp);
        }
    }
}

/// cJSON print_string parity: stops at the first NUL, \u00xx for C0 controls
/// only; every byte >= 0x20 (UTF-8, DEL) passes through raw.
void AppendCjsonEscaped(std::string& out, std::string_view s) {
    const size_t n = s.size();
    for (size_t i = 0; i < n && s[i] != '\0'; ++i) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\b': out += "\\b"; break;
            case '\f': out += "\\f"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (c < 32) {
                    AppendUnicodeEscape(out, c);
                } else {
                    out.push_back(static_cast<char>(c));
                }
                break;
        }
    }
}

/// cJSON compare_double: relative-epsilon equality, not an exact round-trip.
bool CjsonCompareDouble(double a, double b) {
    const double max_val = std::fabs(a) > std::fabs(b) ? std::fabs(a) : std::fabs(b);
    return std::fabs(a - b) <= max_val * DBL_EPSILON;
}

}  // namespace

void JsonWriter::BeginObject() {
    if (!PrepareContainer()) {
        return;
    }
    out_.push_back('{');
    stack_[depth_++] = Frame{true, true};
}

void JsonWriter::BeginArray() {
    if (!PrepareContainer()) {
        return;
    }
    out_.push_back('[');
    stack_[depth_++] = Frame{false, true};
}

void JsonWriter::EndObject() {
    if (error_) {
        return;
    }
    if (depth_ == 0 || !stack_[depth_ - 1].is_object) {
        Fail("EndObject without matching BeginObject");
        return;
    }
    if (expect_value_) {
        Fail("EndObject after a Key with no value");
        return;
    }
    out_.push_back('}');
    --depth_;
}

void JsonWriter::EndArray() {
    if (error_) {
        return;
    }
    if (depth_ == 0 || stack_[depth_ - 1].is_object) {
        Fail("EndArray without matching BeginArray");
        return;
    }
    out_.push_back(']');
    --depth_;
}

void JsonWriter::Key(std::string_view key) {
    if (!PrepareKey()) {
        return;
    }
    out_.push_back('"');
    AppendEscaped(out_, key);
    out_.push_back('"');
    out_.push_back(':');
}

void JsonWriter::KeyCjson(std::string_view key) {
    if (!PrepareKey()) {
        return;
    }
    out_.push_back('"');
    AppendCjsonEscaped(out_, key);
    out_.push_back('"');
    out_.push_back(':');
}

bool JsonWriter::PrepareKey() {
    if (error_) {
        return false;
    }
    if (depth_ == 0) {
        Fail("Key at top level");
        return false;
    }
    Frame& f = stack_[depth_ - 1];
    if (!f.is_object) {
        Fail("Key inside an array");
        return false;
    }
    if (expect_value_) {
        Fail("Key directly after a Key");
        return false;
    }
    if (!f.first) {
        out_.push_back(',');
    }
    f.first = false;
    expect_value_ = true;
    return true;
}

void JsonWriter::String(std::string_view value) {
    if (!PrepareValue()) {
        return;
    }
    out_.push_back('"');
    AppendEscaped(out_, value);
    out_.push_back('"');
}

void JsonWriter::Int(int64_t value) {
    if (!PrepareValue()) {
        return;
    }
    out_ += std::to_string(value);
}

void JsonWriter::Uint(uint64_t value) {
    if (!PrepareValue()) {
        return;
    }
    out_ += std::to_string(value);
}

void JsonWriter::Bool(bool value) {
    if (!PrepareValue()) {
        return;
    }
    out_ += value ? "true" : "false";
}

void JsonWriter::Null() {
    if (!PrepareValue()) {
        return;
    }
    out_ += "null";
}

void JsonWriter::Double(double value) {
    if (!PrepareValue()) {
        return;
    }
    if (std::isnan(value)) {
        out_ += "NaN";  // Python json.dumps(allow_nan=True) token
        return;
    }
    if (std::isinf(value)) {
        out_ += value > 0 ? "Infinity" : "-Infinity";
        return;
    }
    // %.17g round-trips a double but is not always Python's shortest repr.
    char buf[32];
    const int n = std::snprintf(buf, sizeof(buf), "%.17g", value);
    if (n > 0 && static_cast<size_t>(n) < sizeof(buf)) {
        out_.append(buf, static_cast<size_t>(n));
    }
}

void JsonWriter::StringCjson(std::string_view value) {
    if (!PrepareValue()) {
        return;
    }
    out_.push_back('"');
    AppendCjsonEscaped(out_, value);
    out_.push_back('"');
}

void JsonWriter::DoubleCjson(double value) {
    if (!PrepareValue()) {
        return;
    }
    if (std::isnan(value) || std::isinf(value)) {
        out_ += "null";
        return;
    }
    // cJSON's valueint is a saturated int32 mirror; %d prints when exactly equal.
    int valueint = 0;
    if (value >= static_cast<double>(INT_MAX)) {
        valueint = INT_MAX;
    } else if (value <= static_cast<double>(INT_MIN)) {
        valueint = INT_MIN;
    } else {
        valueint = static_cast<int>(value);
    }
    char buf[26];  // cJSON's number_buffer size
    int n = 0;
    if (value == static_cast<double>(valueint)) {
        n = std::snprintf(buf, sizeof(buf), "%d", valueint);
    } else {
        // %1.15g kept when it survives cJSON's fuzzy round-trip, else %1.17g.
        n = std::snprintf(buf, sizeof(buf), "%1.15g", value);
        double parsed = 0.0;
        if (n <= 0 || std::sscanf(buf, "%lg", &parsed) != 1 || !CjsonCompareDouble(parsed, value)) {
            n = std::snprintf(buf, sizeof(buf), "%1.17g", value);
        }
    }
    if (n <= 0 || static_cast<size_t>(n) >= sizeof(buf)) {
        Fail("number formatting failed");
        return;
    }
    out_.append(buf, static_cast<size_t>(n));
}

bool JsonWriter::PrepareValue() {
    if (error_) {
        return false;
    }
    if (depth_ == 0) {
        Fail("value at top level (open a container first)");
        return false;
    }
    Frame& f = stack_[depth_ - 1];
    if (f.is_object) {
        if (!expect_value_) {
            Fail("value inside an object without a preceding Key");
            return false;
        }
        expect_value_ = false;
    } else {
        if (!f.first) {
            out_.push_back(',');
        }
        f.first = false;
    }
    return true;
}

bool JsonWriter::PrepareContainer() {
    if (error_) {
        return false;
    }
    if (depth_ == 0) {
        if (!out_.empty()) {
            Fail("second root value");
            return false;
        }
    } else {
        // A container is a value: same positioning rules as PrepareValue.
        Frame& f = stack_[depth_ - 1];
        if (f.is_object) {
            if (!expect_value_) {
                Fail("container inside an object without a preceding Key");
                return false;
            }
            expect_value_ = false;
        } else {
            if (!f.first) {
                out_.push_back(',');
            }
            f.first = false;
        }
    }
    if (depth_ >= kMaxDepth) {
        Fail("nesting deeper than kMaxDepth");
        return false;
    }
    return true;
}

void JsonWriter::Fail(const char* why) {
    if (error_) {
        return;  // log only the first misuse
    }
    error_ = true;
    ESP_LOGE(TAG, "misuse: %s (output frozen at %u bytes)", why,
             static_cast<unsigned>(out_.size()));
}

}  // namespace tuya_crypto
