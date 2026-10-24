#include "hub_core/device_registry.h"

#include <esp_log.h>

#include <algorithm>
#include <cmath>
#include <utility>

static const char* TAG = "DeviceRegistry";

// Logging covers dropped reports only (non-finite values); accepted
// mutations stay observable via the event listener.

namespace hub_core {

namespace {

/// Same id/name/sort — a steady-state room refresh fires no event.
bool SameRooms(const std::pmr::vector<Room>& a, const std::vector<Room>& b) {
    return a.size() == b.size() &&
           std::equal(a.begin(), a.end(), b.begin(), [](const Room& l, const Room& r) {
               return std::string_view(l.id) == std::string_view(r.id) &&
                      std::string_view(l.name) == std::string_view(r.name) && l.sort == r.sort;
           });
}

}  // namespace

void DeviceRegistry::ClearAll() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        devices_.clear();
        homes_.clear();
    }
    Notify({RegistryEvent::kDevicesCleared, "", "", {}});
    Notify({RegistryEvent::kHomesUpdated, "", "", {}});
}

void DeviceRegistry::SetHomes(const std::vector<Home>& homes) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        homes_.assign(homes.begin(), homes.end());
    }
    Notify({RegistryEvent::kHomesUpdated, "", "", {}});
}

std::vector<Home> DeviceRegistry::GetHomes() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<Home> out;
    out.reserve(homes_.size());
    out.insert(out.end(), homes_.begin(), homes_.end());
    return out;
}

void DeviceRegistry::SetRooms(std::string_view home_id, const std::vector<Room>& rooms) {
    bool changed = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (Home& home : homes_) {
            if (std::string_view(home.id) != home_id) {
                continue;
            }
            if (!SameRooms(home.rooms, rooms)) {
                home.rooms.assign(rooms.begin(), rooms.end());
                changed = true;
            }
            break;
        }
    }
    if (changed) {
        Notify({RegistryEvent::kRoomsUpdated, "", std::string(home_id), {}});
    }
}

void DeviceRegistry::AssignDeviceRooms(
    std::string_view home_id, const std::vector<std::pair<std::string, std::string>>& pairs) {
    std::vector<std::string> changed_ids;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto& [id, device] : devices_) {
            if (std::string_view(device.home_id) != home_id) {
                continue;
            }
            // The sweep is complete: a device missing from `pairs` has no room.
            std::string_view room;
            for (const auto& pair : pairs) {
                if (pair.second == std::string_view(device.id)) {
                    room = pair.first;
                    break;
                }
            }
            if (std::string_view(device.room_id) == room) {
                continue;
            }
            device.room_id = room;
            changed_ids.emplace_back(id);
        }
    }
    for (const std::string& id : changed_ids) {
        Notify({RegistryEvent::kDeviceUpdated, id, "", {}});
    }
}

void DeviceRegistry::UpsertDevice(const Device& device) {
    RegistryEvent event;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        // A re-discovery REPLACES the device wholesale — no field merging.
        const bool inserted = devices_.insert_or_assign(device.id, device).second;
        event = inserted ? RegistryEvent::kDeviceAdded : RegistryEvent::kDeviceUpdated;
    }
    Notify({event, std::string(device.id), "", {}});
}

bool DeviceRegistry::RemoveDevice(std::string_view id) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (devices_.erase(DeviceString(id)) == 0) {
            return false;
        }
    }
    Notify({RegistryEvent::kDeviceRemoved, std::string(id), "", {}});
    return true;
}

bool DeviceRegistry::UpdateStatus(std::string_view dev_id, std::string_view code,
                                  const DpValue& value, int64_t t_ms) {
    bool changed = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = devices_.find(dev_id);
        if (it == devices_.end()) {
            return false;
        }
        // Disabled entities are invisible here: drop the report silently.
        if (it->second.disabled_entities.count(code) != 0) {
            return true;
        }
        // A non-finite double cannot be rendered or re-serialized.
        if (value.type == DpValue::Type::kDouble && !std::isfinite(value.d)) {
            ESP_LOGW(TAG, "%.*s: non-finite value for '%.*s' dropped",
                     static_cast<int>(dev_id.size()), dev_id.data(), static_cast<int>(code.size()),
                     code.data());
            return true;
        }
        // Identical re-reports (same type+value) fire no event; the
        // timestamp below still records them.
        const auto st = it->second.status.find(code);
        changed = st == it->second.status.end() || !(st->second == value);
        if (changed) {
            it->second.status.insert_or_assign(DeviceString(code), value);
        }
        if (t_ms != 0) {
            it->second.dp_timestamps[DeviceString(code)] = t_ms;
        }
    }
    if (changed) {
        Notify({RegistryEvent::kDeviceUpdated, std::string(dev_id), "", {std::string(code)}});
    }
    return true;
}

bool DeviceRegistry::SetOnline(std::string_view dev_id, bool online) {
    bool changed = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = devices_.find(dev_id);
        if (it == devices_.end()) {
            return false;
        }
        if (it->second.online != online) {
            it->second.online = online;
            changed = true;
        }
    }
    if (changed) {
        Notify({RegistryEvent::kDeviceOnlineChanged, std::string(dev_id), "", {}});
    }
    return true;
}

bool DeviceRegistry::RenameDevice(std::string_view dev_id, std::string_view name) {
    bool changed = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = devices_.find(dev_id);
        if (it == devices_.end()) {
            return false;
        }
        // Same-name rename (no-op save) fires no event; still true.
        if (it->second.name != name) {
            it->second.name = name;
            changed = true;
        }
    }
    if (changed) {
        Notify({RegistryEvent::kDeviceUpdated, std::string(dev_id), "", {}});
    }
    return true;
}

bool DeviceRegistry::SetEntityEnabled(std::string_view dev_id, std::string_view code,
                                      bool enabled) {
    bool changed = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = devices_.find(dev_id);
        if (it == devices_.end()) {
            return false;
        }
        // was_disabled == enabled means the disabled-set membership flips;
        // an unchanged state (no-op save) fires no event and still returns true.
        const bool was_disabled = it->second.disabled_entities.count(code) != 0;
        if (was_disabled == enabled) {
            changed = true;
            if (enabled) {
                it->second.disabled_entities.erase(DeviceString(code));
            } else {
                it->second.disabled_entities.insert(DeviceString(code));
            }
        }
    }
    if (changed) {
        Notify({RegistryEvent::kDeviceUpdated, std::string(dev_id), "", {}});
    }
    return true;
}

size_t DeviceRegistry::MarkAllSetUp() {
    std::lock_guard<std::mutex> lock(mutex_);
    size_t flagged = 0;
    for (auto& [id, device] : devices_) {
        if (!device.set_up) {
            device.set_up = true;
        }
        ++flagged;
    }
    return flagged;
}

bool DeviceRegistry::GetDevice(std::string_view id, Device& out) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = devices_.find(id);
    if (it == devices_.end()) {
        return false;
    }
    out = it->second;
    return true;
}

void DeviceRegistry::ForEachDevice(const std::function<void(const Device&)>& callback) const {
    if (callback == nullptr) {
        return;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto& [id, device] : devices_) {
        callback(device);
    }
}

size_t DeviceRegistry::Count() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return devices_.size();
}

void DeviceRegistry::SetEventListener(EventListener listener) {
    std::lock_guard<std::mutex> lock(mutex_);
    listener_ = std::move(listener);
}

void DeviceRegistry::Notify(RegistryEventPayload ev) {
    EventListener listener;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        listener = listener_;
    }
    if (listener) {
        listener(ev);  // outside the lock — see the listener contract
    }
}

}  // namespace hub_core
