// WAS1 asset store: partition mmap, TOC cache, validate-then-flash upload.
// Init runs before httpd_start; the mutex guards the upload re-init path.

#include "web_assets/assets.h"

#include <esp_log.h>
#include <esp_partition.h>
#include <esp_rom_crc.h>
#include <spi_flash_mmap.h>

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <deque>
#include <mutex>
#include <string>
#include <vector>

#include "asset_format.h"
#include "asset_parse.h"

static const char* TAG = "AssetStore";

// The embedded recovery page (EMBED_FILES dist/fallback.html.gz — the
// symbol derives from the flat dist FILE NAME).
extern const unsigned char fallback_html_gz_start[] __asm__("_binary_fallback_html_gz_start");
extern const unsigned char fallback_html_gz_end[] __asm__("_binary_fallback_html_gz_end");

namespace web_assets {

// Internal store state (definitions local to this translation unit; the
// extern "C" API below is the only public surface).
struct StoreState {
    const esp_partition_t* partition = nullptr;
    esp_partition_mmap_handle_t mmap_handle = 0;
    const uint8_t* mapped = nullptr;  ///< image start in the address space
    bool valid = false;               ///< header + TOC + CRC verified
    WasHeader header = {};
    std::vector<WasTocEntry> toc;  ///< heap copy of the TOC
    /// deque (not vector): c_str() pointers in `entries` stay stable
    /// across push_back — a vector realloc would dangle them.
    std::deque<std::string> names;  ///< NUL-safe name copies, parallel
    std::vector<WebAsset> entries;  ///< served views into the mmap
};

std::mutex g_store_mutex;
StoreState g_store;

WebAsset g_fallback = {"fallback.html", fallback_html_gz_start,
                       static_cast<size_t>(fallback_html_gz_end - fallback_html_gz_start)};

/// CRC-32 over the whole image minus the 4-byte crc field itself —
/// matches web_asset_pack.image_crc (zlib incremental, ROM calls).
uint32_t ImageCrc(const uint8_t* buf, size_t len) {
    uint32_t crc = esp_rom_crc32_le(0, buf, kWasCrcOffset);
    return esp_rom_crc32_le(crc, buf + kWasCrcOffset + 4, len - (kWasCrcOffset + 4));
}

/// The single unmap site: the mapping is released before the fields drop.
static void ResetStore(StoreState& store) {
    if (store.mapped != nullptr) {
        esp_partition_munmap(store.mmap_handle);
    }
    store.valid = false;
    store.mapped = nullptr;
    store.mmap_handle = 0;
    store.header = {};
    store.toc.clear();
    store.names.clear();
    store.entries.clear();
}

/// TOC copy cap: a real image has tens of files; a crafted count is rejected
/// before toc.assign/reserve.
constexpr uint32_t kMaxFileCount = 1024;

/// Maps + verifies the partition image. `partition` must stay set even
/// on failure (the upload path flashes into it).
static void MountLocked() {
    ResetStore(g_store);
    if (g_store.partition == nullptr) {
        g_store.partition =
            esp_partition_find_first(ESP_PARTITION_TYPE_ANY, ESP_PARTITION_SUBTYPE_ANY, "assets");
    }
    if (g_store.partition == nullptr) {
        ESP_LOGW(TAG, "No \"assets\" partition — serving the embedded fallback page");
        return;
    }

    WasHeader header = {};
    const esp_err_t err = esp_partition_read(g_store.partition, 0, &header, sizeof(header));
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Header read failed: %s", esp_err_to_name(err));
        return;
    }
    const char* reason = nullptr;
    if (memcmp(header.magic, kWasMagic, sizeof(kWasMagic)) != 0 || header.version != kWasVersion) {
        ESP_LOGW(TAG, "Partition holds no WAS1 v%u image — serving the fallback page",
                 static_cast<unsigned>(kWasVersion));
        return;
    }
    // Bound both counts before the page math: 0xFFFFFFFF would wrap map_len.
    if (header.image_len > g_store.partition->size) {
        ESP_LOGW(TAG, "Header image_len %u exceeds the partition (%u B) — using the fallback page",
                 static_cast<unsigned>(header.image_len),
                 static_cast<unsigned>(g_store.partition->size));
        return;
    }
    if (header.file_count > kMaxFileCount) {
        ESP_LOGW(TAG, "Header file_count %u over the %u cap — using the fallback page",
                 static_cast<unsigned>(header.file_count), static_cast<unsigned>(kMaxFileCount));
        return;
    }

    // Enough free MMU pages for the rounded-up mapping (+1 for the
    // round-down to the 64 KB page boundary of a non-aligned base).
    const size_t page = SPI_FLASH_MMU_PAGE_SIZE;
    const size_t map_len =
        (header.image_len + page - 1) / page * page + page;  // round up to a whole page, +1 spare
    const uint32_t free_pages = spi_flash_mmap_get_free_pages(SPI_FLASH_MMAP_DATA);
    if (free_pages < map_len / page) {
        ESP_LOGW(TAG, "Not enough MMU pages for %u B (%u free)", header.image_len, free_pages);
        return;
    }
    const void* mapped = nullptr;
    if (esp_partition_mmap(g_store.partition, 0, header.image_len, ESP_PARTITION_MMAP_DATA, &mapped,
                           &g_store.mmap_handle) != ESP_OK) {
        ESP_LOGW(TAG, "mmap of the asset image failed");
        return;
    }
    g_store.mapped = static_cast<const uint8_t*>(mapped);

