// REST /api/ route table + core endpoints + the shared-state definitions.

#include "rest_api.h"
#include "rest_api_internal.h"

#include <esp_log.h>
#include <esp_partition.h>
#include <esp_wifi.h>

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "hub_core/device_store.h"
#include "hub_services/command_service.h"
#include "sys/log_buffer.h"
#include "sys/log_capture.h"
#include "sys/settings.h"
#include "tuya_cloud/credentials.h"
#include "tuya_cloud/customer_api.h"
#include "tuya_cloud/openapi_client.h"
#include "web_server/api_auth.h"
#include "ws_handler.h"

static const char* TAG = "RestApi";

namespace web_server {
namespace rest_api {

// ---- Shared state (registered by main before the server starts) ----

std::atomic<int> g_device_state{kDeviceStateStarting};

std::mutex g_scheduler_mutex;
Scheduler g_scheduler;

// Room-refresh hook (main registers it); guarded by g_scheduler_mutex.
std::mutex g_room_trigger_mutex;
RoomRefreshTrigger g_room_refresh_trigger;

/// lines=N: default 200, clamped 1..kMaxLogLines (unparsable -> default).
size_t LogsLinesParam(httpd_req_t* req) {
    char query[64] = {0};
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK) {
        return kDefaultLogLines;
    }
    char value[16] = {0};
    if (httpd_query_key_value(query, "lines", value, sizeof(value)) != ESP_OK) {
        return kDefaultLogLines;
    }
    char* end = nullptr;
    const long parsed = strtol(value, &end, 10);
    if (end == value || *end != '\0') {
        return kDefaultLogLines;  // not a number at all
    }
    if (parsed < 1) {
        return 1;
    }
    return parsed > static_cast<long>(kMaxLogLines) ? kMaxLogLines : static_cast<size_t>(parsed);
}

namespace {

// ---- OpenAPI provisioning ----

// The detached login task lives in tuya_cloud (OpenApiClient::LoginInBackground).

// Known datacenter hosts: must match the web UI's select (tuya-iot.js TIOT_DATACENTERS).
bool IsKnownDatacenter(const std::string& datacenter) {
    return datacenter == "https://openapi.tuyaus.com" ||
           datacenter == "https://openapi.tuyaeu.com" ||
           datacenter == "https://openapi.tuyacn.com" ||
           datacenter == "https://openapi.tuyain.com" ||
           datacenter == "https://openapi-sg.iotbing.com";
}

/// Spawns it (lock-free xTaskCreate; the semaphore wait never trips).
void SpawnOpenApiLoginTask() {
    tuya_cloud::OpenApiClient& client = tuya_cloud::OpenApiClient::GetInstance();
    // A FRESH provisioning POST only wrote NVS — the client is still
    // UNCONFIGURED — so Configure() here, or LoginInBackground would no-op.
    if (!client.snapshot_configured()) {
        tuya_cloud::OpenApiCredentials creds;
        if (tuya_cloud::OpenApiCredentialsStore::Load(creds)) {
            client.Configure(creds);
        } else {
            ESP_LOGE(TAG, "Tuya IoT login start: credentials vanished from NVS — not spawning");
            return;
        }
    }
    client.LoginInBackground();
}

// ---- GET handlers (no network — snapshots only) ----

// GET /api/status — the full snapshot (see BuildStatusObject).
esp_err_t HandleStatusApi(httpd_req_t* req) {
    if (RequireAuth(req) != ESP_OK)
        return ESP_FAIL;  // 401 already sent
    return SendJson(req, BuildStatusObject());
}

// GET /api/logs — direct LogBuffer read (thread-safe; no main-loop
// marshal). One cJSON tree per line, streamed chunked like /api/devices.
esp_err_t HandleLogsApi(httpd_req_t* req) {
    if (RequireAuth(req) != ESP_OK)
        return ESP_FAIL;  // 401 already sent
    const size_t max_lines = LogsLinesParam(req);
    size_t total = 0;
    sys::LogCapture::Buffer().Stats(&total);
    const std::vector<std::string> raw_lines = sys::LogCapture::Buffer().GetLines(max_lines);

    std::vector<std::string> items;
    items.reserve(raw_lines.size());
    sys::LogLineFields fields;
    char prev_level = '\0';
    for (const std::string& raw : raw_lines) {
        // Continuations inherit the previous line's parsed level.
        sys::ParseLogLine(raw, prev_level, &fields);
        prev_level = fields.level;
        cJSON* item = cJSON_CreateObject();
        if (item == nullptr) {
            continue;  // OOM — skip this line
        }
        cJSON_AddNumberToObject(item, "t", static_cast<double>(fields.ms));
        const char lvl[2] = {fields.level, '\0'};
        cJSON_AddStringToObject(item, "lvl", lvl);
        cJSON_AddStringToObject(item, "tag", fields.tag.c_str());
        cJSON_AddStringToObject(item, "msg", fields.msg.c_str());
        char* item_str = cJSON_PrintUnformatted(item);
        cJSON_Delete(item);
        if (item_str == nullptr) {
            continue;
        }
        items.emplace_back(item_str);
        cJSON_free(item_str);
    }

    char prefix[48];
    snprintf(prefix, sizeof(prefix), "{\"total\":%u,\"lines\":[", static_cast<unsigned>(total));
    return StreamItemArray(req, prefix, items);
}

// GET /api/homes — [{id, name}] from the registry.
esp_err_t HandleHomesApi(httpd_req_t* req) {
    if (RequireAuth(req) != ESP_OK)
        return ESP_FAIL;  // 401 already sent
    cJSON* root = cJSON_CreateArray();
    if (root == nullptr) {
        return RespondJsonError(req, "500 Internal Server Error", "internal", "Out of memory");
    }
    for (const hub_core::Home& home : hub_core::DeviceRegistry::GetInstance().GetHomes()) {
        cJSON* item = cJSON_CreateObject();
        if (item == nullptr) {
            continue;  // OOM — skip this entry
        }
        cJSON_AddStringToObject(item, "id", home.id.c_str());
        cJSON_AddStringToObject(item, "name", home.name.c_str());
        AddOwnedToArray(root, item);
    }
    return SendJson(req, root);
}

// GET /api/scenes[?home_id=] — the CommandService scene cache (no fetch).
esp_err_t HandleScenesApi(httpd_req_t* req) {
    if (RequireAuth(req) != ESP_OK)
        return ESP_FAIL;  // 401 already sent
    const std::string home_filter = HomeIdFilter(req);
    cJSON* root = cJSON_CreateArray();
    if (root == nullptr) {
        return RespondJsonError(req, "500 Internal Server Error", "internal", "Out of memory");
    }
    for (const hub_core::Scene& scene : hub_core::CommandService::GetInstance().GetScenes()) {
        if (!home_filter.empty() && scene.home_id != home_filter) {
            continue;
        }
        cJSON* item = cJSON_CreateObject();
        if (item == nullptr) {
            continue;
        }
        cJSON_AddStringToObject(item, "id", scene.scene_id.c_str());
        cJSON_AddStringToObject(item, "name", scene.name.c_str());
        cJSON_AddBoolToObject(item, "enabled", scene.enabled);
        cJSON_AddStringToObject(item, "home_id", scene.home_id.c_str());
        AddOwnedToArray(root, item);
    }
    return SendJson(req, root);
}

// GET /api/auth/status — the login-engine snapshot.
esp_err_t HandleAuthStatusApi(httpd_req_t* req) {
    if (RequireAuth(req) != ESP_OK)
        return ESP_FAIL;  // 401 already sent
    return SendJson(req,
                    BuildAuthStatusObject(tuya_cloud::CloudAuthManager::GetInstance().GetStatus()));
}

// GET /api/settings/language — {"lang":"vi"} (default from Kconfig).
esp_err_t HandleLanguageGetApi(httpd_req_t* req) {
    if (RequireAuth(req) != ESP_OK)
        return ESP_FAIL;  // 401 already sent
    sys::Settings settings("hub");
    const std::string lang = settings.GetString("lang", DefaultLanguage());
    cJSON* root = cJSON_CreateObject();
    if (root == nullptr) {
        return RespondJsonError(req, "500 Internal Server Error", "internal", "Out of memory");
    }
    cJSON_AddStringToObject(root, "lang", lang.c_str());
    return SendJson(req, root);
}

// GET /api/openapi/quota — {"configured", "month", "devices", "limit",
// "warning"}. Snapshot only (RAM reads), runs directly on the httpd task.
esp_err_t HandleOpenApiQuotaApi(httpd_req_t* req) {
    if (RequireAuth(req) != ESP_OK)
        return ESP_FAIL;  // 401 already sent
    const hub_core::OpenApiQuotaInfo quota =
        hub_core::CommandService::GetInstance().GetOpenApiQuota();
    cJSON* root = cJSON_CreateObject();
    if (root == nullptr) {
        return RespondJsonError(req, "500 Internal Server Error", "internal", "Out of memory");
    }
    cJSON_AddBoolToObject(root, "configured",
                          tuya_cloud::OpenApiClient::GetInstance().IsConfigured());
    // YYYYMM -> "YYYY-MM"; "" when nothing was tracked and the clock is
    // unsynced (the UI treats "" as unknown).
    char month_text[16] = {0};
    if (quota.month > 0) {
        snprintf(month_text, sizeof(month_text), "%04d-%02d", quota.month / 100, quota.month % 100);
    }
    cJSON_AddStringToObject(root, "month", month_text);
    cJSON_AddNumberToObject(root, "devices", static_cast<double>(quota.devices));
    cJSON_AddNumberToObject(root, "limit", static_cast<double>(quota.limit));
    cJSON_AddBoolToObject(root, "warning", quota.warning);
    return SendJson(req, root);
}

// GET /api/openapi/status — {configured, has_token, uid?, endpoint?,
// quota{...}} for the settings UI.
esp_err_t HandleOpenApiStatusApi(httpd_req_t* req) {
    if (RequireAuth(req) != ESP_OK)
        return ESP_FAIL;  // 401 already sent
    tuya_cloud::OpenApiClient& client = tuya_cloud::OpenApiClient::GetInstance();

    // Lazy boot wiring: the first query reconfigures the client + reloads
    // the token; the reads below are lock-free.
    if (!client.snapshot_configured() && !client.snapshot_login_in_flight()) {
        tuya_cloud::OpenApiCredentials creds;
        if (tuya_cloud::OpenApiCredentialsStore::Load(creds)) {
            client.Configure(creds);
            client.LoadToken();  // absent/expired -> stays logged out (result ignored)
        }
    }
    // Recompute a token that crossed into expiry while the hub idled.
    client.RefreshTokenSnapshotForExpiry();
    const hub_core::OpenApiQuotaInfo quota =
        hub_core::CommandService::GetInstance().GetOpenApiQuota();

    cJSON* root = cJSON_CreateObject();
    if (root == nullptr) {
        return RespondJsonError(req, "500 Internal Server Error", "internal", "Out of memory");
    }
    cJSON_AddBoolToObject(root, "configured", client.snapshot_configured());
    cJSON_AddBoolToObject(root, "has_token", client.snapshot_has_token());
    cJSON_AddBoolToObject(root, "login_in_flight", client.snapshot_login_in_flight());
    const std::string uid = client.snapshot_uid();
    if (!uid.empty()) {
        cJSON_AddStringToObject(root, "uid", uid.c_str());
    }
    // Effective endpoint (snapshot): the login response's platform_url
    // when known, else the configured datacenter.
    const std::string endpoint = client.snapshot_endpoint();
    if (!endpoint.empty()) {
        cJSON_AddStringToObject(root, "endpoint", endpoint.c_str());
    }
    cJSON* quota_obj = cJSON_AddObjectToObject(root, "quota");
    if (quota_obj != nullptr) {
        // YYYYMM -> "YYYY-MM" (same convention as /api/openapi/quota).
        char month_text[16] = {0};
        if (quota.month > 0) {
            snprintf(month_text, sizeof(month_text), "%04d-%02d", quota.month / 100,
                     quota.month % 100);
        }
        cJSON_AddStringToObject(quota_obj, "month", month_text);
        cJSON_AddNumberToObject(quota_obj, "devices", static_cast<double>(quota.devices));
        cJSON_AddNumberToObject(quota_obj, "limit", static_cast<double>(quota.limit));
        cJSON_AddBoolToObject(quota_obj, "warning", quota.warning);
    }
    // Self-heal: spawn the background re-login (single-flight; non-blocking).
    if (client.snapshot_configured() && !client.snapshot_has_token() &&
        !client.snapshot_login_in_flight()) {
        client.LoginInBackground();
    }
    return SendJson(req, root);
}

// ---- POST handlers (marshalled: main loop or worker task) ----

// POST /api/scenes/{id}/trigger — body optional {"home_id":"..."}; without
// it the scene's own home is looked up from the cached list.
esp_err_t HandleSceneTriggerApi(httpd_req_t* req) {
    if (RequireAuth(req) != ESP_OK)
        return ESP_FAIL;  // 401 already sent
    std::string scene_id;
    if (!ParseIdPath(req, "/api/scenes/", "/trigger", scene_id)) {
        return RespondJsonError(req, "404 Not Found", "not_found", "Unknown scene path");
    }
    std::string home_id;
    std::string body;
    if (!ReadBody(req, body, kMaxSmallBody, true)) {
        return ESP_FAIL;  // response already sent
    }
    if (!body.empty()) {
        cJSON* root = BodyNestingOk(body) ? cJSON_Parse(body.c_str()) : nullptr;
        if (root == nullptr) {
            return RespondJsonError(req, "400 Bad Request", "invalid_param", "Invalid JSON");
        }
        const cJSON* home_item = cJSON_GetObjectItemCaseSensitive(root, "home_id");
        if (cJSON_IsString(home_item) && home_item->valuestring != nullptr) {
            home_id = home_item->valuestring;
        }
        cJSON_Delete(root);
    }
    if (home_id.empty()) {
        for (const hub_core::Scene& scene : hub_core::CommandService::GetInstance().GetScenes()) {
            if (scene.scene_id == scene_id) {
                home_id = scene.home_id;
                break;
            }
        }
    }
    if (home_id.empty()) {
        return RespondJsonError(req, "404 Not Found", "not_found",
                                "Unknown scene id (and no home_id supplied)");
    }

    auto ok = std::make_shared<bool>(false);
    // Cloud I/O runs on the worker task (the main loop stays free); ≤ 25 s.
    const WorkerResult result = RunOnWorkerAndWait([home_id, scene_id, ok]() {
        *ok = hub_core::CommandService::GetInstance().TriggerScene(home_id, scene_id);
    });
    if (result != WorkerResult::kCompleted) {
        return RespondWorkerError(req, result, "Scene trigger timed out");
    }
    if (*ok) {
        return RespondJson(req, "{\"success\":true}", "200 OK");
    }
    return RespondJsonError(req, "502 Bad Gateway", "command_failed", "Scene trigger failed");
}

// POST /api/auth/start — body {"user_code":"..."}; async, the UI polls
// /api/auth/status for the QR payload.
esp_err_t HandleAuthStartApi(httpd_req_t* req) {
    if (RequireAuth(req) != ESP_OK)
        return ESP_FAIL;  // 401 already sent
    std::string body;
    if (!ReadBody(req, body, kMaxSmallBody, false)) {
        return ESP_FAIL;  // response already sent
    }
    cJSON* root = BodyNestingOk(body) ? cJSON_Parse(body.c_str()) : nullptr;
    if (root == nullptr) {
        return RespondJsonError(req, "400 Bad Request", "invalid_param", "Invalid JSON");
    }
    const cJSON* code_item = cJSON_GetObjectItemCaseSensitive(root, "user_code");
    std::string user_code;
    if (cJSON_IsString(code_item) && code_item->valuestring != nullptr) {
        user_code = code_item->valuestring;
    }
    cJSON_Delete(root);
    if (user_code.empty() || user_code.size() > kMaxIdLength) {
        return RespondJsonError(req, "400 Bad Request", "invalid_param",
                                "'user_code' must be a non-empty string");
    }

    // StartLogin must run on the main loop (serialization contract).
    const MainLoopResult result = RunOnMainLoop(
        [user_code]() { tuya_cloud::CloudAuthManager::GetInstance().StartLogin(user_code); });
    if (result != MainLoopResult::kCompleted) {
        return RespondMainLoopError(req, result, "Login start timed out");
    }
    return RespondJson(req, "{\"success\":true}", "200 OK");
}

// POST /api/auth/unlink — best-effort revoke + credential wipe; the cloud
// leg runs on the worker task, the state teardown is mutex-guarded.
esp_err_t HandleAuthUnlinkApi(httpd_req_t* req) {
    if (RequireAuth(req) != ESP_OK)
        return ESP_FAIL;  // 401 already sent
    const WorkerResult result =
        RunOnWorkerAndWait([]() { tuya_cloud::CloudAuthManager::GetInstance().Unlink(); });
    if (result != WorkerResult::kCompleted) {
        return RespondWorkerError(req, result, "Unlink timed out");
    }
    return RespondJson(req, "{\"success\":true}", "200 OK");
}

// POST /api/openapi/credentials — validates + persists the fields, then
// spawns the detached "oa_login" task and answers 200.
esp_err_t HandleOpenApiCredentialsApi(httpd_req_t* req) {
    if (RequireAuth(req) != ESP_OK)
        return ESP_FAIL;  // 401 already sent
    std::string body;
    if (!ReadBody(req, body, kMaxOpenApiBody, false)) {
        return ESP_FAIL;  // response already sent
    }
    cJSON* root = BodyNestingOk(body) ? cJSON_Parse(body.c_str()) : nullptr;
    if (root == nullptr) {
        return RespondJsonError(req, "400 Bad Request", "invalid_param", "Invalid JSON");
    }
    tuya_cloud::OpenApiCredentials creds;
    const struct Field {
        const char* key;
        std::string* out;
    } fields[] = {
        {"access_id", &creds.access_id},       {"access_secret", &creds.access_secret},
        {"username", &creds.username},         {"password", &creds.password},
        {"country_code", &creds.country_code}, {"schema", &creds.schema},
        {"datacenter", &creds.datacenter},
    };
    bool valid = true;
    for (const Field& field : fields) {
        const cJSON* item = cJSON_GetObjectItemCaseSensitive(root, field.key);
        if (item == nullptr) {
            continue;  // absent -> stays "" (optional, server-side defaults)
        }
        if (!cJSON_IsString(item) || item->valuestring == nullptr) {
            valid = false;  // present but not a string
            break;
        }
        *field.out = item->valuestring;
    }
    cJSON_Delete(root);
    if (!valid || creds.access_id.empty() || creds.access_secret.empty() ||
        creds.username.empty() || creds.password.empty()) {
        return RespondJsonError(req, "400 Bad Request", "invalid_param",
                                "'access_id', 'access_secret', 'username' and 'password' must "
                                "be non-empty strings");
    }
    // Empty datacenter stays allowed (server-side default); anything else must be known.
    if (!creds.datacenter.empty() && !IsKnownDatacenter(creds.datacenter)) {
        return RespondJsonError(req, "400 Bad Request", "invalid_param",
                                "'datacenter' must be a known Tuya IoT datacenter host");
    }
    // Persist before the login attempt (a failed login must not lose the form).
    tuya_cloud::OpenApiCredentialsStore::Save(creds);
    // The main-loop job only spawns the login task; a 504 leaves the
    // credentials saved with no token.
    const MainLoopResult result = RunOnMainLoop([]() { SpawnOpenApiLoginTask(); });
    if (result != MainLoopResult::kCompleted) {
        return RespondMainLoopError(req, result, "Tuya IoT login start timed out");
    }
    return RespondJson(req, "{\"success\":true}", "200 OK");
}

// POST /api/openapi/clear — wipes the NVS namespace + the in-memory client;
// racing an in-flight login converges to "cleared".
esp_err_t HandleOpenApiClearApi(httpd_req_t* req) {
    if (RequireAuth(req) != ESP_OK)
        return ESP_FAIL;  // 401 already sent
    ESP_LOGW(TAG, "Tuya IoT clear requested — wiping credentials + token");
    tuya_cloud::OpenApiCredentialsStore::ClearAll();
    // The two client calls take a mutex an in-flight login can hold ~120 s;
    // they run on the worker task, never on the shared httpd task.
    const WorkerResult result = RunOnWorkerAndWait([]() {
        tuya_cloud::OpenApiClient& client = tuya_cloud::OpenApiClient::GetInstance();
        client.ClearToken();
        client.Configure(tuya_cloud::OpenApiCredentials{});
    });
    if (result != WorkerResult::kCompleted) {
        return RespondWorkerError(req, result, "Tuya IoT clear timed out");
    }
    return RespondJson(req, "{\"success\":true}", "200 OK");
}

// POST /api/settings/language — body {"lang":"vi"|"en"}.
esp_err_t HandleLanguageSetApi(httpd_req_t* req) {
    if (RequireAuth(req) != ESP_OK)
        return ESP_FAIL;  // 401 already sent
    std::string body;
    if (!ReadBody(req, body, kMaxSmallBody, false)) {
        return ESP_FAIL;  // response already sent
    }
    cJSON* root = BodyNestingOk(body) ? cJSON_Parse(body.c_str()) : nullptr;
    if (root == nullptr) {
        return RespondJsonError(req, "400 Bad Request", "invalid_param", "Invalid JSON");
    }
    const cJSON* lang_item = cJSON_GetObjectItemCaseSensitive(root, "lang");
    std::string lang;
    if (cJSON_IsString(lang_item) && lang_item->valuestring != nullptr) {
        lang = lang_item->valuestring;
    }
    cJSON_Delete(root);
    if (lang != "vi" && lang != "en") {
        return RespondJsonError(req, "400 Bad Request", "invalid_param",
                                "'lang' must be \"vi\" or \"en\"");
    }
    {
        sys::Settings settings("hub", true);
        settings.SetString("lang", lang);
    }
    return RespondJson(req, "{\"success\":true}", "200 OK");
}

// GET /api/settings/apikey — {"initial":bool}; the key itself never
// leaves the hub (only its SHA-256 hash is stored).
esp_err_t HandleApiKeyGetApi(httpd_req_t* req) {
    if (RequireAuth(req) != ESP_OK)
        return ESP_FAIL;  // 401 already sent
    cJSON* root = cJSON_CreateObject();
    if (root == nullptr) {
        return RespondJsonError(req, "500 Internal Server Error", "internal", "Out of memory");
    }
    cJSON_AddBoolToObject(root, "initial", ApiKeyIsInitial());
    return SendJson(req, root);
}

// POST /api/settings/apikey — body {"new":"..."} (8..64 printable chars);
// the current Bearer key must already be valid.
esp_err_t HandleApiKeySetApi(httpd_req_t* req) {
    if (RequireAuth(req) != ESP_OK)
        return ESP_FAIL;  // 401 already sent
    std::string body;
    if (!ReadBody(req, body, kMaxSmallBody, false)) {
        return ESP_FAIL;  // response already sent
    }
    cJSON* root = BodyNestingOk(body) ? cJSON_Parse(body.c_str()) : nullptr;
    if (root == nullptr) {
        return RespondJsonError(req, "400 Bad Request", "invalid_param", "Invalid JSON");
    }
    const cJSON* new_item = cJSON_GetObjectItemCaseSensitive(root, "new");
    std::string new_key;
    if (cJSON_IsString(new_item) && new_item->valuestring != nullptr) {
        new_key = new_item->valuestring;
    }
    cJSON_Delete(root);
    if (!ApiKeyStore(new_key.c_str(), new_key.size())) {
        return RespondJsonError(req, "400 Bad Request", "invalid_param",
                                "'new' must be 8..64 printable characters");
    }
    // Off the httpd task: the queued session closes need ctrl-msg slots that
    // only the httpd task returns, so evicting here would self-deadlock.
    // One worker batches every close — a second job could exceed the slots.
    if (RunOnWorkerTask([server = req->handle]() { ws::EvictAllSessions(server); }) == nullptr) {
        ESP_LOGE(TAG, "Worker spawn failed — evicting old-key sessions on the httpd task");
        ws::EvictAllSessions(req->handle);
    }
    return RespondJson(req, "{\"success\":true}", "200 OK");
}

// POST /api/system/reboot — respond first, reboot 800 ms later.
esp_err_t HandleRebootApi(httpd_req_t* req) {
    if (RequireAuth(req) != ESP_OK)
        return ESP_FAIL;  // 401 already sent
    ESP_LOGW(TAG, "Reboot requested via /api/system/reboot");
    esp_err_t err = RespondJson(req, "{\"success\":true}", "200 OK");
    ScheduleReboot();
    return err;
}

// POST /api/system/factory-reset — erases wifi/tuya/hub/openapi NVS + the
// registry snapshot + the WiFi driver's nvs.net80211, then responds and reboots.
esp_err_t HandleFactoryResetApi(httpd_req_t* req) {
    if (RequireAuth(req) != ESP_OK)
        return ESP_FAIL;  // 401 already sent
    ESP_LOGW(TAG, "Factory reset requested — erasing all NVS + registry store");
    // Stop in-flight logins BEFORE the wipe: a QR poll or Tuya IoT login
    // finishing in the reboot window must not re-persist credentials.
    tuya_cloud::CloudAuthManager::GetInstance().SuppressTokenPersistence();
    tuya_cloud::CloudAuthManager::GetInstance().Cancel();
    tuya_cloud::CustomerApi::GetInstance().Reset();
    tuya_cloud::OpenApiClient::GetInstance().AbortLogin();
    // Wipe runs on this task before the response (never queued, never
    // dropped); WriteSnapshotFile refuses saves until the reboot.
    hub_core::DeviceStore& store = hub_core::DeviceStore::GetInstance();
    store.SuppressSaves();
    const bool store_cleared = store.Mount() && store.Clear();
    if (!store_cleared) {
        // Saves are suppressed and this handler reboots, so a raw erase cannot be re-saved.
        ESP_LOGE(TAG, "Registry store clear failed — erasing the storage partition");
        const esp_partition_t* storage = esp_partition_find_first(
            ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_LITTLEFS, "storage");
        if (storage == nullptr) {
            ESP_LOGE(TAG, "storage partition not found — the old registry may survive");
        } else {
            const esp_err_t erase_err = esp_partition_erase_range(storage, 0, storage->size);
            if (erase_err != ESP_OK) {
                ESP_LOGE(TAG, "storage partition erase failed: %s", esp_err_to_name(erase_err));
            }
        }
    }
    {
        sys::Settings wifi("wifi", true);
        wifi.EraseKey("ssid");
        wifi.EraseKey("password");
        wifi.EraseKey("ap_pass");  // the setup-AP passphrase regenerates when absent
        wifi.EraseKey("force_ap");
    }
    // The driver persists STA credentials in its own nvs.net80211 namespace,
    // untouched by the app "wifi" erase above; best effort — a reboot follows.
    const esp_err_t restore_err = esp_wifi_restore();
    if (restore_err != ESP_OK) {
        ESP_LOGW(TAG, "esp_wifi_restore failed: %s", esp_err_to_name(restore_err));
    }
    {
        sys::Settings tuya("tuya", true);
        tuya.EraseAll();
    }
    {
        sys::Settings hub("hub", true);
        hub.EraseKey("lang");
        hub.EraseKey("openapi_quota");
        // Next boot's InitApiKey generates a fresh random initial key.
        hub.EraseKey("api_key_sha");
        hub.EraseKey("initial_api_key");
        hub.EraseKey("key_is_initial");
    }
    {
        // OpenAPI credentials (password plaintext) + token must not survive.
        sys::Settings openapi("openapi", true);
        openapi.EraseAll();
    }
    // Best effort: the coredump partition is outside NVS; absence/failure is fine.
    const esp_partition_t* coredump = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_COREDUMP, "coredump");
    const esp_err_t coredump_err = coredump == nullptr
                                       ? ESP_ERR_NOT_FOUND
                                       : esp_partition_erase_range(coredump, 0, coredump->size);
    ESP_LOGI(TAG, "Coredump partition erase: %s", esp_err_to_name(coredump_err));
    // No OpenAPI client calls here: they take a mutex an in-flight login can hold
    // for up to 60 s, and the reboot drops the in-memory token anyway.
    esp_err_t err = RespondJson(req, "{\"success\":true}", "200 OK");
    ScheduleReboot();
    return err;
}

}  // namespace

