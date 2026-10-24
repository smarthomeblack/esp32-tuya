#pragma once

#include <cstddef>
#include <functional>
#include <memory_resource>
#include <mutex>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "hub_core/device.h"

namespace hub_core {

/// Registry mutation / notification events.
enum class RegistryEvent {
    kDevicesCleared,       ///< ClearAll(): every device dropped
    kHomesUpdated,         ///< SetHomes() (or ClearAll() emptying them)
    kRoomsUpdated,         ///< SetRooms(): a home's room list changed
    kDeviceAdded,          ///< UpsertDevice() created a new id
    kDeviceRemoved,        ///< RemoveDevice() deleted a device
    kDeviceUpdated,        ///< status values changed (changed_codes set) or name/room changed
    kDeviceOnlineChanged,  ///< SetOnline() flipped the online flag
};

/// dev_id empty for global events (kRoomsUpdated carries home_id instead);
/// changed_codes set only by UpdateStatus.
struct RegistryEventPayload {
    RegistryEvent event = RegistryEvent::kDevicesCleared;
    std::string dev_id;
    std::string home_id;
    std::vector<std::string> changed_codes;
};

/// Resident device database, pmr-backed (nodes/strings in the PSRAM pool;
/// discovery replaces wholesale, MQTT updates surgically). Listener: no
/// lock held, must not re-enter the registry.
class DeviceRegistry {
public:
    static DeviceRegistry& GetInstance() {
        static DeviceRegistry instance;
        return instance;
    }
    DeviceRegistry(const DeviceRegistry&) = delete;
    DeviceRegistry& operator=(const DeviceRegistry&) = delete;

    using EventListener = std::function<void(const RegistryEventPayload&)>;

    /// Wipes devices AND homes. Fires kDevicesCleared and kHomesUpdated.
    void ClearAll();

    /// Replaces the home list. Fires kHomesUpdated.
    void SetHomes(const std::vector<Home>& homes);

    std::vector<Home> GetHomes() const;

    /// Replaces one home's room list; fires kRoomsUpdated only when it changed.
    void SetRooms(std::string_view home_id, const std::vector<Room>& rooms);

    /// `pairs` = (room_id, dev_id); "" room_id unassigns. Devices of `home_id`
    /// missing from `pairs` are cleared — so pass only a COMPLETE room sweep.
    /// Fires kDeviceUpdated (no codes) once per device whose room changed.
    void AssignDeviceRooms(std::string_view home_id,
                           const std::vector<std::pair<std::string, std::string>>& pairs);

    /// Inserts or fully replaces the device; fires kDeviceAdded or kDeviceUpdated.
    void UpsertDevice(const Device& device);

    /// Deletes a device; false when the id is unknown. Fires kDeviceRemoved.
    bool RemoveDevice(std::string_view id);

    /// Applies one status report (MQTT protocol 4); t_ms==0 records no
    /// timestamp. Disabled codes are silently dropped. An identical
    /// re-report (same type+value) fires no event.
    bool UpdateStatus(std::string_view dev_id, std::string_view code, const DpValue& value,
                      int64_t t_ms);

    /// Flips the online flag; fires kDeviceOnlineChanged only on a change.
    bool SetOnline(std::string_view dev_id, bool online);

    /// Renames a device (protocol 20 nameUpdate). False when the id is unknown.
    bool RenameDevice(std::string_view dev_id, std::string_view name);

    /// Enables/disables one entity; unknown codes accepted, unknown id false.
    bool SetEntityEnabled(std::string_view dev_id, std::string_view code, bool enabled);

    /// Marks every device set_up=true; fires no events.
    size_t MarkAllSetUp();

    /// Copies the device with this id into `out`; false when unknown.
    bool GetDevice(std::string_view id, Device& out) const;

    /// Visits every device with the mutex HELD — calling registry APIs
    /// from the callback deadlocks.
    void ForEachDevice(const std::function<void(const Device&)>& callback) const;

    size_t Count() const;

    /// Sets (replaces) the event listener — see the contract above.
    void SetEventListener(EventListener listener);

private:
    DeviceRegistry() = default;

    /// Invokes the listener with the lock NOT held; null is a no-op.
    void Notify(RegistryEventPayload ev);

    mutable std::mutex mutex_;
    std::pmr::vector<Home> homes_;
    DeviceMap<Device> devices_;
    EventListener listener_;
};

}  // namespace hub_core
