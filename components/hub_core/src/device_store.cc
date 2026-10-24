#include "hub_core/device_store.h"

#include <cJSON.h>
#include <esp_littlefs.h>
#include <esp_log.h>
#include <esp_partition.h>

#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <ctime>
#include <string>
#include <unistd.h>
#include <vector>

#include "device_store_internal.h"
#include "tuya_crypto/at_rest_crypto.h"

static const char* TAG = "DeviceStore";

namespace hub_core {

namespace {

// ---- filesystem constants ----

constexpr const char* kMountPoint = "/littlefs";
constexpr const char* kPartitionLabel = "storage";
constexpr const char* kRegistryPath = "/littlefs/registry.json";
constexpr const char* kTmpPath = "/littlefs/registry.json.tmp";

/// Snapshot size cap: well under the 6.9 MB storage partition; on load the blob,
/// its plaintext copy and the JSON tree all live in PSRAM (large allocs go there).
constexpr long kMaxFileBytes = 2 * 1024 * 1024;
constexpr size_t kWriteChunk = 4096;

/// Moves the registry to the single .bad slot (0 / errno). LittleFS rename
/// replaces in place, so an older .bad is dropped first: two 2 MB slots would
/// leave the next save no room for its .tmp.
int MoveAsideBadSlot() {
    std::remove(kBadRegistryPath);
    if (std::rename(kRegistryPath, kBadRegistryPath) == 0) {
        return 0;
    }
    return errno;
}

}  // namespace

namespace {

/// Full conf in declaration order (-Wmissing-field-initializers is fatal).
esp_err_t RegisterStorage(bool format_if_mount_failed) {
    const esp_vfs_littlefs_conf_t conf = {
        .base_path = kMountPoint,
        .partition_label = kPartitionLabel,
        .partition = nullptr,
#ifdef CONFIG_LITTLEFS_SDMMC_SUPPORT
        .sdcard = nullptr,
#endif
#if ESP_LITTLEFS_HAS_BLOCKDEV
        .blockdev = nullptr,
#endif
        .format_if_mount_failed = format_if_mount_failed,
        .read_only = false,
        .dont_mount = false,
        .grow_on_mount = false,
    };
    return esp_vfs_littlefs_register(&conf);
}

/// A stale superblock can advertise more blocks than the shrunken
/// partition has; reformat once at the true size, keeping registry.json.
bool MigrateShrunkStorage() {
    std::string registry;
    const bool had_registry = [&registry]() {
        FILE* f = fopen(kRegistryPath, "rb");
        if (f == nullptr) {
            return false;
        }
        char buf[kWriteChunk];
        size_t n = 0;
        while ((n = fread(buf, 1, sizeof(buf), f)) > 0) {
            registry.append(buf, n);
            if (registry.size() > static_cast<size_t>(kMaxFileBytes)) {
                break;  // corruption, not data — refuse to carry it over
            }
        }
        fclose(f);
        return registry.size() <= static_cast<size_t>(kMaxFileBytes);
    }();
    if (!had_registry) {
        registry.clear();
    }
    esp_vfs_littlefs_unregister(kPartitionLabel);
    // Format at the true block count (a mount override would wipe instead).
    if (esp_littlefs_format(kPartitionLabel) != ESP_OK) {
        ESP_LOGE(TAG, "storage migration: esp_littlefs_format failed");
        return false;
    }
    if (RegisterStorage(false) != ESP_OK) {
        ESP_LOGE(TAG, "storage migration: re-register failed");
        return false;
    }
    const char* registry_outcome = "absent";
    if (had_registry && !registry.empty()) {
        // Raw old-registry bytes, not a sealed snapshot: WriteSnapshotFile
        // seals, so its atomic tmp->flush->fsync->rename write is replicated.
        FILE* out = fopen(kTmpPath, "wb");
        if (out == nullptr) {
            ESP_LOGW(TAG, "storage migration: registry restore open failed");
            registry_outcome = "not restored";
        } else {
            bool ok = fwrite(registry.data(), 1, registry.size(), out) == registry.size();
            if (ok) {
                ok = fflush(out) == 0;
            }
            if (ok) {
                ok = fsync(fileno(out)) == 0;
            }
            if (fclose(out) != 0) {
                ok = false;  // fclose flushes the stream too
            }
            if (!ok) {
                std::remove(kTmpPath);
                ESP_LOGE(TAG, "storage migration: registry restore write failed");
                registry_outcome = "not restored";
            } else if (std::rename(kTmpPath, kRegistryPath) != 0) {
                std::remove(kTmpPath);
                ESP_LOGE(TAG, "storage migration: registry restore rename failed");
                registry_outcome = "not restored";
            } else {
                registry_outcome = "restored";
            }
        }
    }
    ESP_LOGW(TAG, "storage migrated to the smaller partition (registry %s)", registry_outcome);
    return true;
}

}  // namespace

bool DeviceStore::Mount() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (mounted_) {
        return true;  // idempotent
    }
    // VFS truth wins over our flag if the partition is already up.
    if (esp_littlefs_mounted(kPartitionLabel)) {
        mounted_ = true;
        return true;
    }
    // Formatting allowed once per boot (no format loop).
    const esp_err_t err = RegisterStorage(!format_attempted_);
    if (err != ESP_OK) {
        format_attempted_ = true;
        ESP_LOGE(TAG, "esp_vfs_littlefs_register failed: %s — later retries will NOT reformat",
                 esp_err_to_name(err));
        return false;
    }
    // Stale superblock? Migrate once; afterwards a no-op.
    const esp_partition_t* part = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY, kPartitionLabel);
    if (part != nullptr) {
        size_t total = 0;
        size_t used = 0;
        if (esp_littlefs_info(kPartitionLabel, &total, &used) == ESP_OK && total > part->size) {
            if (!MigrateShrunkStorage()) {
                format_attempted_ = true;
                mounted_ = false;
                return false;
            }
        }
    }
    mounted_ = true;
    ESP_LOGI(TAG, "LittleFS mounted at %s (partition \"%s\")", kMountPoint, kPartitionLabel);
    return true;
}

