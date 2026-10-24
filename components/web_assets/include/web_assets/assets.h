/** WAS1 asset store — web UI in the "assets" flash partition; ~2 KB
 *  recovery page stays embedded in the app. */

#ifndef WEB_ASSETS_ASSETS_H
#define WEB_ASSETS_ASSETS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <esp_err.h>

#ifdef __cplusplus
extern "C" {
#endif

/** One asset entry: URL-path name, gzip payload, payload size. */
typedef struct WebAsset {
    const char* name;
    const unsigned char* data;
    size_t size;
} WebAsset;

/** Snapshot for /api/asset/info (all zeros when the store is invalid). */
typedef struct WebAssetInfo {
    bool valid;             ///< image mapped + CRC verified
    uint16_t version;       ///< WAS1 version (1)
    char build_id[17];      ///< NUL-terminated "w" + sha256(payload)[:11]
    uint32_t files;         ///< TOC entry count
    uint32_t payload_len;   ///< payload region bytes
    size_t partition_size;  ///< "assets" partition bytes (0 = missing)
} WebAssetInfo;

/** Find by URL path name ("index.html"; leading '/' tolerated); NULL
 *  when absent/invalid — the caller serves the embedded fallback page.
 *  The result is valid only until the next store mutation (upload
 *  remount); serve and upload share the single httpd worker task. */
const WebAsset* FindAsset(const char* name);

/** Map + verify the "assets" partition (idempotent); ESP_ERR_NOT_FOUND
 *  when absent/invalid — the fallback page still serves. */
esp_err_t WebAssetsInit(void);

/** Copies the current store snapshot (safe any time after boot). */
void WebAssetsGetInfo(WebAssetInfo* out);

/** Embedded ~2 KB gzip recovery page for unmatched non-API paths;
 *  always non-NULL. */
const WebAsset* GetAssetFallbackPage(void);

/** Validate a WAS1 image in RAM (image_len must equal len; partition_size
 *  caps it, 0 = uncapped); optionally fills *out. */
bool AssetImageValidateBuf(const uint8_t* buf, size_t len, size_t partition_size,
                           WebAssetInfo* out);

/** Validate, flash + re-init the store: body first, 40-byte header LAST
 *  (a partial write never yields a valid image); no reboot. */
esp_err_t AssetImageFlashBuf(const uint8_t* buf, size_t len);

#ifdef __cplusplus
}
#endif

#endif /* WEB_ASSETS_ASSETS_H */
