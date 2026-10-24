#pragma once

/// INTERNAL (src-only) — do not include from public headers.
/// Pure OpenAPI command routing helpers, shared with the self-test.

#include <set>
#include <string>
#include <string_view>
#include <vector>

#include "hub_core/device.h"
#include "hub_services/command_service.h"

namespace hub_core {

/// Substitutes "{id}" (the client resolves only "{uid}"); no URL escaping.
std::string OpenApiDevicePath(std::string_view path_template, const std::string& dev_id);

/// shadow/properties/issue body: {"properties": "<double-encoded JSON
/// string>"}. False, `body` untouched, on empty list/code/null-ish value.
bool BuildShadowIssueBody(const std::vector<DeviceCommand>& commands, std::string& body);

/// /v1.0/devices/{id}/commands body; rejection rules as BuildShadowIssueBody.
bool BuildOpenApiCommandsBody(const std::vector<DeviceCommand>& commands, std::string& body);

/// Splits commands by local_strategy: use_open_api -> openapi_cmds, all
/// else -> sharing_cmds. Sharing goes first; both groups must succeed.
void RouteCommands(const Device& device, const std::vector<DeviceCommand>& commands,
                   std::vector<DeviceCommand>& openapi_cmds,
                   std::vector<DeviceCommand>& sharing_cmds);

/// Splits OpenAPI commands by property_update: true -> issue_cmds
/// (issue endpoint first), false -> regular_cmds.
void SplitByPropertyUpdate(const Device& device, const std::vector<DeviceCommand>& commands,
                           std::vector<DeviceCommand>& issue_cmds,
                           std::vector<DeviceCommand>& regular_cmds);

// ---- OpenAPI quota constants ----

/// NVS namespace/key of the quota state ("openapi_quota" fits the 15-char limit).
constexpr const char* kQuotaNvsNamespace = "hub";
constexpr const char* kQuotaNvsKey = "openapi_quota";
/// Tuya free-tier cap — advisory; the server enforces the real one.
constexpr int kQuotaLimit = 10;
/// Devices used at which the WARN fires and the REST `warning` flag.
constexpr int kQuotaWarnThreshold = 8;
/// Months before this are bogus (pre-SNTP default); quota never acts below.
constexpr int kQuotaMinValidMonth = 200101;

/// Wall-clock month as YYYYMM (UTC); 0 when the clock is unsynced.
int CurrentMonth();

// OpenAPI quota: max 10 distinct controllable devices per month (free
// tier); the live side persists the set via NVS.

/// Encodes "<YYYYMM>:dev1,dev2,..." (std::set order); empty set -> "<month>:".
std::string QuotaEncode(int month, const std::set<std::string>& devices);

/// Parses the QuotaEncode form; devices is cleared first, filled on success.
bool QuotaDecode(const std::string& text, int& month, std::set<std::string>& devices);

/// month <= 0 -> no-op false; a new month clears the set; inserts dev_id.
/// True when the state changed.
bool QuotaRecord(int month, const std::string& dev_id, int& tracked_month,
                 std::set<std::string>& devices);

}  // namespace hub_core
