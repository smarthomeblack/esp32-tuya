// INTERNAL (src-only) — cross-TU declarations for the REST API split
// files; the public contract stays in rest_api.h.

#pragma once

#include <cJSON.h>
#include <esp_http_server.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include <freertos/semphr.h>

#include "rest_api.h"

namespace web_server {
namespace rest_api {

// ---- Bounds & policy constants ----

/// POST /api/devices/{id}/commands body cap (≤8KB).
constexpr size_t kMaxCommandsBody = 8192;
/// Cap for every other request body (auth/start, scenes trigger, language).
constexpr size_t kMaxSmallBody = 256;
/// POST /api/openapi/credentials body cap (7 credential strings).
constexpr size_t kMaxOpenApiBody = 512;
/// httpd block time on a marshalled job (main loop or worker task). The
/// worker task finishes the job even after a 504; a transient main-loop
/// job rides the drop-oldest queue and may never run.
constexpr uint32_t kMainLoopTimeoutMs = 25000;
/// GET /api/devices response budget (heap items + chunked send).
constexpr size_t kMaxDevicesResponse = 512 * 1024;
/// Device ids / user codes are short ASCII; longer input is a client bug.
constexpr size_t kMaxIdLength = 64;
/// GET /api/logs?lines=N cap (mirrors the LogBuffer record cap).
constexpr size_t kMaxLogLines = 500;
/// GET /api/logs default when `lines` is absent or unparsable.
constexpr size_t kDefaultLogLines = 200;
/// Let the HTTP response reach the client before esp_restart().
constexpr int kRebootDelayMs = 800;

// ---- Shared state (defined in rest_api.cc) ----

extern std::atomic<int> g_device_state;
extern std::mutex g_scheduler_mutex;
extern Scheduler g_scheduler;
extern std::mutex g_room_trigger_mutex;
extern RoomRefreshTrigger g_room_refresh_trigger;

// ---- Response helpers (rest_api_marshal.cc) ----

esp_err_t RespondJson(httpd_req_t* req, const char* body, const char* status);

/// The error schema with the given HTTP status line.
esp_err_t RespondJsonError(httpd_req_t* req, const char* status, const char* code,
                           const char* message);

/// Prints (compact) and sends one owned cJSON tree.
esp_err_t SendJson(httpd_req_t* req, cJSON* root, const char* status = "200 OK");

// ---- Request helpers (rest_api_marshal.cc) ----

/// Bounded body read: timeout -> 408, ret <= 0 -> error (never busy-spin).
/// `allow_empty` permits a zero-length body.
bool ReadBody(httpd_req_t* req, std::string& out, size_t max_len, bool allow_empty);

std::string UrlPath(const httpd_req_t* req);

/// Request bodies reach cJSON_Parse only after this passes.
bool BodyNestingOk(const std::string& body);

/// Content-Length bytes into a PSRAM buffer (multi-MB bodies); caller
/// frees. nullptr only after the error response; no chunked uploads.
uint8_t* ReadBinaryBody(httpd_req_t* req, size_t max_len);

/// Extracts <id> from "/api/<collection>/<id>/<suffix>" (the registered
/// route is a prefix wildcard, so the id and suffix are parsed here).
bool ParseIdPath(const httpd_req_t* req, const char* prefix, const char* suffix, std::string& id);

/// device_id -> room_id from every registry home's rooms ("" = unassigned):
/// staged discovery candidates take their room from here before the commit.
std::vector<std::pair<std::string, std::string>>
RoomAssignmentsFor(const std::vector<hub_core::Home>& homes);

std::string HomeIdFilter(httpd_req_t* req);

/// Delayed reboot from a detached task (never inside an httpd handler).
void ScheduleReboot();

/// lines=N: default 200, clamped 1..kMaxLogLines (unparsable -> default).
size_t LogsLinesParam(httpd_req_t* req);

// ---- Main-loop marshalling (rest_api_marshal.cc) ----

enum class MainLoopResult {
    kCompleted,  ///< the job ran on the main event loop
    kTimeout,    ///< timed out — 504 (the job may never run: drop-oldest queue)
    kNotWired,   ///< main never registered the scheduler (or critical scheduler)
    kNoMem,      ///< semaphore/job allocation failed
};

/// Schedules `body` onto the main event loop; blocks the httpd task on a
/// semaphore for at most `timeout_ms`.
MainLoopResult RunOnMainLoop(std::function<void()> body, uint32_t timeout_ms = kMainLoopTimeoutMs);

/// Never-drop variant of RunOnMainLoop, for future must-complete actions:
/// the job rides the 16-slot critical queue (always runs, even after the
/// 504) — the producer must keep it idempotent/coalescible (latch); the
/// httpd wait stays bounded. Existing callers stay on the drop-oldest path.
MainLoopResult RunOnMainLoopCritical(std::function<void()> body,
                                     uint32_t timeout_ms = kMainLoopTimeoutMs);

/// Registers the never-drop hook (WebServer::SetCriticalScheduler).
void SetCriticalScheduler(Scheduler scheduler);

/// 504/500 responses for the marshalling result (shared by the handlers).
esp_err_t RespondMainLoopError(httpd_req_t* req, MainLoopResult result, const char* what);

// ---- Worker-task marshalling (rest_api_marshal.cc) ----

/// Job for RunOnWorkerTask: `body` runs on the spawned task; `done` is
/// given after it returns — callers may wait on it or drop the handle.
struct WorkerTaskJob {
    std::function<void()> body;
    SemaphoreHandle_t done = nullptr;

