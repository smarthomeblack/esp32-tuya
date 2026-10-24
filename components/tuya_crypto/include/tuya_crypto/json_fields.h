#pragma once

/// Shared cJSON field readers + scoped RAII helpers (JsonText, JsonRoot).
/// Null-safe lookups: a missing key or wrong type yields the default.

#include <cJSON.h>

#include <cmath>
#include <cstdint>
#include <string>
#include <string_view>

namespace tuya_crypto {

// valuedouble is exact for |x| < 2^53; Tuya timestamps (~1.7e12 ms) fit.
// Non-finite / out-of-int64 values yield 0 (the naive cast would be UB).
inline int64_t GetInt64Field(const cJSON* object, const char* name) {
    const cJSON* item = cJSON_GetObjectItemCaseSensitive(object, name);
    if (cJSON_IsNumber(item)) {
        const double value = item->valuedouble;
        if (std::isfinite(value) && value == std::floor(value) &&
            value >= static_cast<double>(INT64_MIN) && value < static_cast<double>(INT64_MAX)) {
            return static_cast<int64_t>(value);
        }
    }
    return 0;
}

inline std::string GetStringField(const cJSON* object, const char* name) {
    const cJSON* item = cJSON_GetObjectItemCaseSensitive(object, name);
    if (cJSON_IsString(item) && item->valuestring != nullptr) {
        return item->valuestring;
    }
    return "";
}

inline bool GetBoolField(const cJSON* object, const char* name) {
    const cJSON* item = cJSON_GetObjectItemCaseSensitive(object, name);
    if (cJSON_IsBool(item)) {
        return cJSON_IsTrue(item);
    }
    return false;
}

/// RAII cJSON node -> compact JSON text (empty when null/print fails).
class JsonText {
public:
    explicit JsonText(const cJSON* node)
        : text_(node != nullptr ? cJSON_PrintUnformatted(node) : nullptr) {}
    ~JsonText() {
        if (text_ != nullptr) {
            cJSON_free(text_);
        }
    }
    JsonText(const JsonText&) = delete;
    JsonText& operator=(const JsonText&) = delete;

    std::string str() const { return text_ != nullptr ? std::string(text_) : std::string(); }

private:
    char* text_;
};

/// RAII guard: frees the whole parsed tree on scope exit (root owns children).
struct JsonRoot {
    cJSON* node = nullptr;
    ~JsonRoot() { cJSON_Delete(node); }
};

/// Parses text and owns the tree; true iff root is a non-null JSON object.
/// `out` must be fresh — any tree it still holds is freed first.
inline bool ParseJsonObject(std::string_view text, JsonRoot& out) {
    cJSON_Delete(out.node);
    out.node = cJSON_ParseWithLength(text.data(), text.size());
    return out.node != nullptr && cJSON_IsObject(out.node);
}

}  // namespace tuya_crypto
