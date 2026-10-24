// WAS1 image format (mirrors scripts/web_asset_pack.py; static_asserts
// pin the geometry). Little-endian; offsets from the image start.

#pragma once

#include <cstdint>

namespace web_assets {

constexpr char kWasMagic[4] = {'W', 'A', 'S', '1'};
constexpr uint16_t kWasVersion = 1;
/// TOC flag: the blob is gzip-compressed.
constexpr uint32_t kWasFlagGzip = 0x1;

// The header is 40 B and the TOC entry 76 B (field-exact sizes;
// repo-internal format).
constexpr size_t kWasHeaderSize = 40;
constexpr size_t kWasTocEntrySize = 76;
constexpr size_t kWasNameSize = 64;
/// build_id offset (4+2+2+4+4+4+4, mirrors python "<4sHHIIII16s"); points
/// WasImage::build_id into the SOURCE buffer, never a stack-local copy.
constexpr size_t kWasBuildIdOffset = 24;
/// image_crc32 field offset — the CRC covers the whole image minus
/// this 4-byte field itself (header + TOC + payload + pad).
constexpr size_t kWasCrcOffset = 16;

struct __attribute__((packed)) WasHeader {
    char magic[4];
    uint16_t version;
    uint16_t reserved;
    uint32_t file_count;
    uint32_t payload_len;
    uint32_t image_crc32;  ///< CRC-32 of the image minus this field
    uint32_t image_len;
    char build_id[16];
};

struct __attribute__((packed)) WasTocEntry {
    char name[64];
    uint32_t offset;  ///< from the payload region start
    uint32_t size;
    uint32_t flags;
};

static_assert(sizeof(WasHeader) == kWasHeaderSize, "WAS1 header must be 40 B");
static_assert(sizeof(WasTocEntry) == kWasTocEntrySize, "WAS1 TOC entry must be 76 B");

}  // namespace web_assets
