#include "tuya_crypto/hmac_sha256.h"

#include <esp_log.h>
#include <psa/crypto.h>

#include <string>

#include "hex_util.h"

static const char* TAG = "HmacSha256";

namespace tuya_crypto {

bool HmacSha256(const std::string& key, const std::string& msg, std::string& out) {
    out.clear();

    if (key.empty()) {
        // PSA rejects zero-bit keys.
        ESP_LOGE(TAG, "empty HMAC key");
        return false;
    }

    psa_status_t st = psa_crypto_init();
    if (st != PSA_SUCCESS) {
        ESP_LOGE(TAG, "psa_crypto_init failed: %ld (0x%08lx)", static_cast<long>(st),
                 static_cast<unsigned long>(st));
        return false;
    }

    // Volatile key imported per operation and destroyed right after use.
    psa_key_attributes_t attrs = PSA_KEY_ATTRIBUTES_INIT;
    psa_set_key_type(&attrs, PSA_KEY_TYPE_HMAC);
    psa_set_key_bits(&attrs, static_cast<psa_key_bits_t>(key.size() * 8));
    psa_set_key_usage_flags(&attrs, PSA_KEY_USAGE_SIGN_MESSAGE);
    psa_set_key_algorithm(&attrs, PSA_ALG_HMAC(PSA_ALG_SHA_256));

    mbedtls_svc_key_id_t key_id = 0;
    st = psa_import_key(&attrs, reinterpret_cast<const uint8_t*>(key.data()), key.size(), &key_id);
    if (st != PSA_SUCCESS) {
        ESP_LOGE(TAG, "psa_import_key(PSA_KEY_TYPE_HMAC) failed: %ld (0x%08lx)",
                 static_cast<long>(st), static_cast<unsigned long>(st));
        return false;
    }

    uint8_t mac[32] = {};
    size_t mac_len = 0;
    st = psa_mac_compute(key_id, PSA_ALG_HMAC(PSA_ALG_SHA_256),
                         reinterpret_cast<const uint8_t*>(msg.data()), msg.size(), mac, sizeof(mac),
                         &mac_len);
    psa_destroy_key(key_id);
    if (st != PSA_SUCCESS) {
        ESP_LOGE(TAG, "psa_mac_compute(HMAC-SHA256) failed: %ld (0x%08lx)", static_cast<long>(st),
                 static_cast<unsigned long>(st));
        return false;
    }
    if (mac_len != sizeof(mac)) {
        ESP_LOGE(TAG, "unexpected HMAC-SHA256 length %u", static_cast<unsigned>(mac_len));
        return false;
    }

    out.assign(reinterpret_cast<const char*>(mac), mac_len);
    return true;
}

bool HmacSha256Hex(const std::string& key, const std::string& msg, std::string& hex_out) {
    hex_out.clear();
    std::string raw;
    if (!HmacSha256(key, msg, raw)) {
        return false;
    }
    AppendHexLower(hex_out, reinterpret_cast<const uint8_t*>(raw.data()), raw.size());
    return true;
}

}  // namespace tuya_crypto
