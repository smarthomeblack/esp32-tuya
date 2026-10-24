// At-rest encryption helper: AES-256-GCM container keyed by an eFuse HMAC key.
//
// File layout ("ENC1" container): "ENC1" (4) + random IV (12) + GCM
// ciphertext (plaintext length) + GCM tag (16).

#include "tuya_crypto/at_rest_crypto.h"

#include <bootloader_random.h>
#include <esp_efuse.h>
#include <esp_hmac.h>
#include <esp_log.h>
#include <esp_random.h>
#include <psa/crypto.h>

#include <array>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <string>
#include <string_view>

static const char* TAG = "AtRest";

namespace tuya_crypto {

namespace {

constexpr size_t kAes256KeyLen = 32;
constexpr size_t kIvLen = 12;
constexpr size_t kTagLen = 16;  // PSA_ALG_GCM default tag length
constexpr size_t kMagicLen = 4;
constexpr size_t kHeaderLen = kMagicLen + kIvLen;
constexpr char kMagic[kMagicLen] = {'E', 'N', 'C', '1'};

// EFUSE_BLK_KEY1 (HMAC_UP): KEY0 is owned by the NVS encryption scheme; this
// hub burns nothing else, so KEY1 is free. One-way by design.
constexpr hmac_key_id_t kHmacKeyId = HMAC_KEY1;
constexpr char kKeyLabel[] = "registry-aes-v1";

std::mutex g_init_mutex;
std::atomic<bool> g_key_ready{false};
std::array<uint8_t, kAes256KeyLen> g_key{};

void LogPsaError(const char* what, psa_status_t st) {
    ESP_LOGE(TAG, "%s failed: %ld (0x%08lx)", what, static_cast<long>(st),
             static_cast<unsigned long>(st));
}

bool ImportAesKey(std::string_view key, mbedtls_svc_key_id_t* key_id) {
    psa_key_attributes_t attrs = PSA_KEY_ATTRIBUTES_INIT;
    psa_set_key_type(&attrs, PSA_KEY_TYPE_AES);
    psa_set_key_bits(&attrs, kAes256KeyLen * 8);
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

bool BurnKeyIfMissing() {
    if (esp_efuse_get_key_purpose(EFUSE_BLK_KEY1) == ESP_EFUSE_KEY_PURPOSE_HMAC_UP) {
        return true;  // burnt on an earlier boot
    }
    if (!esp_efuse_key_block_unused(EFUSE_BLK_KEY1)) {
        ESP_LOGE(TAG, "eFuse KEY1 is not empty and not HMAC — at-rest key unavailable");
        return false;
    }
    // Bootloader RNG: the only validated entropy source before Wi-Fi/BT start.
    uint8_t raw[kAes256KeyLen];
    bootloader_random_enable();
    esp_fill_random(raw, sizeof(raw));
    bootloader_random_disable();
    esp_err_t err =
        esp_efuse_write_key(EFUSE_BLK_KEY1, ESP_EFUSE_KEY_PURPOSE_HMAC_UP, raw, sizeof(raw));
    std::memset(raw, 0, sizeof(raw));
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_efuse_write_key(KEY1): %s", esp_err_to_name(err));
        return false;
    }
    ESP_LOGI(TAG, "At-rest eFuse HMAC key burnt into KEY1 (one-way)");
    return true;
}

std::string_view CachedKey() {
    return std::string_view(reinterpret_cast<const char*>(g_key.data()), g_key.size());
}

}  // namespace

bool InitAtRestKey() {
    std::lock_guard<std::mutex> lock(g_init_mutex);
    if (g_key_ready.load(std::memory_order_acquire)) {
        return true;
    }
    if (!BurnKeyIfMissing()) {
        return false;
    }
    uint8_t derived[kAes256KeyLen];
    esp_err_t err = esp_hmac_calculate(kHmacKeyId, kKeyLabel, sizeof(kKeyLabel) - 1, derived);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_hmac_calculate: %s", esp_err_to_name(err));
        return false;
    }
    std::memcpy(g_key.data(), derived, sizeof(derived));
    std::memset(derived, 0, sizeof(derived));
    g_key_ready.store(true, std::memory_order_release);
    return true;
}

bool AtRestKeyReady() {
    return g_key_ready.load(std::memory_order_acquire);
}

