#include "tuya_crypto/uuid.h"

#include <esp_random.h>

#include <cstdint>
#include <string>

namespace tuya_crypto {

std::string Uuid4() {
    uint8_t b[16];
    esp_fill_random(b, sizeof(b));
    // RFC 4122: version 4, variant 10xx.
    b[6] = static_cast<uint8_t>((b[6] & 0x0F) | 0x40);
    b[8] = static_cast<uint8_t>((b[8] & 0x3F) | 0x80);

    static constexpr char kHex[] = "0123456789abcdef";
    std::string out;
    out.reserve(36);
    for (int i = 0; i < 16; ++i) {
        // Dashes before bytes 4, 6, 8, 10 -> 8-4-4-4-12 layout.
        if (i == 4 || i == 6 || i == 8 || i == 10) {
            out.push_back('-');
        }
        out.push_back(kHex[b[i] >> 4]);
        out.push_back(kHex[b[i] & 0x0F]);
    }
    return out;
}

}  // namespace tuya_crypto