bool DeviceStore::IsMounted() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return mounted_;
}

void DeviceStore::SuppressSaves() {
    saves_suppressed_.store(true);
    ESP_LOGW(TAG, "Store saves suppressed (factory reset path)");
}

bool DeviceStore::SavesSuppressed() const {
    return saves_suppressed_.load();
}

bool DeviceStore::Save(const std::vector<Home>& homes, const std::string& uid,
                       const DeviceSource& device_source) {
    // Serialize first: the source walk may hold the registry mutex (no I/O);
    // the blocking flash write below must run WITHOUT it.
    size_t device_count = 0;
    const std::string body = BuildStoreJsonStreamed(
        homes, uid, static_cast<int64_t>(std::time(nullptr)), device_source, &device_count);
    if (body.empty()) {
        ESP_LOGE(TAG, "Save: serialization failed (out of memory?) — NOTHING WRITTEN");
        return false;
    }

    // Sealed before any flash write: only ciphertext reaches the file.
    std::string sealed;
    if (!tuya_crypto::AtRestSeal(body, sealed)) {
        ESP_LOGE(TAG, "Save: snapshot encryption failed — NOTHING WRITTEN");
        return false;
    }
    // Load refuses anything over the cap: writing it would strand the snapshot.
    if (sealed.size() > static_cast<size_t>(kMaxFileBytes)) {
        ESP_LOGE(TAG, "Save: refusing to write an unloadable snapshot (%u B > %ld B cap)",
                 static_cast<unsigned>(sealed.size()), kMaxFileBytes);
        return false;
    }
    if (!WriteSnapshotFile(sealed)) {
        return false;
    }
    ESP_LOGI(TAG, "Saved registry snapshot: %zu homes, %zu devices, %zu bytes", homes.size(),
             device_count, body.size());
    return true;
}

