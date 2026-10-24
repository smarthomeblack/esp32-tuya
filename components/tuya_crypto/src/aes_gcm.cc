#include "tuya_crypto/aes_gcm.h"

#include <esp_log.h>
#include <esp_random.h>
#include <psa/crypto.h>

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "tuya_crypto/base64.h"

static const char* TAG = "AesGcm";

namespace tuya_crypto {

namespace {

constexpr size_t kAes128KeyLen = 16;  // 16 hex chars used as raw ASCII bytes
constexpr size_t kNonceLen = 12;      // 12 ASCII bytes -> GCM IV
constexpr size_t kGcmTagLen = 16;     // PSA_ALG_GCM default tag length

// Tuya 48-char nonce alphabet.
constexpr char kNonceAlphabet[] = "ABCDEFGHJKMNPQRSTWXYZabcdefhijkmnprstwxyz2345678";
constexpr size_t kNonceAlphabetSize = sizeof(kNonceAlphabet) - 1;
static_assert(kNonceAlphabetSize == 48, "Tuya nonce alphabet must be 48 chars");

void LogPsaError(const char* what, psa_status_t st) {
    ESP_LOGE(TAG, "%s failed: %ld (0x%08lx)", what, static_cast<long>(st),
             static_cast<unsigned long>(st));
}

// Modulo bias is benign: nonce is a public per-message value.
std::string RandomNonce() {
    uint8_t raw[kNonceLen];
    esp_fill_random(raw, sizeof(raw));
    std::string nonce;
    nonce.reserve(kNonceLen);
    for (size_t i = 0; i < kNonceLen; ++i) {
        nonce.push_back(kNonceAlphabet[raw[i] % kNonceAlphabetSize]);
    }
    return nonce;
}

bool ImportAesKey(std::string_view key, mbedtls_svc_key_id_t* key_id) {
    psa_key_attributes_t attrs = PSA_KEY_ATTRIBUTES_INIT;
    psa_set_key_type(&attrs, PSA_KEY_TYPE_AES);
    psa_set_key_bits(&attrs, kAes128KeyLen * 8);
    psa_set_key_usage_flags(&attrs, PSA_KEY_USAGE_ENCRYPT | PSA_KEY_USAGE_DECRYPT);
    psa_set_key_algorithm(&attrs, PSA_ALG_GCM);

    psa_status_t st =
        psa_import_key(&attrs, reinterpret_cast<const uint8_t*>(key.data()), key.size(), key_id);
    if (st != PSA_SUCCESS) {
        LogPsaError("psa_import_key(PSA_KEY_TYPE_AES)", st);
        return false;
    }
    return true;
}

}  // namespace

bool AesGcmEncrypt(std::string_view key, std::string_view nonce, std::string_view plaintext,
                   std::string_view aad, std::vector<uint8_t>& out) {
    out.clear();
    if (key.size() != kAes128KeyLen) {
        ESP_LOGE(TAG, "key must be exactly 16 bytes (AES-128), got %u",
                 static_cast<unsigned>(key.size()));
        return false;
    }
    if (nonce.size() != kNonceLen) {
        ESP_LOGE(TAG, "nonce must be exactly 12 bytes, got %u",
                 static_cast<unsigned>(nonce.size()));
        return false;
    }

    psa_status_t st = psa_crypto_init();
    if (st != PSA_SUCCESS) {
        LogPsaError("psa_crypto_init", st);
        return false;
    }

    mbedtls_svc_key_id_t key_id = 0;
    if (!ImportAesKey(key, &key_id)) {
        return false;
    }

    out.resize(plaintext.size() + kGcmTagLen);
    size_t out_len = 0;
    // Zero-length string_view may legally have a null data(); pass a dummy pointer.
    const uint8_t dummy = 0;
    const uint8_t* pt =
        plaintext.empty() ? &dummy : reinterpret_cast<const uint8_t*>(plaintext.data());
    const uint8_t* ad = aad.empty() ? &dummy : reinterpret_cast<const uint8_t*>(aad.data());
    st = psa_aead_encrypt(key_id, PSA_ALG_GCM, reinterpret_cast<const uint8_t*>(nonce.data()),
                          nonce.size(), ad, aad.size(), pt, plaintext.size(), out.data(),
                          out.size(), &out_len);
    psa_destroy_key(key_id);
    if (st != PSA_SUCCESS) {
        LogPsaError("psa_aead_encrypt(PSA_ALG_GCM)", st);
        out.clear();
        return false;
    }
    out.resize(out_len);
    return true;
}

bool AesGcmDecrypt(std::string_view key, std::string_view nonce, std::string_view ct_and_tag,
                   std::string_view aad, std::vector<uint8_t>& out) {
    out.clear();
    if (key.size() != kAes128KeyLen) {
        ESP_LOGE(TAG, "key must be exactly 16 bytes (AES-128), got %u",
                 static_cast<unsigned>(key.size()));
        return false;
    }
    if (nonce.size() != kNonceLen) {
        ESP_LOGE(TAG, "nonce must be exactly 12 bytes, got %u",
                 static_cast<unsigned>(nonce.size()));
        return false;
    }
    if (ct_and_tag.size() < kGcmTagLen) {
        ESP_LOGE(TAG, "ciphertext+tag too short: %u bytes (need >= 16)",
                 static_cast<unsigned>(ct_and_tag.size()));
        return false;
    }

    psa_status_t st = psa_crypto_init();
    if (st != PSA_SUCCESS) {
        LogPsaError("psa_crypto_init", st);
        return false;
    }

    mbedtls_svc_key_id_t key_id = 0;
    if (!ImportAesKey(key, &key_id)) {
        return false;
    }

    const size_t pt_len = ct_and_tag.size() - kGcmTagLen;
    out.resize(pt_len);
    size_t out_len = 0;
    uint8_t dummy = 0;
    uint8_t* pt = pt_len == 0 ? &dummy : out.data();
    const uint8_t* ad = aad.empty() ? &dummy : reinterpret_cast<const uint8_t*>(aad.data());
    st = psa_aead_decrypt(key_id, PSA_ALG_GCM, reinterpret_cast<const uint8_t*>(nonce.data()),
                          nonce.size(), ad, aad.size(),
                          reinterpret_cast<const uint8_t*>(ct_and_tag.data()), ct_and_tag.size(),
                          pt, pt_len, &out_len);
    psa_destroy_key(key_id);
    if (st != PSA_SUCCESS) {
        if (st == PSA_ERROR_INVALID_SIGNATURE) {
            ESP_LOGE(TAG, "GCM authentication failed (wrong key or corrupted data)");
        } else {
            LogPsaError("psa_aead_decrypt(PSA_ALG_GCM)", st);
        }
        out.clear();
        return false;
    }
    out.resize(out_len);
    return true;
}

bool AesGcmEncryptPackWithNonce(const std::string& secret, const std::string& nonce,
                                const std::string& plaintext, std::string& encdata) {
    encdata.clear();
    if (secret.size() != kAes128KeyLen) {
        ESP_LOGE(TAG, "secret must be 16 hex chars (AES-128 key), got %u",
                 static_cast<unsigned>(secret.size()));
        return false;
    }
    if (nonce.size() != kNonceLen) {
        ESP_LOGE(TAG, "nonce must be 12 chars, got %u", static_cast<unsigned>(nonce.size()));
        return false;
    }

    std::vector<uint8_t> ct;
    if (!AesGcmEncrypt(secret, nonce, plaintext, std::string_view(), ct)) {
        return false;
    }

    std::string b64_nonce;
    std::string b64_ct;
    if (!Base64Encode(reinterpret_cast<const uint8_t*>(nonce.data()), nonce.size(), b64_nonce) ||
        !Base64Encode(ct.data(), ct.size(), b64_ct)) {
        return false;
    }
    encdata = b64_nonce + b64_ct;
    return true;
}

bool AesGcmEncryptPack(const std::string& secret, const std::string& plaintext,
                       std::string& encdata) {
    return AesGcmEncryptPackWithNonce(secret, RandomNonce(), plaintext, encdata);
}

bool AesGcmDecryptPack(const std::string& secret, const std::string& encdata,
                       std::string& plaintext) {
    plaintext.clear();

    std::vector<uint8_t> blob;
    if (!Base64Decode(encdata, blob)) {
        return false;  // already logged
    }
    if (blob.size() < kNonceLen + kGcmTagLen) {
        ESP_LOGE(TAG, "encdata blob too short: %u bytes (need >= 28)",
                 static_cast<unsigned>(blob.size()));
        return false;
    }

    const std::string_view secret_view(secret);
    const std::string_view nonce(reinterpret_cast<const char*>(blob.data()), kNonceLen);
    const std::string_view ct(reinterpret_cast<const char*>(blob.data()) + kNonceLen,
                              blob.size() - kNonceLen);
    std::vector<uint8_t> pt;
    if (!AesGcmDecrypt(secret_view, nonce, ct, std::string_view(), pt)) {
        return false;
    }
    plaintext.assign(reinterpret_cast<const char*>(pt.data()), pt.size());
    return true;
}

}  // namespace tuya_crypto
