#pragma once

#include <atomic>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

#include "hub_core/device.h"

namespace hub_core {

/// Feeds one device into a streamed save.
using DeviceEmit = std::function<void(const Device&)>;
/// Invokes `emit` once per device (e.g. DeviceRegistry::ForEachDevice); may
/// hold the registry lock — bounded walk, no I/O, no registry re-entry.
using DeviceSource = std::function<void(const DeviceEmit&)>;

/// Persistent registry snapshot at /littlefs/registry.json ("storage"
/// partition, atomic tmp+rename), stored as an AES-256-GCM "ENC1" container
/// (plaintext JSON is never written). The flash write blocks (1-2 s) and
/// never runs under the registry mutex — the streamed device walk does.
class DeviceStore {
public:
    static DeviceStore& GetInstance() {
        static DeviceStore instance;
        return instance;
    }
    DeviceStore(const DeviceStore&) = delete;
    DeviceStore& operator=(const DeviceStore&) = delete;

    /// Mounts (formats on first boot) the "storage" littlefs partition; idempotent.
    bool Mount();

    bool IsMounted() const;

    /// One-way latch: saves are refused from the factory-reset wipe
    /// until reboot (WriteSnapshotFile gate).
    void SuppressSaves();

    bool SavesSuppressed() const;

    /// Streams + atomically writes the snapshot; `device_source` feeds
    /// devices one by one (no vector copy); `uid` feeds the account check.
    bool Save(const std::vector<Home>& homes, const std::string& uid,
              const DeviceSource& device_source);

    /// Loads the snapshot (fully replaced); false when missing or unparsable.
    bool Load(std::vector<Home>& homes, std::vector<Device>& devices, std::string& stored_uid);

    /// Reads ONLY the snapshot owner uid (no Home/Device materialization):
    /// decrypt + parse, then the top-level "uid" string. False when the file
    /// is missing/unreadable or carries no uid (out_uid cleared).
    bool PeekOwnerUid(std::string& out_uid);

    /// Removes the registry file (and a stale .tmp / .bad); ENOENT is success.
    bool Clear();

private:
    DeviceStore() = default;

    /// Atomic tmp->flush->fsync->rename write of the (sealed) snapshot bytes.
    bool WriteSnapshotFile(const std::string& bytes);

    /// Reads the registry file bytes under the mutex (size-capped).
    bool ReadSnapshotBlob(std::string& blob);

    mutable std::mutex mutex_;
    bool mounted_ = false;
    bool format_attempted_ = false;  // one format-on-failure per boot
    /// Atomic: the httpd factory-reset handler touches it without the mutex.
    std::atomic<bool> saves_suppressed_{false};
};

}  // namespace hub_core
