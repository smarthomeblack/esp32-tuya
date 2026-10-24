#pragma once

#include <string_view>

namespace tuya_cloud {

// Protocol constants (Tuya Device Sharing protocol).

constexpr std::string_view kClientId = "HA_3y9q4ak7g4ephrvke";

constexpr std::string_view kSchema = "haauthorize";

/// Login gateway base URL, scheme included.
constexpr std::string_view kLoginHost = "https://apigw.iotbing.com";

/// HTTP timeout in ms, PER socket op — esp_http_client resets it on progress
/// (a trickling server can outlast it); single attempt, no retries.
constexpr int32_t kHttpTimeoutMs = 60000;

/// Total budget for one user-facing command/scene chain (ms, esp_timer);
/// background scans keep their own retry policy.
constexpr int32_t kCloudCommandDeadlineMs = 30000;

// API path catalog. Paths keep literal "{devId}"/"{token}" placeholders for the
// caller to substitute. Gotcha: specifications/commands are /v1.1/, others /v1.0/.

/// Prefix — the refresh token is appended.
constexpr std::string_view kPathTokenRefresh = "/v1.0/m/token/";

constexpr std::string_view kPathHomes = "/v1.0/m/life/users/homes";

constexpr std::string_view kPathDevicesByHome = "/v1.0/m/life/ha/home/devices";

constexpr std::string_view kPathDeviceDetail = "/v1.0/m/life/ha/devices/detail";

constexpr std::string_view kPathSpecifications = "/v1.1/m/life/{devId}/specifications";

constexpr std::string_view kPathStatusStrategy = "/v1.0/m/life/devices/{devId}/status";

constexpr std::string_view kPathDpReportTypes = "/v1.0/m/life/ha/{devId}/dp-report-types";

constexpr std::string_view kPathCommands = "/v1.1/m/thing/{devId}/commands";

constexpr std::string_view kPathMqttConfig = "/v1.0/m/life/ha/access/config";

constexpr std::string_view kPathScenesByHome = "/v1.0/m/scene/ha/home/scenes";

constexpr std::string_view kPathSceneTrigger = "/v1.0/m/scene/ha/trigger";

constexpr std::string_view kPathTerminalRevoke = "/v1.0/m/token/terminal/expire";

/// QR token request on the login gateway; PLAINTEXT params, no X-sign, no encryption.
constexpr std::string_view kPathQrToken = "/v1.0/m/life/home-assistant/qrcode/tokens";

/// Login result poll on the login gateway; PLAINTEXT.
constexpr std::string_view kPathQrTokenPoll = "/v1.0/m/life/home-assistant/qrcode/tokens/{token}";

}  // namespace tuya_cloud
