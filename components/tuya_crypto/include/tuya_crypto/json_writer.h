#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>

namespace tuya_crypto {

/// Compact deterministic JSON writer; byte-identical to Python json.dumps(separators=(',', ':'),
/// ensure_ascii=True). Doubles use %.17g (round-trips, not always shortest repr); misuse sets a
/// sticky error — Str() keeps partial output. The *Cjson methods mirror cJSON print bytes instead
/// (legacy registry.json format).
class JsonWriter {
public:
    static constexpr int kMaxDepth = 16;

    JsonWriter() = default;
    JsonWriter(const JsonWriter&) = delete;
    JsonWriter& operator=(const JsonWriter&) = delete;

    void BeginObject();
    void EndObject();
    void BeginArray();
    void EndArray();

    /// Writes `"key":` inside the current object (insertion order).
    void Key(std::string_view key);

    void String(std::string_view value);
    void Int(int64_t value);
    void Uint(uint64_t value);
    void Bool(bool value);
    void Null();
    void Double(double value);

    /// Key with StringCjson escaping.
    void KeyCjson(std::string_view key);
    /// cJSON print_string parity: stops at the first NUL, \u00xx for C0
    /// controls only; every byte >= 0x20 (UTF-8, DEL) passes raw.
    void StringCjson(std::string_view value);
    /// cJSON print_number parity: int32-equal doubles via %d, else %1.15g
    /// kept when the fuzzy round-trip holds, else %1.17g; NaN/Inf -> null.
    void DoubleCjson(double value);

    bool HasError() const { return error_; }

    /// Accumulated JSON text (partial after an error).
    const std::string& Str() const { return out_; }

    /// Moves the accumulated text out (Str() is empty afterwards).
    std::string TakeStr() {
        std::string out = std::move(out_);
        out_.clear();
        return out;
    }

private:
    struct Frame {
        bool is_object;
        bool first;  // no element (array) / key-value pair (object) yet
    };

    bool PrepareValue();
    bool PrepareContainer();
    bool PrepareKey();
    void Fail(const char* why);

    std::string out_;
    Frame stack_[kMaxDepth];
    int depth_ = 0;
    bool expect_value_ = false;  // inside an object, right after Key()
    bool error_ = false;
};

}  // namespace tuya_crypto
