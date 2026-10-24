#pragma once

#include <string>

namespace tuya_crypto {

/// SHA-256 lowercase hex digest (64 chars) of the UTF-8 bytes of `input`.
bool Sha256Hex(const std::string& input, std::string& hex_out);

}  // namespace tuya_crypto
