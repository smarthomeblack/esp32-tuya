#pragma once

#include <string>

namespace tuya_crypto {

/// MD5 lowercase hex digest (32 chars) of UTF-8 bytes. Tuya-legacy only (hash_key derivation).
bool Md5Hex(const std::string& input, std::string& hex_out);

}  // namespace tuya_crypto
