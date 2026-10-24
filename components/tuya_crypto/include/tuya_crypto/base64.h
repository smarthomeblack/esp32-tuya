#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace tuya_crypto {

/// Standard base64 encode with '=' padding. `data` may be null only when `len` is 0.
bool Base64Encode(const uint8_t* data, size_t len, std::string& out);

/// Strict base64 decode (length % 4 == 0, trailing '=' fill, alphabet only); invalid input fails.
bool Base64Decode(const std::string& in, std::vector<uint8_t>& out);

}  // namespace tuya_crypto
