/// INTERNAL (src-only) — REST API handlers (everything under /api/) plus
/// the JSON builders shared with the WebSocket events.

#pragma once

#include <cJSON.h>
#include <esp_http_server.h>

#include <functional>
#include <string>

#include "hub_core/device_registry.h"
#include "hub_services/discovery.h"
#include "sys/device_state.h"
#include "tuya_cloud/cloud_auth.h"
#include "tuya_mqtt/tuya_mqtt_client.h"

namespace web_server {
namespace rest_api {

/// Main-loop marshalling hook type (WebServer::Scheduler spelled out).
using Scheduler = std::function<void(std::function<void()>)>;

/// Main-loop hook that starts the room refresh; false when one is already in
/// flight (POST /api/rooms/refresh answers 200 then). Empty = not wired.
using RoomRefreshTrigger = std::function<bool()>;

/// Registers the /api/ URI handlers (all Bearer-gated; /api/devices/*
/// dispatches by suffix). Register before /ws + the wildcard (33 of 33).
void RegisterHandlers(httpd_handle_t server);

/// main-loop hooks + device-state storage (thread-safe).
void SetScheduler(Scheduler scheduler);
void SetDeviceState(DeviceState state);
void SetRoomRefreshTrigger(RoomRefreshTrigger trigger);

// ---- owned cJSON object builders (caller owns / transfers the result) ----

/// GET /api/status snapshot — also the "status" WS event data.
cJSON* BuildStatusObject();

/// GET /api/auth/status payload — also the "auth_status" WS event data.
cJSON* BuildAuthStatusObject(const tuya_cloud::AuthStatus& status);

/// "discovery_progress" WS event data (progress counters + terminal flags).
cJSON* BuildDiscoveryProgressObject(const hub_core::DiscoveryProgress& progress);

/// "mqtt_status" WS event data ({connected, error}); also /api/status's "mqtt".
cJSON* BuildMqttStatusObject(const tuya_mqtt::MqttStatus& status);

/// Registry-event WS data: {devId} (add/remove), {devId, online},
/// {devId, name, props:[{code, value, t}]}.
cJSON* BuildRegistryEventData(const hub_core::RegistryEventPayload& event);

/// WS event name for a registry event; nullptr for kHomesUpdated/kDevicesCleared.
const char* RegistryEventName(hub_core::RegistryEvent event);

}  // namespace rest_api
}  // namespace web_server
