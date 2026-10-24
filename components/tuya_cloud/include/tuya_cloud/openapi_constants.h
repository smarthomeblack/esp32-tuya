#pragma once

#include <string_view>

namespace tuya_cloud {

// Tuya OpenAPI constants: plaintext HTTP; only HMAC-SHA256 request signing.

// Data centers: initial guess for the account's region; the login response's platform_url overrides
// it.
constexpr std::string_view kOpenApiDcUs = "https://openapi.tuyaus.com";

/// Fallback when OpenApiCredentials::datacenter is empty.
constexpr std::string_view kOpenApiDcDefault = kOpenApiDcUs;

/// SMART_HOME login: POST {username, password: md5-hex, country_code, schema}.
constexpr std::string_view kOpenApiLoginSmartHome =
    "/v1.0/iot-01/associated-users/actions/authorized-login";

/// CUSTOM login: POST {username, password: sha256-hex}.
constexpr std::string_view kOpenApiLoginCustom = "/v1.0/iot-03/users/login";

// Device endpoints; {id}/{uid} placeholders are replaced by the caller.

/// Device model (Tuya-spec functions/dps).
constexpr std::string_view kOpenApiThingModel = "/v2.0/cloud/thing/{id}/model";

/// Device shadow properties (current dp values).
constexpr std::string_view kOpenApiThingShadow = "/v2.0/cloud/thing/{id}/shadow/properties";

/// Issue a shadow property update (command round-trip).
constexpr std::string_view kOpenApiThingIssue = "/v2.0/cloud/thing/{id}/shadow/properties/issue";

/// Classic command endpoint (instructions format).
constexpr std::string_view kOpenApiDeviceCommands = "/v1.0/devices/{id}/commands";

// Home endpoints; {home_id}/{room_id}/{uid} are resolved by the client.

/// Rooms of one home: result = {home_id, name, rooms:[{room_id, name, sort}]}.
constexpr std::string_view kOpenApiHomeRooms = "/v1.0/homes/{home_id}/rooms";

/// Devices of one room: result = full device objects (joined by "id").
constexpr std::string_view kOpenApiRoomDevices = "/v1.0/homes/{home_id}/rooms/{room_id}/devices";

}  // namespace tuya_cloud
