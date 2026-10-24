// STA-mode hub web server: REST /api/, /ws, static assets, mDNS.
// Public methods thread-safe; cloud-touching handlers marshal onto the
// main event loop (25 s cap, 504 on timeout).

#pragma once

#include <esp_http_server.h>

#include <cstdint>
#include <functional>
#include <string>

#include "sys/device_state.h"

// Event payload structs (fwd decls; definitions live in their components).
namespace tuya_cloud {
struct AuthStatus;
}
namespace hub_core {
struct DiscoveryProgress;
struct RegistryEventPayload;
}  // namespace hub_core
namespace tuya_mqtt {
struct MqttStatus;
}

namespace web_server {

class WebServer {
public:
    /// Marshals a job onto the application's main event loop (cloud calls
    /// must never run on the httpd task); registered by main before Start().
    using Scheduler = std::function<void(std::function<void()>)>;

    static WebServer& GetInstance() {
        static WebServer instance;
        return instance;
    }
    WebServer(const WebServer&) = delete;
    WebServer& operator=(const WebServer&) = delete;

    /// Starts the server (idempotent); registers REST, /ws and the static
    /// wildcard (last), then mDNS "tuyahub-<mac4>.local" (_http._tcp).
    bool Start(uint16_t port = 80);

    /// Stops the httpd instance and forgets the WS clients; mDNS stays
    /// announced (one-shot init).
    void Stop();

    bool IsRunning() const { return server_ != nullptr; }

    /// Registers the marshalling hook (see Scheduler); call before Start().
    void SetScheduler(Scheduler scheduler);

    /// Registers the never-drop variant (see Scheduler); call before Start().
    void SetCriticalScheduler(Scheduler scheduler);

    /// Registers the room-refresh hook (POST /api/rooms/refresh); main-loop
    /// call, returns false when one is already in flight. Call before Start().
    void SetRoomRefreshTrigger(std::function<bool()> trigger);

    /// Sets the "state" field of /api/status + every "status" WS event.
    void SetDeviceState(DeviceState state);

    // ---- typed broadcast entry points (JSON shapes live in this component) ----

    /// Full "status" snapshot event (same shape as GET /api/status).
    void BroadcastStatus();

    /// "auth_status" event (same shape as GET /api/auth/status).
    void BroadcastAuthStatus(const tuya_cloud::AuthStatus& status);

    /// "discovery_progress" event (progress counters + active/done/failed).
    void BroadcastDiscoveryProgress(const hub_core::DiscoveryProgress& progress);

    /// "mqtt_status" event ({connected, error?}).
    void BroadcastMqttStatus(const tuya_mqtt::MqttStatus& status);

    /// "device_update"/"device_online"/"device_added"/"device_removed"/
    /// "rooms_updated" (kHomesUpdated/kDevicesCleared stay off the WS).
    void BroadcastRegistryEvent(const hub_core::RegistryEventPayload& event);

private:
    WebServer() = default;
    ~WebServer() = default;

    /// URI registration — REST, /ws, then the static wildcard LAST (an
    /// earlier wildcard would shadow the specific URIs).
    void RegisterUris();

    /// mDNS: hostname "tuyahub-<last-4-hex-of-MAC>", service _http._tcp;
    /// one-shot.
    void InitMdns();

    httpd_handle_t server_ = nullptr;
    uint16_t port_ = 80;
    bool mdns_initialized_ = false;
};

}  // namespace web_server
