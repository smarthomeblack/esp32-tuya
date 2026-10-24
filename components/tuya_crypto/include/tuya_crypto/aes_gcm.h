#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace tuya_crypto {

/// AES-128-GCM raw encryption; on success `out` = ciphertext || 16-byte tag.
/// `key` must be 16 bytes (AES-128 only) and `nonce` 12 bytes.
bool AesGcmEncrypt(std::string_view key, std::string_view nonce, std::string_view plaintext,
                   std::string_view aad, std::vector<uint8_t>& out);

/// AES-128-GCM decrypt of ciphertext || 16-byte tag; returns false on tag mismatch.
bool AesGcmDecrypt(std::string_view key, std::string_view nonce, std::string_view ct_and_tag,
                   std::string_view aad, std::vector<uint8_t>& out);

/// Protocol "pack": secret's 16 hex chars are used as raw ASCII key bytes (not hex-decoded).
/// encdata = base64(nonce) + base64(ct||tag), concatenated with no separator.
bool AesGcmEncryptPack(const std::string& secret, const std::string& plaintext,
                       std::string& encdata);

/// Pack with explicit 12-char Tuya-alphabet nonce (self-test byte-exact vectors).
bool AesGcmEncryptPackWithNonce(const std::string& secret, const std::string& nonce,
                                const std::string& plaintext, std::string& encdata);

/// Protocol "unpack": base64-decode `encdata` as one blob, iv = blob[0:12], ct = blob[12:].
bool AesGcmDecryptPack(const std::string& secret, const std::string& encdata,
                       std::string& plaintext);

}  // namespace tuya_crypto
