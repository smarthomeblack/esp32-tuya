#include "tuya_crypto/md5.h"

#include <esp_log.h>
#include <psa/crypto.h>

#include <string>

#include "hex_util.h"

static const char* TAG = "Md5";

namespace tuya_crypto {

bool Md5Hex(const std::string& input, std::string& hex_out) {
    hex_out.clear();

    // psa_crypto_init() is idempotent; called defensively.
    psa_status_t st = psa_crypto_init();
    if (st != PSA_SUCCESS) {
        ESP_LOGE(TAG, "psa_crypto_init failed: %ld (0x%08lx)", static_cast<long>(st),
                 static_cast<unsigned long>(st));
        return false;
    }

    uint8_t digest[16] = {};
    size_t digest_len = 0;
    st = psa_hash_compute(PSA_ALG_MD5, reinterpret_cast<const uint8_t*>(input.data()), input.size(),
                          digest, sizeof(digest), &digest_len);
    if (st != PSA_SUCCESS) {
        ESP_LOGE(TAG, "psa_hash_compute(PSA_ALG_MD5) failed: %ld (0x%08lx)", static_cast<long>(st),
                 static_cast<unsigned long>(st));
        return false;
    }
    if (digest_len != sizeof(digest)) {
        ESP_LOGE(TAG, "unexpected MD5 digest length %u", static_cast<unsigned>(digest_len));
        return false;
    }

    AppendHexLower(hex_out, digest, sizeof(digest));
    return true;
}

}  // namespace tuya_crypto
