// Internal helper shared by tuya_crypto sources. Not part of the public API.

#pragma once

#include <cstdint>
#include <string>

namespace tuya_crypto {

/// Append lowercase hex of `data` (2 * len chars); matches Python bytes.hex().
inline void AppendHexLower(std::string& out, const uint8_t* data, size_t len) {
    static constexpr char kHex[] = "0123456789abcdef";
    out.reserve(out.size() + len * 2);
    for (size_t i = 0; i < len; ++i) {
        out.push_back(kHex[data[i] >> 4]);
        out.push_back(kHex[data[i] & 0x0F]);
    }
}

}  // namespace tuya_crypto
