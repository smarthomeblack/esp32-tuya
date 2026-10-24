#include "application.h"

#include <esp_log.h>

#include <mutex>
#include <string>
#include <vector>

#include "hub_core/device_registry.h"
#include "hub_core/device_store.h"
#include "sys/system_info.h"

// Log tag mirrors Application so the boot log stays byte-identical.
static const char* TAG = "Application";

bool Application::RestoreRegistryFromStore(bool was_linked) {
    // Single-flight: the CAS lets exactly one restore; the loser
    // returns true.
    bool expected = false;
    if (!registry_restore_done_.compare_exchange_strong(expected, true)) {
        return true;  // already restored
    }

    hub_core::DeviceStore& store = hub_core::DeviceStore::GetInstance();
    if (!store.Mount()) {
        // Not fatal — the hub works discovery-based this boot.
        ESP_LOGW(TAG, "Store unavailable — falling back to discovery");
        RestoreFailedKeepRetry(was_linked);
        return false;
    }

    std::vector<hub_core::Home> homes;
    std::vector<hub_core::Device> devices;
    std::string stored_uid;
    if (!store.Load(homes, devices, stored_uid)) {
        ClearStoreOwnerUid();
        RestoreFailedKeepRetry(was_linked);
        return false;  // Load already logged the reason
    }

    // Different known uids -> wipe; an empty uid proves nothing.
    const std::string current_uid = CurrentAccountUid();
    if (!current_uid.empty() && !stored_uid.empty() && current_uid != stored_uid) {
        store.Clear();
        ClearStoreOwnerUid();
        ESP_LOGW(TAG, "Registry snapshot belongs to a different account — wiped");
        // Refuse without a retry: the wipe already re-armed the restore latch and
        // StartPushSession falls through to its scan (a re-arm here would loop).
        return false;
    }

    // Stamp the owner BEFORE the Upserts (no re-stamp).
    SetStoreOwnerUid(stored_uid);

    // restoring_registry_ suppresses the event-driven saves.
    restoring_registry_.store(true);
    hub_core::DeviceRegistry& registry = hub_core::DeviceRegistry::GetInstance();
    registry.SetHomes(homes);
    for (const hub_core::Device& device : devices) {
        registry.UpsertDevice(device);
    }
    // Safety net for paths that skipped the added-events.
    registry.MarkAllSetUp();
    // Flag-clear lands after every restore event (critical jobs drain last).
    ScheduleCritical([this] { restoring_registry_.store(false); });

    ESP_LOGI(TAG, "Registry restored from store: %zu home(s), %zu device(s)", homes.size(),
             devices.size());
    // Homes-only snapshots return false -> the caller stages a scan.
    return registry.Count() > 0;
}

/// After a failed restore: with the cloud already linked the push still needs a
/// registry restore latch + a running transport, so re-arm both. Cleared first;
/// the StartMqttPush refusal re-arms the tick. Main loop only.
void Application::RestoreFailedKeepRetry(bool was_cloud_linked) {
    if (!was_cloud_linked) {
        return;
    }
    ESP_LOGW(TAG, "Restore failed after link — re-arming the restore and the push");
    registry_restore_done_.store(false);
    mqtt_push_started_ = false;
    mqtt_push_next_retry_ms_ =
        static_cast<int64_t>(sys::SystemInfo::GetUptimeMs()) + kMqttPushRetryFirstMs;
}

bool Application::StoreOwnerMatchesCurrentAccount() {
    hub_core::DeviceStore& store = hub_core::DeviceStore::GetInstance();
    // Owner-only peek: the snapshot is read/decrypted without materialising devices.
    std::string stored_uid;
    if (!store.Mount() || !store.PeekOwnerUid(stored_uid)) {
        // Unreadable snapshot: fall back to the in-RAM owner stamp.
        stored_uid = GetStoreOwnerUid();
        if (stored_uid.empty()) {
            return true;  // neither source has a uid — nothing to compare
        }
    }
    // Boot-check rule: only known, different uids count as a mismatch.
    const std::string current_uid = CurrentAccountUid();
    return current_uid.empty() || stored_uid.empty() || current_uid == stored_uid;
}

void Application::SetStoreOwnerUid(const std::string& uid) {
    std::lock_guard<std::mutex> lock(store_owner_mutex_);
    store_owner_uid_ = uid;
}

std::string Application::GetStoreOwnerUid() {
    std::lock_guard<std::mutex> lock(store_owner_mutex_);
    return store_owner_uid_;
}

void Application::ClearStoreOwnerUid() {
    std::lock_guard<std::mutex> lock(store_owner_mutex_);
    store_owner_uid_.clear();
}

void Application::SaveRegistryToStore(const char* why) {
    // Main loop only — the atomic write blocks for up to ~1-2 s.
    hub_core::DeviceStore& store = hub_core::DeviceStore::GetInstance();
    if (!store.IsMounted()) {
        // Mount failure is permanent for this boot (no format loop).
        ESP_LOGW(TAG, "Store save skipped (%s) — store not mounted", why);
        return;
    }
    hub_core::DeviceRegistry& registry = hub_core::DeviceRegistry::GetInstance();
    const std::vector<hub_core::Home> homes = registry.GetHomes();
    size_t device_count = 0;
    // Stream devices straight from the registry walk (no device copies); the
    // registry lock is held only for the walk, never for the flash write.
    const hub_core::DeviceSource stream_devices =
        [&registry, &device_count](const hub_core::DeviceEmit& emit) {
            registry.ForEachDevice([&emit, &device_count](const hub_core::Device& device) {
                ++device_count;
                emit(device);
            });
        };

    // Stamp the owner uid, not the live credential (unlink-race safe).
    const bool ok = store.Save(homes, GetStoreOwnerUid(), stream_devices);
    if (ok) {
        ESP_LOGI(TAG, "Registry persisted (%s): %zu home(s), %zu device(s)", why, homes.size(),
                 device_count);
        store_save_retry_logged_.store(false);
        return;
    }
    // Keep the changes armed for the 60 s tick so the save retries.
    store_dirty_.store(true);
    if (!store_save_retry_logged_.exchange(true)) {
        ESP_LOGW(TAG, "Registry persist FAILED (%s) — snapshot unchanged on flash; retrying", why);
    }
}

void Application::ScheduleStoreSave(const char* why) {
    // One coalesced save; the job clears the flag first (no lost changes).
    if (hub_core::DeviceStore::GetInstance().SavesSuppressed()) {
        // Factory-reset window: never resurrect registry.json after the wipe.
        return;
    }
    bool expected = false;
    if (!store_save_pending_.compare_exchange_strong(expected, true)) {
        return;  // already queued — it snapshots the latest state
    }
    if (restoring_registry_.load()) {
        // A restore's own upserts are already on flash; a real change landing
        // inside the window stays dirty for the debounce.
        store_save_pending_.store(false);
        store_dirty_.store(true);
        return;
    }
    ScheduleCritical([this, why] {
        store_save_pending_.store(false);
        SaveRegistryToStore(why);
    });
}