bool DeviceStore::WriteSnapshotFile(const std::string& bytes) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!mounted_) {
        ESP_LOGE(TAG, "Snapshot write before Mount() — nothing written");
        return false;
    }
    // Factory-reset window: a suppressed save must not resurrect the snapshot.
    if (saves_suppressed_.load()) {
        return false;
    }

    // Atomic write: tmp -> flush -> fsync -> rename; a failed step removes the tmp.
    FILE* f = std::fopen(kTmpPath, "wb");
    if (f == nullptr) {
        ESP_LOGE(TAG, "Snapshot write: fopen(%s) failed (errno %d)", kTmpPath, errno);
        return false;
    }
    bool ok = true;
    const char* data = bytes.data();
    size_t remaining = bytes.size();
    while (remaining > 0) {
        const size_t chunk = remaining > kWriteChunk ? kWriteChunk : remaining;
        if (std::fwrite(data, 1, chunk, f) != chunk) {
            ok = false;
            break;
        }
        data += chunk;
        remaining -= chunk;
    }
    if (ok) {
        ok = std::fflush(f) == 0;
    }
    if (ok) {
        ok = fsync(fileno(f)) == 0;
    }
    if (std::fclose(f) != 0) {
        ok = false;  // fclose flushes the stream too
    }
    if (!ok) {
        std::remove(kTmpPath);
        ESP_LOGE(TAG, "Snapshot write: write to %s failed — snapshot NOT stored", kTmpPath);
        return false;
    }
    constexpr int kRenameAttempts = 2;
    int rename_errno = 0;
    for (int attempt = 0; attempt < kRenameAttempts; ++attempt) {
        if (std::rename(kTmpPath, kRegistryPath) == 0) {
            return true;
        }
        rename_errno = errno;
        if (rename_errno != ENOSPC) {
            break;
        }
        // A full disk is ENOSPC-only: drop the .bad slot once; the live snapshot
        // stays (it is the last known-good copy, the new one is still in .tmp).
        if (std::remove(kBadRegistryPath) == 0) {
            ESP_LOGW(TAG, "Snapshot write: .bad slot freed to fit the new snapshot");
            continue;
        }
        break;
    }
    // Capture errno before the cleanup remove() may clobber it.
    std::remove(kTmpPath);
    ESP_LOGE(TAG, "Snapshot write: rename(%s -> %s) failed (errno %d)", kTmpPath, kRegistryPath,
             rename_errno);
    return false;
}

bool DeviceStore::ReadSnapshotBlob(std::string& blob) {
    blob.clear();
    std::lock_guard<std::mutex> lock(mutex_);
    if (!mounted_) {
        ESP_LOGE(TAG, "Snapshot read before Mount()");
        return false;
    }
    FILE* f = std::fopen(kRegistryPath, "rb");
    if (f == nullptr) {
        if (errno == ENOENT) {
            ESP_LOGI(TAG, "No registry snapshot yet — first boot or cleared");
        } else {
            ESP_LOGW(TAG, "Snapshot read: fopen(%s) failed (errno %d)", kRegistryPath, errno);
        }
        return false;
    }
    // seek/tell under the open handle; capped defensively.
    if (std::fseek(f, 0, SEEK_END) != 0) {
        std::fclose(f);
        ESP_LOGW(TAG, "Snapshot read: fseek end failed");
        return false;
    }
    const long size = std::ftell(f);
    if (size < 0 || size > kMaxFileBytes) {
        std::fclose(f);
        ESP_LOGE(TAG, "Snapshot read: bad file size %ld (cap %ld) — refusing to parse", size,
                 kMaxFileBytes);
        // Move aside so the next save can heal instead of failing every boot
        // (overwrites any older .bad: the partition must never hold two).
        const bool moved = MoveAsideBadSlot() == 0;
        ESP_LOGW(TAG, "Snapshot unreadable (%s) — moved aside; registry starts empty (file %s)",
                 size < 0 ? "size read failed" : "over cap",
                 moved ? "saved as .bad" : "left in place");
        return false;
    }
    if (std::fseek(f, 0, SEEK_SET) != 0) {
        std::fclose(f);
        ESP_LOGW(TAG, "Snapshot read: fseek set failed");
        return false;
    }
    blob.resize(static_cast<size_t>(size));
    if (size > 0 &&
        std::fread(blob.data(), 1, static_cast<size_t>(size), f) != static_cast<size_t>(size)) {
        std::fclose(f);
        ESP_LOGW(TAG, "Snapshot read: short read");
        return false;
    }
    std::fclose(f);
    return true;
}