    // Full validation over the mapped bytes (TOC bounds + CRC).
    WasImage image = {};
    if (WasValidate(g_store.mapped, header.image_len, g_store.partition->size, &image, &reason) !=
        ESP_OK) {
        ESP_LOGW(TAG, "Mapped image invalid: %s — serving the fallback page", reason);
        ResetStore(g_store);  // keeps partition, drops the mapping
        return;
    }

    g_store.header = header;
    g_store.toc.assign(image.toc, image.toc + image.file_count);
    g_store.entries.reserve(image.file_count);
    for (uint32_t i = 0; i < image.file_count; ++i) {
        g_store.names.emplace_back(g_store.toc[i].name);
        WebAsset entry = {};
        entry.name = g_store.names.back().c_str();
        entry.data = image.payload + g_store.toc[i].offset;
        entry.size = g_store.toc[i].size;
        g_store.entries.push_back(entry);
    }
    g_store.valid = true;
    ESP_LOGI(TAG, "Asset store ready: %u file(s), %u B payload, build_id=%.12s", image.file_count,
             image.payload_len, image.build_id);
}

static void FillInfo(const StoreState& store, WebAssetInfo* out) {
    memset(out, 0, sizeof(*out));
    out->valid = store.valid;
    out->partition_size = store.partition != nullptr ? store.partition->size : 0;
    if (!store.valid) {
        return;
    }
    out->version = store.header.version;
    out->files = store.header.file_count;
    out->payload_len = store.header.payload_len;
    snprintf(out->build_id, sizeof(out->build_id), "%.16s", store.header.build_id);
}

// ---- Pure validation (asset_parse.h) ----

esp_err_t WasValidate(const uint8_t* buf, size_t len, size_t max_len, WasImage* out,
                      const char** reason) {
    auto fail = [&](esp_err_t code, const char* why) {
        if (reason != nullptr) {
            *reason = why;
        }
        return code;
    };
    if (buf == nullptr) {
        return fail(ESP_ERR_INVALID_ARG, "null buffer");
    }
    if (len < kWasHeaderSize) {
        return fail(ESP_ERR_INVALID_SIZE, "shorter than the 40-byte header");
    }
    WasHeader header = {};
    memcpy(&header, buf, sizeof(header));
    if (memcmp(header.magic, kWasMagic, sizeof(kWasMagic)) != 0) {
        return fail(ESP_ERR_INVALID_STATE, "bad magic (not WAS1)");
    }
    if (header.version != kWasVersion) {
        return fail(ESP_ERR_INVALID_STATE, "unsupported WAS1 version");
    }
    if (header.image_len != len) {
        return fail(ESP_ERR_INVALID_SIZE, "image_len does not match the buffer length");
    }
    if (max_len != 0 && len > max_len) {
        return fail(ESP_ERR_INVALID_SIZE, "image exceeds the partition size");
    }
    // Bound the count before the multiply: 76 * file_count wraps a
    // 32-bit size_t and a crafted count walks the TOC OOB.
    if (header.file_count > (len - kWasHeaderSize) / kWasTocEntrySize) {
        return fail(ESP_ERR_INVALID_SIZE, "TOC overruns the image");
    }
    const size_t toc_len = kWasTocEntrySize * static_cast<size_t>(header.file_count);
    if (kWasHeaderSize + toc_len > len) {
        return fail(ESP_ERR_INVALID_SIZE, "TOC overruns the image");
    }
    // No wrap: the TOC bound above holds kWasHeaderSize + toc_len <= len.
    const size_t room = len - (kWasHeaderSize + toc_len);
    if (header.payload_len > room || room - header.payload_len >= 4) {
        // 0..3 zero tail-pad bytes close the image (word alignment).
        return fail(ESP_ERR_INVALID_SIZE, "payload_len does not close the image");
    }
    const WasTocEntry* toc = reinterpret_cast<const WasTocEntry*>(buf + kWasHeaderSize);
    for (uint32_t i = 0; i < header.file_count; ++i) {
        const WasTocEntry& entry = toc[i];
        if (memchr(entry.name, '\0', kWasNameSize) == nullptr) {
            return fail(ESP_ERR_INVALID_STATE, "TOC name not NUL-terminated");
        }
        if (entry.name[0] == '\0') {
            return fail(ESP_ERR_INVALID_STATE, "TOC name is empty");
        }
        if (entry.offset > header.payload_len || entry.size > header.payload_len - entry.offset) {
            return fail(ESP_ERR_INVALID_SIZE, "TOC entry overruns the payload");
        }
    }
    const uint8_t* payload = buf + kWasHeaderSize + toc_len;
    if (ImageCrc(buf, len) != header.image_crc32) {
        return fail(ESP_ERR_INVALID_STATE, "image CRC32 mismatch");
    }
    if (memchr(header.build_id, '\0', sizeof(header.build_id)) == nullptr) {
        return fail(ESP_ERR_INVALID_STATE, "build_id not NUL-terminated");
    }
    if (out != nullptr) {
        out->version = header.version;
        out->file_count = header.file_count;
        out->payload_len = header.payload_len;
        out->image_len = header.image_len;
        // Point into the SOURCE buffer, not the stack-local header copy.
        out->build_id = reinterpret_cast<const char*>(buf) + kWasBuildIdOffset;
        out->toc = toc;
        out->payload = payload;
    }
    return ESP_OK;
}

}  // namespace web_assets

