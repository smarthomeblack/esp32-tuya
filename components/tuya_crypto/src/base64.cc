#include "tuya_crypto/base64.h"

#include <esp_log.h>

#include <array>
#include <string>
#include <vector>

static const char* TAG = "Base64";

namespace tuya_crypto {

namespace {

constexpr char kEncodeAlphabet[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

// Reverse lookup: 6-bit value per alphabet byte, 0xFF for everything else.
constexpr auto kDecodeTable = [] {
    std::array<uint8_t, 256> t{};
    t.fill(0xFF);
    for (int i = 0; i < 64; ++i) {
        t[static_cast<uint8_t>(kEncodeAlphabet[i])] = static_cast<uint8_t>(i);
    }
    return t;
}();

inline uint8_t DecodeChar(char c) {
    return kDecodeTable[static_cast<uint8_t>(c)];
}

}  // namespace

bool Base64Encode(const uint8_t* data, size_t len, std::string& out) {
    out.clear();
    if (data == nullptr && len != 0) {
        ESP_LOGE(TAG, "null data with length %u", static_cast<unsigned>(len));
        return false;
    }
    out.reserve((len + 2) / 3 * 4);

    size_t i = 0;
    for (; i + 3 <= len; i += 3) {
        const uint32_t v = (static_cast<uint32_t>(data[i]) << 16) |
                           (static_cast<uint32_t>(data[i + 1]) << 8) | data[i + 2];
        out.push_back(kEncodeAlphabet[(v >> 18) & 0x3F]);
        out.push_back(kEncodeAlphabet[(v >> 12) & 0x3F]);
        out.push_back(kEncodeAlphabet[(v >> 6) & 0x3F]);
        out.push_back(kEncodeAlphabet[v & 0x3F]);
    }
    if (len - i == 1) {
        const uint32_t v = static_cast<uint32_t>(data[i]) << 16;
        out.push_back(kEncodeAlphabet[(v >> 18) & 0x3F]);
        out.push_back(kEncodeAlphabet[(v >> 12) & 0x3F]);
        out.push_back('=');
        out.push_back('=');
    } else if (len - i == 2) {
        const uint32_t v =
            (static_cast<uint32_t>(data[i]) << 16) | (static_cast<uint32_t>(data[i + 1]) << 8);
        out.push_back(kEncodeAlphabet[(v >> 18) & 0x3F]);
        out.push_back(kEncodeAlphabet[(v >> 12) & 0x3F]);
        out.push_back(kEncodeAlphabet[(v >> 6) & 0x3F]);
        out.push_back('=');
    }
    return true;
}

bool Base64Decode(const std::string& in, std::vector<uint8_t>& out) {
    out.clear();
    const size_t n = in.size();
    if (n == 0) {
        return true;
    }
    if (n % 4 != 0) {
        ESP_LOGE(TAG, "base64 length %u is not a multiple of 4", static_cast<unsigned>(n));
        return false;
    }

    size_t pad = 0;
    while (pad < 2 && in[n - 1 - pad] == '=') {
        ++pad;
    }
    const size_t data_chars = n - pad;

    // Validate every non-padding character; embedded '=' is invalid.
    for (size_t i = 0; i < data_chars; ++i) {
        if (in[i] == '=') {
            ESP_LOGE(TAG, "embedded '=' at index %u", static_cast<unsigned>(i));
            return false;
        }
        if (DecodeChar(in[i]) == 0xFF) {
            ESP_LOGE(TAG, "invalid base64 character 0x%02x at index %u",
                     static_cast<unsigned>(static_cast<uint8_t>(in[i])), static_cast<unsigned>(i));
            return false;
        }
    }

    out.reserve(data_chars / 4 * 3 + 2);

    size_t i = 0;
    for (; i + 4 <= data_chars; i += 4) {
        const uint32_t v = (static_cast<uint32_t>(DecodeChar(in[i])) << 18) |
                           (static_cast<uint32_t>(DecodeChar(in[i + 1])) << 12) |
                           (static_cast<uint32_t>(DecodeChar(in[i + 2])) << 6) |
                           DecodeChar(in[i + 3]);
        out.push_back(static_cast<uint8_t>(v >> 16));
        out.push_back(static_cast<uint8_t>(v >> 8));
        out.push_back(static_cast<uint8_t>(v));
    }
    // Remainder: 2 data chars -> 1 byte; 3 data chars -> 2 bytes.
    if (data_chars - i == 2) {
        const uint32_t v = (static_cast<uint32_t>(DecodeChar(in[i])) << 18) |
                           (static_cast<uint32_t>(DecodeChar(in[i + 1])) << 12);
        out.push_back(static_cast<uint8_t>(v >> 16));
    } else if (data_chars - i == 3) {
        const uint32_t v = (static_cast<uint32_t>(DecodeChar(in[i])) << 18) |
                           (static_cast<uint32_t>(DecodeChar(in[i + 1])) << 12) |
                           (static_cast<uint32_t>(DecodeChar(in[i + 2])) << 6);
        out.push_back(static_cast<uint8_t>(v >> 16));
        out.push_back(static_cast<uint8_t>(v >> 8));
    }
    return true;
}

}  // namespace tuya_crypto