bool DeviceStore::Load(std::vector<Home>& homes, std::vector<Device>& devices,
                       std::string& stored_uid) {
    std::string blob;
    if (!ReadSnapshotBlob(blob)) {
        return false;
    }

    // Decrypt + parse outside the lock (the bytes are already ours). A decrypt
    // failure behaves like a corrupt snapshot: fall back to discovery.
    std::string text;
    const bool encrypted = tuya_crypto::AtRestIsEncrypted(blob);
    if (encrypted) {
        if (!tuya_crypto::AtRestOpen(blob, text)) {
            return false;  // already logged
        }
    } else {
        text = std::move(blob);
    }
    if (!ParseStoreJson(text, homes, devices, stored_uid)) {
        return false;
    }
    // Legacy plaintext snapshot: rewrite once in the sealed format.
    if (!encrypted) {
        std::string sealed;
        // Same cap as Save: a sealed blob Load would refuse must not be written.
        if (tuya_crypto::AtRestSeal(text, sealed) &&
            sealed.size() <= static_cast<size_t>(kMaxFileBytes) && WriteSnapshotFile(sealed)) {
            ESP_LOGI(TAG, "Legacy snapshot re-sealed in the encrypted format");
        } else {
            ESP_LOGW(TAG, "Legacy snapshot re-seal failed — plaintext copy kept");
        }
    }
    ESP_LOGI(TAG, "Loaded registry snapshot: %zu homes, %zu devices", homes.size(), devices.size());
    return true;
}

bool DeviceStore::PeekOwnerUid(std::string& out_uid) {
    out_uid.clear();
    std::string blob;
    if (!ReadSnapshotBlob(blob)) {
        return false;
    }
    // Decrypt + parse only: no Home/Device materialization.
    std::string text;
    if (tuya_crypto::AtRestIsEncrypted(blob)) {
        if (!tuya_crypto::AtRestOpen(blob, text)) {
            return false;  // already logged
        }
    } else {
        text = std::move(blob);
    }
    cJSON* root = cJSON_ParseWithLength(text.c_str(), text.size());
    if (root == nullptr) {
        ESP_LOGW(TAG, "PeekOwnerUid: snapshot is not valid JSON");
        return false;
    }
    bool ok = false;
    if (!cJSON_IsObject(root)) {
        ESP_LOGW(TAG, "PeekOwnerUid: snapshot root is not an object");
    } else {
        const cJSON* uid = cJSON_GetObjectItemCaseSensitive(root, "uid");
        ok = cJSON_IsString(uid) && uid->valuestring != nullptr && uid->valuestring[0] != '\0';
        if (ok) {
            out_uid = uid->valuestring;
        }
    }
    cJSON_Delete(root);
    return ok;
}

bool DeviceStore::Clear() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!mounted_) {
        ESP_LOGW(TAG, "Clear before Mount()");
        return false;
    }
    // Also remove a stale .tmp (interrupted Save) and .bad (unsupported
    // version moved aside); best effort.
    std::remove(kTmpPath);
    std::remove(kBadRegistryPath);
    if (std::remove(kRegistryPath) != 0) {
        if (errno == ENOENT) {
            return true;  // nothing left to clear
        }
        ESP_LOGE(TAG, "Clear: remove(%s) failed (errno %d)", kRegistryPath, errno);
        return false;
    }
    ESP_LOGI(TAG, "Registry snapshot removed (%s)", kRegistryPath);
    return true;
}

}  // namespace hub_core