    ~WorkerTaskJob() {
        if (done != nullptr) {
            vSemaphoreDelete(done);
        }
    }
};

/// Spawns the "web_worker" task running `body` (blocking cloud calls, httpd
/// session closes — never httpd or main-loop context). nullptr on spawn fail.
std::shared_ptr<WorkerTaskJob> RunOnWorkerTask(std::function<void()> body);

/// Result of RunOnWorkerAndWait (mirrors MainLoopResult).
enum class WorkerResult {
    kCompleted,    ///< the job ran to completion on the worker task
    kTimeout,      ///< timed out — 504 (the job still completes later)
    kSpawnFailed,  ///< the worker task could not be created
};

/// Runs `body` on the worker task; the httpd task blocks up to `timeout_ms`
/// (the job completes even after the 504 goes out).
WorkerResult RunOnWorkerAndWait(std::function<void()> body,
                                uint32_t timeout_ms = kMainLoopTimeoutMs);

/// 504/500 responses for RunOnWorkerAndWait (shared by the handlers).
esp_err_t RespondWorkerError(httpd_req_t* req, WorkerResult result, const char* what);

// ---- Value/name helpers (rest_api_json.cc) ----

/// Language default from the Kconfig choice (main/Kconfig.projbuild).
const char* DefaultLanguage();

/// Attaches an owned child under `key`; frees the child when the attach
/// fails (cJSON does not consume the item then).
void AttachOwned(cJSON* parent, const char* key, cJSON* child);

/// Appends an owned child; frees it when the append fails.
void AddOwnedToArray(cJSON* array, cJSON* child);

/// cJSON -> DpValue: bool->kBool, integral->kInt, other numbers->kDouble,
/// string->kString. Null is rejected.
bool DpValueFromJson(const cJSON* node, hub_core::DpValue& out);

// ---- Object builders (rest_api_json.cc) ----

cJSON* BuildWifiObject();
cJSON* BuildHeapObject();
/// {"usage", "core0", "core1"} — null until the monitor's first
/// valid sample (+5 s), so BuildStatusObject omits the key entirely.
cJSON* BuildCpuObject();
cJSON* BuildAuthSummaryObject();
cJSON* BuildDiscoverySummaryObject();

/// room_id -> room_name within one home; null resolver -> "".
using RoomNameResolver = std::function<std::string(const hub_core::DeviceString& home_id,
                                                   const hub_core::DeviceString& room_id)>;

/// One device for GET /api/devices: {id, name, category, online,
/// support_local, home_id, room_id, room_name, status, status_range}.
cJSON* BuildDeviceObject(const hub_core::Device& device, const RoomNameResolver& room_names);

/// Resolver bound to ONE registry home snapshot (call once per response).
RoomNameResolver MakeRoomNameResolver();

/// Entity row {code, name, type, writable, value?, enabled}: union of
/// status_range + function + status keys, sorted; enabled = not disabled.
cJSON* BuildEntityRows(const hub_core::Device& device);

/// The union of a device's entity code keys (the commit "all entities" fill).
hub_core::DeviceCodeSet CollectEntityCodes(const hub_core::Device& device);

/// Prints each row of BuildEntityRows into `out` (the chunked item form).
void CollectEntityRowStrings(const hub_core::Device& device, std::vector<std::string>& out);

/// Streams `<prefix><item>,...]}` chunked (no contiguous body). `prefix`
/// must end with the response array's opening bracket.
esp_err_t StreamItemArray(httpd_req_t* req, const char* prefix,
                          const std::vector<std::string>& items);

/// One candidate for GET /api/discovery/candidates and the detail
/// endpoint: {id, name, category, product_name, online, home_id, room_id,
/// room_name, already_added, detail_loaded, entities:[...]}.
cJSON* BuildCandidateObject(const hub_core::Device& candidate, bool detail_loaded);

// ---- Handlers defined in the split files (referenced by the route table
// in rest_api.cc; auth is checked inside each one) ----

// rest_api_devices.cc
esp_err_t HandleDevicesApi(httpd_req_t* req);
esp_err_t HandleDeviceWildcardApi(httpd_req_t* req);

// rest_api_rooms.cc
esp_err_t HandleRoomsApi(httpd_req_t* req);
esp_err_t HandleRoomsRefreshApi(httpd_req_t* req);

// rest_api_discovery.cc
esp_err_t HandleDiscoveryScanApi(httpd_req_t* req);
esp_err_t HandleDiscoveryCandidatesApi(httpd_req_t* req);
esp_err_t HandleDiscoveryCandidateDetailApi(httpd_req_t* req);
esp_err_t HandleDiscoveryCommitApi(httpd_req_t* req);

// rest_api_ota.cc
esp_err_t HandleAssetInfoApi(httpd_req_t* req);
esp_err_t HandleAssetUploadApi(httpd_req_t* req);
esp_err_t HandleOtaInfoApi(httpd_req_t* req);
esp_err_t HandleOtaUploadApi(httpd_req_t* req);
esp_err_t HandleOtaRollbackApi(httpd_req_t* req);

}  // namespace rest_api
}  // namespace web_server
