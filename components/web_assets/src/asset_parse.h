// Pure-RAM WAS1 validation (no flash, no global state) — shared by
// AssetStore init, the upload REST path and the boot self-test.

#pragma once

#include <cstddef>
#include <cstdint>

#include <esp_err.h>

#include "asset_format.h"

namespace web_assets {

/// A validated view over a WAS1 buffer (pointers reference that buffer).
struct WasImage {
    uint16_t version = 0;
    uint32_t file_count = 0;
    uint32_t payload_len = 0;
    uint32_t image_len = 0;
    const char* build_id = nullptr;    ///< NUL-terminated inside the buffer
    const WasTocEntry* toc = nullptr;  ///< file_count entries
    const uint8_t* payload = nullptr;  ///< payload region start
};

/// Full structural + CRC32 validation; max_len caps the size (0 = uncapped).
/// Any failure maps to one 400 invalid_asset at the REST layer.
esp_err_t WasValidate(const uint8_t* buf, size_t len, size_t max_len, WasImage* out,
                      const char** reason);

/// CRC-32 over the whole image minus the 4-byte crc field itself
/// (incremental ROM calls — matches web_asset_pack.image_crc).
uint32_t ImageCrc(const uint8_t* buf, size_t len);

}  // namespace web_assets
