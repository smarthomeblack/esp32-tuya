// Asset self-test — WAS1 validation vectors over pure RAM images.
// The pack/parse agreement with scripts/web_asset_pack.py is exercised
// end-to-end by the mock server + the flash boot.

#include "web_assets/asset_self_test.h"

#include <esp_log.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

#include "asset_format.h"
#include "asset_parse.h"
#include "web_assets/assets.h"

static const char* TAG = "AssetTest";

namespace web_assets {

namespace {

struct TestResult {
    int total = 0;
    int passed = 0;
};

void CheckTrue(TestResult& r, const char* name, bool condition) {
    ++r.total;
    if (condition) {
        ++r.passed;
        return;
    }
    ESP_LOGE(TAG, "FAIL %s", name);
}

void CheckEqU32(TestResult& r, const char* name, uint32_t expected, uint32_t got) {
    ++r.total;
    if (expected == got) {
        ++r.passed;
        return;
    }
    ESP_LOGE(TAG, "FAIL %s: expected=%u got=%u", name, expected, got);
}

/// Minimal image builder (mirrors scripts/web_asset_pack.py pack()).
std::vector<uint8_t> BuildImage(const std::vector<std::pair<std::string, std::string>>& files,
                                uint16_t version = kWasVersion) {
    std::vector<std::pair<std::string, std::string>> sorted = files;
    // std::pair sorts by first then second — names are unique here.
    std::sort(sorted.begin(), sorted.end());
    size_t payload_len = 0;
    for (const auto& file : sorted) {
        payload_len += file.second.size();
    }
    const size_t unpadded = kWasHeaderSize + kWasTocEntrySize * sorted.size() + payload_len;
    const size_t pad = (4 - unpadded % 4) % 4;
    const uint32_t image_len = static_cast<uint32_t>(unpadded + pad);

    std::vector<uint8_t> payload;
    payload.reserve(payload_len);
    for (const auto& file : sorted) {
        payload.insert(payload.end(), file.second.begin(), file.second.end());
    }
    std::string build_id = "w";
    // sha256 is not linked here; any 11-char id exercises the field.
    build_id += "abc123def00";

    std::vector<uint8_t> image(kWasHeaderSize, 0);
    WasHeader header = {};
    memcpy(header.magic, kWasMagic, sizeof(header.magic));
    header.version = version;
    header.reserved = 0;
    header.file_count = static_cast<uint32_t>(sorted.size());
    header.payload_len = static_cast<uint32_t>(payload_len);
    header.image_crc32 = 0;  // patched after the image is assembled
    header.image_len = image_len;
    memcpy(header.build_id, build_id.c_str(), build_id.size());
    memcpy(image.data(), &header, sizeof(header));

    uint32_t offset = 0;
    for (const auto& file : sorted) {
        WasTocEntry entry = {};
        snprintf(entry.name, sizeof(entry.name), "%s", file.first.c_str());
        entry.offset = offset;
        entry.size = static_cast<uint32_t>(file.second.size());
        entry.flags = kWasFlagGzip;
        image.insert(image.end(), reinterpret_cast<const uint8_t*>(&entry),
                     reinterpret_cast<const uint8_t*>(&entry) + sizeof(entry));
        offset += entry.size;
    }
    image.insert(image.end(), payload.begin(), payload.end());
    image.resize(image.size() + pad, 0);
    // CRC covers everything except its own 4 bytes (mirrors the packer).
    const uint32_t crc = ImageCrc(image.data(), image.size());
    memcpy(image.data() + kWasCrcOffset, &crc, sizeof(crc));
    return image;
}

void TestValidImage(TestResult& r) {
    const std::vector<uint8_t> image =
        BuildImage({{"index.html", std::string(211, 'a')}, {"js/app.js", std::string(97, 'b')}});
    WasImage parsed = {};
    const esp_err_t err = WasValidate(image.data(), image.size(), 0x100000, &parsed, nullptr);
    CheckTrue(r, "valid_ok", err == ESP_OK);
    CheckEqU32(r, "valid_file_count", 2, parsed.file_count);
    CheckEqU32(r, "valid_payload_len", 211 + 97, parsed.payload_len);
    CheckEqU32(r, "valid_image_len", image.size(), parsed.image_len);
    CheckTrue(r, "valid_toc_names",
              parsed.toc != nullptr && strcmp(parsed.toc[0].name, "index.html") == 0 &&
                  strcmp(parsed.toc[1].name, "js/app.js") == 0);
    CheckTrue(r, "valid_toc_offsets",
              parsed.toc[1].offset == 211 && parsed.toc[1].size == 97 &&
                  parsed.payload + 308 == image.data() + image.size());
    CheckTrue(r, "valid_build_id", strcmp(parsed.build_id, "wabc123def00") == 0);
}

void TestCorruptions(TestResult& r) {
    const std::vector<uint8_t> good = BuildImage(
        {{"index.html", std::string(300, 'x')}, {"css/base.css", std::string(500, 'y')}});

    // Bad magic.
    std::vector<uint8_t> bad = good;
    bad[0] = 'X';
    CheckTrue(r, "bad_magic_rejected",
              WasValidate(bad.data(), bad.size(), 0, nullptr, nullptr) != ESP_OK);

    // Future version.
    bad = good;
    uint16_t version = kWasVersion + 1;
    memcpy(bad.data() + 4, &version, sizeof(version));
    CheckTrue(r, "future_version_rejected",
              WasValidate(bad.data(), bad.size(), 0, nullptr, nullptr) != ESP_OK);

    // Shorter than the header.
    CheckTrue(r, "short_buffer_rejected",
              WasValidate(good.data(), kWasHeaderSize - 1, 0, nullptr, nullptr) != ESP_OK);

    // image_len disagrees with the buffer length.
    bad = good;
    const uint32_t bogus_len = static_cast<uint32_t>(good.size()) + 4;
    memcpy(bad.data() + 20, &bogus_len, sizeof(bogus_len));
    CheckTrue(r, "image_len_mismatch_rejected",
              WasValidate(bad.data(), bad.size(), 0, nullptr, nullptr) != ESP_OK);

    // payload_len says 0xFFFFFFFF but only 1 pad byte follows header + TOC.
    {
        std::vector<uint8_t> wrap = BuildImage({{"a", ""}});
        wrap.push_back(0);
        WasHeader patched = {};
        memcpy(&patched, wrap.data(), sizeof(patched));
        patched.payload_len = 0xFFFFFFFF;
        patched.image_len = static_cast<uint32_t>(wrap.size());
        memcpy(wrap.data(), &patched, sizeof(patched));
        const uint32_t crc = ImageCrc(wrap.data(), wrap.size());
        memcpy(wrap.data() + kWasCrcOffset, &crc, sizeof(crc));
        CheckTrue(r, "payload_len_wrap_rejected",
                  WasValidate(wrap.data(), wrap.size(), 0, nullptr, nullptr) != ESP_OK);
    }

    // TOC entry whose offset+size overrun the payload: rebuild with a
    // tampered size field (offset 40 + 64 in the first TOC entry).
    bad = good;
    const uint32_t bogus_size = 100000;
    memcpy(bad.data() + kWasHeaderSize + kWasNameSize + 4, &bogus_size, sizeof(bogus_size));
    CheckTrue(r, "toc_overflow_rejected",
              WasValidate(bad.data(), bad.size(), 0, nullptr, nullptr) != ESP_OK);

    // A 64-byte name without any NUL.
    {
        std::vector<uint8_t> nonul = BuildImage({{"a", "b"}});
        memset(nonul.data() + kWasHeaderSize, 'A', kWasNameSize);
        CheckTrue(r, "name_not_nul_rejected",
                  WasValidate(nonul.data(), nonul.size(), 0, nullptr, nullptr) != ESP_OK);
    }

    // One flipped payload byte -> CRC mismatch.
    bad = good;
    bad[bad.size() - 1] ^= 0xFF;
    CheckTrue(r, "payload_flip_crc_rejected",
              WasValidate(bad.data(), bad.size(), 0, nullptr, nullptr) != ESP_OK);

    // Exceeds the size cap.
    CheckTrue(r, "size_cap_rejected",
              WasValidate(good.data(), good.size(), 16, nullptr, nullptr) != ESP_OK);

    // The upload-path wrapper agrees on both sides.
    WebAssetInfo info = {};
    CheckTrue(r, "validate_buf_ok",
              AssetImageValidateBuf(good.data(), good.size(), 0x100000, &info));
    CheckTrue(r, "validate_buf_info",
              info.valid && info.files == 2 && std::string(info.build_id) == "wabc123def00");
    CheckTrue(r, "validate_buf_corrupt_rejected",
              !AssetImageValidateBuf(bad.data(), bad.size(), 0x100000, nullptr));
}

}  // namespace

bool RunWebAssetSelfTest() {
    TestResult r;
    // Runs before WebServer::Start -> before WebAssetsInit: FindAsset
    // must answer NULL (the store is not mounted yet).
    CheckTrue(r, "find_asset_pre_init_null", FindAsset("index.html") == nullptr);
    CheckTrue(r, "find_asset_null_name_null", FindAsset(nullptr) == nullptr);
    const WebAsset* fallback = GetAssetFallbackPage();
    CheckTrue(r, "fallback_page_ready",
              fallback != nullptr && fallback->data != nullptr && fallback->size > 0);
    TestValidImage(r);
    TestCorruptions(r);
    if (r.passed == r.total) {
        ESP_LOGI(TAG, "ASSET SELFTEST PASS (%d checks)", r.total);
        return true;
    }
    ESP_LOGE(TAG, "ASSET SELFTEST FAIL (%d/%d)", r.passed, r.total);
    return false;
}

}  // namespace web_assets
