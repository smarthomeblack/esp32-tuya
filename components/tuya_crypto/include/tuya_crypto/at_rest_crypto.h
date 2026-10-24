#pragma once

#include <string>
#include <string_view>

namespace tuya_crypto {

/// True when the eFuse-derived AES-256 key is loaded and sealing is available.
bool AtRestKeyReady();

/// Dev-mode provisioning: burns a random HMAC key into the free eFuse key
/// block on first boot, then derives and caches the AES-256 key. Idempotent;
/// false leaves sealing disabled (loads of legacy files still work).
bool InitAtRestKey();

/// Seals `plaintext` into the ENC1 container (see at_rest_crypto.cc for the
/// byte layout); false leaves `out` empty.
bool AtRestSeal(std::string_view plaintext, std::string& out);

/// Opens an ENC1 container sealed with the eFuse-derived key; false on wrong
/// key or corruption (`plaintext` stays empty).
bool AtRestOpen(std::string_view blob, std::string& plaintext);

/// True when `blob` carries the ENC1 magic (legacy plaintext files do not).
bool AtRestIsEncrypted(std::string_view blob);

/// Explicit-key ENC1 container primitives — the AtRestSeal/AtRestOpen core.
bool Aes256GcmSeal(std::string_view key32, std::string_view plaintext, std::string& out);
bool Aes256GcmOpen(std::string_view key32, std::string_view blob, std::string& plaintext);

}  // namespace tuya_crypto