// ---- Public (rest_api.h) ----

void RegisterHandlers(httpd_handle_t server) {
    struct Entry {
        const char* uri;
        httpd_method_t method;
        esp_err_t (*handler)(httpd_req_t* req);
    };
    // 31 /api routes + /ws + the static wildcard = 33 (the httpd handler cap).
    const Entry entries[] = {
        {"/api/status", HTTP_GET, &HandleStatusApi},
        {"/api/logs", HTTP_GET, &HandleLogsApi},
        {"/api/homes", HTTP_GET, &HandleHomesApi},
        {"/api/rooms", HTTP_GET, &HandleRoomsApi},
        {"/api/rooms/refresh", HTTP_POST, &HandleRoomsRefreshApi},
        {"/api/devices", HTTP_GET, &HandleDevicesApi},
        {"/api/devices/*", HTTP_POST, &HandleDeviceWildcardApi},
        {"/api/discovery/scan", HTTP_POST, &HandleDiscoveryScanApi},
        {"/api/discovery/candidates", HTTP_GET, &HandleDiscoveryCandidatesApi},
        {"/api/discovery/candidate/detail", HTTP_GET, &HandleDiscoveryCandidateDetailApi},
        {"/api/discovery/commit", HTTP_POST, &HandleDiscoveryCommitApi},
        {"/api/scenes", HTTP_GET, &HandleScenesApi},
        {"/api/scenes/*", HTTP_POST, &HandleSceneTriggerApi},
        {"/api/auth/start", HTTP_POST, &HandleAuthStartApi},
        {"/api/auth/status", HTTP_GET, &HandleAuthStatusApi},
        {"/api/auth/unlink", HTTP_POST, &HandleAuthUnlinkApi},
        {"/api/settings/language", HTTP_GET, &HandleLanguageGetApi},
        {"/api/settings/language", HTTP_POST, &HandleLanguageSetApi},
        {"/api/settings/apikey", HTTP_GET, &HandleApiKeyGetApi},
        {"/api/settings/apikey", HTTP_POST, &HandleApiKeySetApi},
        {"/api/openapi/quota", HTTP_GET, &HandleOpenApiQuotaApi},
        {"/api/openapi/status", HTTP_GET, &HandleOpenApiStatusApi},
        {"/api/openapi/credentials", HTTP_POST, &HandleOpenApiCredentialsApi},
        {"/api/openapi/clear", HTTP_POST, &HandleOpenApiClearApi},
        {"/api/system/reboot", HTTP_POST, &HandleRebootApi},
        {"/api/system/factory-reset", HTTP_POST, &HandleFactoryResetApi},
        {"/api/asset/info", HTTP_GET, &HandleAssetInfoApi},
        {"/api/asset/upload", HTTP_POST, &HandleAssetUploadApi},
        {"/api/ota/info", HTTP_GET, &HandleOtaInfoApi},
        {"/api/ota/upload", HTTP_POST, &HandleOtaUploadApi},
        {"/api/ota/rollback", HTTP_POST, &HandleOtaRollbackApi},
    };
    // Must match web_server.cc max_uri_handlers (this count + /ws + asset wildcard).
    static_assert(sizeof(entries) / sizeof(entries[0]) == 31,
                  "/api route count changed — update web_server.cc max_uri_handlers");
    for (const Entry& entry : entries) {
        // Plain assignments (not designated initializers): httpd_uri_t's
        // trailing fields would trip -Wextra.
        httpd_uri_t uri = {};
        uri.uri = entry.uri;
        uri.method = entry.method;
        uri.handler = entry.handler;
        uri.user_ctx = nullptr;
        esp_err_t err = httpd_register_uri_handler(server, &uri);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "register %s: %s", entry.uri, esp_err_to_name(err));
        }
    }
}

void SetScheduler(Scheduler scheduler) {
    std::lock_guard<std::mutex> lock(g_scheduler_mutex);
    g_scheduler = std::move(scheduler);
}

void SetDeviceState(DeviceState state) {
    g_device_state.store(static_cast<int>(state), std::memory_order_relaxed);
}

void SetRoomRefreshTrigger(RoomRefreshTrigger trigger) {
    std::lock_guard<std::mutex> lock(g_room_trigger_mutex);
    g_room_refresh_trigger = std::move(trigger);
}

}  // namespace rest_api
}  // namespace web_server
