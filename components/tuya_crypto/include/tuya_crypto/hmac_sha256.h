#pragma once

#include <string>

namespace tuya_crypto {

/// Raw HMAC-SHA256 (32 bytes). Empty keys rejected (PSA forbids zero-bit keys).
bool HmacSha256(const std::string& key, const std::string& msg, std::string& out);

/// HMAC-SHA256 lowercase hex digest (64 chars).
bool HmacSha256Hex(const std::string& key, const std::string& msg, std::string& hex_out);

}  // namespace tuya_crypto