bool Aes256GcmSeal(std::string_view key32, std::string_view plaintext, std::string& out) {
    out.clear();
    if (key32.size() != kAes256KeyLen) {
        ESP_LOGE(TAG, "key must be exactly 32 bytes (AES-256), got %u",
                 static_cast<unsigned>(key32.size()));
        return false;
    }

    psa_status_t st = psa_crypto_init();
    if (st != PSA_SUCCESS) {
        LogPsaError("psa_crypto_init", st);
        return false;
    }

    mbedtls_svc_key_id_t key_id = 0;
    if (!ImportAesKey(key32, &key_id)) {
        return false;
    }

    uint8_t iv[kIvLen];
    esp_fill_random(iv, sizeof(iv));

    out.resize(kHeaderLen + plaintext.size() + kTagLen);
    std::memcpy(out.data(), kMagic, kMagicLen);
    std::memcpy(out.data() + kMagicLen, iv, kIvLen);

    // Zero-length string_view may legally have a null data(); pass a dummy.
    const uint8_t dummy = 0;
    const uint8_t* pt =
        plaintext.empty() ? &dummy : reinterpret_cast<const uint8_t*>(plaintext.data());
    size_t out_len = 0;
    st = psa_aead_encrypt(key_id, PSA_ALG_GCM, iv, kIvLen, nullptr, 0, pt, plaintext.size(),
                          reinterpret_cast<uint8_t*>(out.data()) + kHeaderLen,
                          out.size() - kHeaderLen, &out_len);
    psa_destroy_key(key_id);
    if (st != PSA_SUCCESS) {
        LogPsaError("psa_aead_encrypt(PSA_ALG_GCM)", st);
        out.clear();
        return false;
    }
    out.resize(kHeaderLen + out_len);
    return true;
}

bool Aes256GcmOpen(std::string_view key32, std::string_view blob, std::string& plaintext) {
    plaintext.clear();
    if (key32.size() != kAes256KeyLen) {
        ESP_LOGE(TAG, "key must be exactly 32 bytes (AES-256), got %u",
                 static_cast<unsigned>(key32.size()));
        return false;
    }
    if (blob.size() < kHeaderLen + kTagLen) {
        ESP_LOGE(TAG, "container too short: %u bytes (need >= %u)",
                 static_cast<unsigned>(blob.size()), static_cast<unsigned>(kHeaderLen + kTagLen));
        return false;
    }
    if (std::memcmp(blob.data(), kMagic, kMagicLen) != 0) {
        ESP_LOGE(TAG, "container magic mismatch (not an ENC1 file)");
        return false;
    }

    psa_status_t st = psa_crypto_init();
    if (st != PSA_SUCCESS) {
        LogPsaError("psa_crypto_init", st);
        return false;
    }

    mbedtls_svc_key_id_t key_id = 0;
    if (!ImportAesKey(key32, &key_id)) {
        return false;
    }

    const size_t ct_len = blob.size() - kHeaderLen - kTagLen;
    plaintext.resize(ct_len);
    const uint8_t* iv = reinterpret_cast<const uint8_t*>(blob.data()) + kMagicLen;
    const uint8_t* ct = reinterpret_cast<const uint8_t*>(blob.data()) + kHeaderLen;
    uint8_t dummy = 0;
    uint8_t* pt = ct_len == 0 ? &dummy : reinterpret_cast<uint8_t*>(plaintext.data());
    size_t out_len = 0;
    st = psa_aead_decrypt(key_id, PSA_ALG_GCM, iv, kIvLen, nullptr, 0, ct, blob.size() - kHeaderLen,
                          pt, ct_len, &out_len);
    psa_destroy_key(key_id);
    if (st != PSA_SUCCESS) {
        if (st == PSA_ERROR_INVALID_SIGNATURE) {
            ESP_LOGE(TAG, "GCM authentication failed (wrong key or corrupted data)");
        } else {
            LogPsaError("psa_aead_decrypt(PSA_ALG_GCM)", st);
        }
        plaintext.clear();
        return false;
    }
    plaintext.resize(out_len);
    return true;
}

bool AtRestSeal(std::string_view plaintext, std::string& out) {
    if (!AtRestKeyReady()) {
        ESP_LOGE(TAG, "at-rest key not initialised — cannot seal");
        return false;
    }
    return Aes256GcmSeal(CachedKey(), plaintext, out);
}

bool AtRestOpen(std::string_view blob, std::string& plaintext) {
    if (!AtRestKeyReady()) {
        ESP_LOGE(TAG, "at-rest key not initialised — cannot open");
        return false;
    }
    return Aes256GcmOpen(CachedKey(), blob, plaintext);
}

bool AtRestIsEncrypted(std::string_view blob) {
    return blob.size() >= kMagicLen && std::memcmp(blob.data(), kMagic, kMagicLen) == 0;
}

}  // namespace tuya_crypto