// ---- Public C API (extern "C" via assets.h; file scope) ----

const WebAsset* FindAsset(const char* name) {
    if (name == nullptr) {
        return nullptr;
    }
    if (name[0] == '/') {  // tolerate the URL path form
        ++name;
    }
    std::lock_guard<std::mutex> lock(web_assets::g_store_mutex);
    if (!web_assets::g_store.valid) {
        return nullptr;
    }
    for (size_t i = 0; i < web_assets::g_store.names.size(); ++i) {
        if (web_assets::g_store.names[i] == name) {
            return &web_assets::g_store.entries[i];
        }
    }
    return nullptr;
}

esp_err_t WebAssetsInit() {
    std::lock_guard<std::mutex> lock(web_assets::g_store_mutex);
    if (web_assets::g_store.valid) {
        return ESP_OK;  // idempotent
    }
    web_assets::MountLocked();
    return web_assets::g_store.valid ? ESP_OK : ESP_ERR_NOT_FOUND;
}

void WebAssetsGetInfo(WebAssetInfo* out) {
    if (out == nullptr) {
        return;
    }
    std::lock_guard<std::mutex> lock(web_assets::g_store_mutex);
    web_assets::FillInfo(web_assets::g_store, out);
}

const WebAsset* GetAssetFallbackPage() {
    return &web_assets::g_fallback;
}

bool AssetImageValidateBuf(const uint8_t* buf, size_t len, size_t partition_size,
                           WebAssetInfo* out) {
    const char* reason = nullptr;
    web_assets::WasImage image = {};
    const esp_err_t err = web_assets::WasValidate(buf, len, partition_size, &image, &reason);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Asset image rejected: %s (%s)", reason, esp_err_to_name(err));
        return false;
    }
    if (out != nullptr) {
        memset(out, 0, sizeof(*out));
        out->valid = true;
        out->version = image.version;
        out->files = image.file_count;
        out->payload_len = image.payload_len;
        snprintf(out->build_id, sizeof(out->build_id), "%.16s", image.build_id);
        out->partition_size = partition_size;
    }
    return true;
}

esp_err_t AssetImageFlashBuf(const uint8_t* buf, size_t len) {
    if (buf == nullptr || len == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    // Validate the whole image in RAM before touching flash.
    const char* reason = nullptr;
    web_assets::WasImage image = {};
    const esp_err_t validate_err = web_assets::WasValidate(buf, len, 0, &image, &reason);
    if (validate_err != ESP_OK) {
        ESP_LOGW(TAG, "Asset image rejected: %s (%s)", reason, esp_err_to_name(validate_err));
        return validate_err;
    }

    std::lock_guard<std::mutex> lock(web_assets::g_store_mutex);
    if (web_assets::g_store.partition == nullptr) {
        web_assets::g_store.partition =
            esp_partition_find_first(ESP_PARTITION_TYPE_ANY, ESP_PARTITION_SUBTYPE_ANY, "assets");
    }
    if (web_assets::g_store.partition == nullptr) {
        return ESP_ERR_NOT_FOUND;
    }
    const esp_partition_t* part = web_assets::g_store.partition;

    // Drop the mmap (ResetStore) before writing through the same region.
    web_assets::ResetStore(web_assets::g_store);
    web_assets::g_store.partition = part;

    const size_t erase = (len + part->erase_size - 1) / part->erase_size * part->erase_size;
    esp_err_t err = esp_partition_erase_range(part, 0, erase);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Erase failed: %s", esp_err_to_name(err));
        return err;
    }
    // Payload + TOC first, header LAST — a partial write never yields a
    // valid image.
    err = esp_partition_write(part, web_assets::kWasHeaderSize, buf + web_assets::kWasHeaderSize,
                              len - web_assets::kWasHeaderSize);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Body write failed: %s", esp_err_to_name(err));
        return err;
    }
    err = esp_partition_write(part, 0, buf, web_assets::kWasHeaderSize);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Header write failed: %s", esp_err_to_name(err));
        return err;
    }
    ESP_LOGI(TAG, "Asset image flashed: %u B, build_id=%.12s", static_cast<unsigned>(len),
             image.build_id);

    // Re-mount from flash (the store now serves the new image, no reboot).
    web_assets::MountLocked();
    return web_assets::g_store.valid ? ESP_OK : ESP_FAIL;
}
