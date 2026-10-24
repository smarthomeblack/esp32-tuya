// Discovery scan / candidates / detail / commit endpoints.

#include "rest_api_internal.h"

#include <esp_log.h>
#include <freertos/task.h>

#include <cstdio>
#include <cstring>
#include <memory>
#include <set>
#include <string_view>

#include "hub_core/device_registry.h"
#include "hub_services/discovery.h"
#include "web_server/api_auth.h"

static const char* TAG = "RestApi";

namespace web_server {
namespace rest_api {

namespace {

/// GET /api/discovery/candidates response budget.
constexpr size_t kMaxCandidatesResponse = 512 * 1024;
/// POST /api/discovery/commit body cap.
constexpr size_t kMaxCommitBody = 65536;
/// httpd wait for one lazy detail fetch (GET /api/discovery/candidate/detail).
constexpr uint32_t kDetailFetchWaitMs = 15000;
/// httpd wait for the commit detail pre-fetch (N devices, sequential).
constexpr uint32_t kCommitPrefetchWaitMs = 30000;
/// Commit's marshalled registry-write wait (the commit REST timeout).
constexpr uint32_t kCommitTimeoutMs = 30000;
/// Commit pre-fetch retry cadence while the single-flight flag is held
/// (~10 s window at 20 retries, within the 30 s commit budget).
constexpr uint32_t kDetailBusyRetryMs = 500;
constexpr int kDetailBusyRetries = 20;
/// Extra whole-fetch attempts after a degraded (failed-call) detail fetch.
constexpr int kDetailDegradedRetries = 2;

/// Marshalled commit job: entries in, CommitSelectionResult back.
struct DiscoveryCommitJob {
    std::vector<hub_core::DiscoveryCommitEntry> entries;
    hub_core::CommitSelectionResult result;
};

/// Staged rows carry no room: fill it from the registry ("" when not added yet).
void FillCandidateRoom(hub_core::Device& candidate) {
    hub_core::Device known;
    if (hub_core::DeviceRegistry::GetInstance().GetDevice(candidate.id, known)) {
        candidate.room_id = known.room_id;
    }
}

/// The "id" query parameter ("" when absent/unparsable).
std::string CandidateIdParam(httpd_req_t* req) {
    char query[128] = {0};
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK) {
        return "";
    }
    char value[kMaxIdLength + 1] = {0};
    if (httpd_query_key_value(query, "id", value, sizeof(value)) != ESP_OK) {
        return "";
    }
    return value;
}

}  // namespace

// GET /api/discovery/candidates — last completed scan's candidates (staging copy, chunked).
esp_err_t HandleDiscoveryCandidatesApi(httpd_req_t* req) {
    if (RequireAuth(req) != ESP_OK)
        return ESP_FAIL;  // 401 already sent
    hub_core::DiscoveryService& service = hub_core::DiscoveryService::GetInstance();
    const hub_core::DiscoveryStaging staging = service.GetStaging();

    std::vector<std::string> items;
    bool capped = false;
    size_t budget = 0;
    std::vector<hub_core::Device> candidates = staging.candidates;
    for (hub_core::Device& candidate : candidates) {
        FillCandidateRoom(candidate);
    }
    for (const hub_core::Device& candidate : candidates) {
        if (budget > kMaxCandidatesResponse) {
            capped = true;  // stop adding; the list may be truncated
            break;
        }
        cJSON* item =
            BuildCandidateObject(candidate, staging.detail_loaded.count(candidate.id.c_str()) > 0);
        if (item == nullptr) {
            continue;  // OOM — skip this candidate
        }
        char* item_str = cJSON_PrintUnformatted(item);
        cJSON_Delete(item);
        if (item_str == nullptr) {
            continue;
        }
        budget += strlen(item_str) + 1;
        items.emplace_back(item_str);
        cJSON_free(item_str);
    }
    if (capped) {
        ESP_LOGE(TAG, "/api/discovery/candidates exceeded the %u byte budget — truncated",
                 static_cast<unsigned>(kMaxCandidatesResponse));
    }

    char prefix[80];
    snprintf(prefix, sizeof(prefix), "{\"active\":%s,\"done\":%s,\"failed\":%s,\"candidates\":[",
             staging.active ? "true" : "false", staging.done ? "true" : "false",
             staging.failed ? "true" : "false");
    return StreamItemArray(req, prefix, items);
}

// POST /api/discovery/scan — no body; marshalled spawn, Start() returns
// immediately. 409 when a scan is already running.
esp_err_t HandleDiscoveryScanApi(httpd_req_t* req) {
    if (RequireAuth(req) != ESP_OK)
        return ESP_FAIL;  // 401 already sent
    auto ok = std::make_shared<bool>(false);
    const MainLoopResult result =
        RunOnMainLoop([ok]() { *ok = hub_core::DiscoveryService::GetInstance().Start(); });
    if (result != MainLoopResult::kCompleted) {
        return RespondMainLoopError(req, result, "Discovery start timed out");
    }
    if (!*ok) {
        return RespondJsonError(req, "409 Conflict", "discovery_running",
                                "A discovery scan is already running");
    }
    return RespondJson(req, "{\"success\":true}", "200 OK");
}

// GET /api/discovery/candidate/detail?id=<dev> — lazily fetches ONE
// staged candidate's full model on the worker task; the response is that
// candidate object (detail_loaded false when degraded). Error shapes mirror
// the commit handler.
esp_err_t HandleDiscoveryCandidateDetailApi(httpd_req_t* req) {
    if (RequireAuth(req) != ESP_OK)
        return ESP_FAIL;  // 401 already sent
    const std::string id = CandidateIdParam(req);
    if (id.empty()) {
        return RespondJsonError(req, "400 Bad Request", "invalid_param",
                                "Missing 'id' query parameter");
    }

    auto status =
        std::make_shared<hub_core::DetailFetchStatus>(hub_core::DetailFetchStatus::kNotFound);
    std::shared_ptr<WorkerTaskJob> job = RunOnWorkerTask([id, status]() {
        *status = hub_core::DiscoveryService::GetInstance().FetchCandidateDetail(id);
    });
    if (job == nullptr) {
        return RespondJsonError(req, "500 Internal Server Error", "internal",
                                "Failed to start the detail fetch");
    }
    if (xSemaphoreTake(job->done, pdMS_TO_TICKS(kDetailFetchWaitMs)) != pdTRUE) {
        // The task keeps running; its staging updates land when it finishes.
        return RespondJsonError(req, "504 Gateway Timeout", "timeout",
                                "Candidate detail fetch timed out");
    }

    switch (*status) {
        case hub_core::DetailFetchStatus::kLoaded:
        case hub_core::DetailFetchStatus::kDegraded:
        case hub_core::DetailFetchStatus::kAlreadyLoaded: {
            // A degraded fetch also serves its partial model, but it is not
            // authoritative: detail_loaded stays false (server-side uncached).
            const bool detail_loaded = *status != hub_core::DetailFetchStatus::kDegraded;
            const hub_core::DiscoveryStaging staging =
                hub_core::DiscoveryService::GetInstance().GetStaging();
            for (const hub_core::Device& candidate : staging.candidates) {
                if (std::string_view(candidate.id) == id) {
                    hub_core::Device row = candidate;
                    FillCandidateRoom(row);
                    return SendJson(req, BuildCandidateObject(row, detail_loaded));
                }
            }
            return RespondJsonError(req, "404 Not Found", "not_found",
                                    "Unknown device id — not a staged candidate");
        }
        case hub_core::DetailFetchStatus::kNoScan:
            return RespondJsonError(req, "400 Bad Request", "no_scan",
                                    "No completed discovery scan — run a scan first");
        case hub_core::DetailFetchStatus::kNotFound:
            return RespondJsonError(req, "404 Not Found", "not_found",
                                    "Unknown device id — not a staged candidate");
        case hub_core::DetailFetchStatus::kScanActive:
            return RespondJsonError(req, "400 Bad Request", "discovery_active",
                                    "A discovery scan is running — wait for it to complete");
        case hub_core::DetailFetchStatus::kBusy:
            return RespondJsonError(req, "409 Conflict", "detail_fetch_running",
                                    "A candidate detail fetch is already running");
    }
    return RespondJsonError(req, "500 Internal Server Error", "internal", "Unreachable");
}

// POST /api/discovery/commit — body {"devices":[{"id","entities":[...]}]};
// absent = all enabled (filled from the post-fetch model), empty invalid;
// registry-known ids are forwarded (CommitSelection refreshes a strictly-
// poorer model, else skips idempotently); unexpanded devices get their
// model fetched first (spawned task), then the marshalled registry write.
esp_err_t HandleDiscoveryCommitApi(httpd_req_t* req) {
    if (RequireAuth(req) != ESP_OK)
        return ESP_FAIL;  // 401 already sent
    hub_core::DiscoveryService& service = hub_core::DiscoveryService::GetInstance();

    std::string body;
    if (!ReadBody(req, body, kMaxCommitBody, false)) {
        return ESP_FAIL;  // response already sent
    }
    cJSON* root = BodyNestingOk(body) ? cJSON_Parse(body.c_str()) : nullptr;
    if (root == nullptr) {
        return RespondJsonError(req, "400 Bad Request", "invalid_param", "Invalid JSON");
    }
    const cJSON* devices_item = cJSON_GetObjectItemCaseSensitive(root, "devices");
    if (!cJSON_IsArray(devices_item) || cJSON_GetArraySize(devices_item) == 0) {
        cJSON_Delete(root);
        return RespondJsonError(req, "400 Bad Request", "invalid_param",
                                "'devices' must be a non-empty array");
    }

    // Stage 1: shape validation against the parsed body.
    struct ParsedEntry {
        std::string id;
        hub_core::DeviceCodeSet entities;
        bool has_entities = false;
        bool commit = false;  // false = duplicate/already-added (skipped)
    };
    std::vector<ParsedEntry> parsed;
    bool valid = true;
    for (const cJSON* item = devices_item->child; item != nullptr; item = item->next) {
        if (!cJSON_IsObject(item)) {
            valid = false;
            break;
        }
        const cJSON* id_item = cJSON_GetObjectItemCaseSensitive(item, "id");
        if (!cJSON_IsString(id_item) || id_item->valuestring == nullptr ||
            id_item->valuestring[0] == '\0') {
            valid = false;
            break;
        }
        ParsedEntry entry;
        entry.id = id_item->valuestring;
        const cJSON* entities_item = cJSON_GetObjectItemCaseSensitive(item, "entities");
        if (entities_item != nullptr) {
            // Present: must be non-empty (empty would select nothing).
            if (!cJSON_IsArray(entities_item) || cJSON_GetArraySize(entities_item) == 0) {
                valid = false;
                break;
            }
            for (const cJSON* code = entities_item->child; code != nullptr; code = code->next) {
                if (!cJSON_IsString(code) || code->valuestring == nullptr ||
                    code->valuestring[0] == '\0') {
                    valid = false;
                    break;
                }
                entry.entities.insert(code->valuestring);
            }
            if (!valid) {
                break;
            }
            entry.has_entities = true;
        }
        parsed.push_back(std::move(entry));
    }
    cJSON_Delete(root);
    if (!valid || parsed.empty()) {
        return RespondJsonError(req, "400 Bad Request", "invalid_param",
                                "Each device needs a non-empty string 'id' and an optional "
                                "non-empty 'entities' array of non-empty codes");
    }

    // Stage 2: staging-state + id validation against the snapshot copy.
    int skipped = 0;
    std::vector<std::string> pending_detail;
    {
        const hub_core::DiscoveryStaging staging = service.GetStaging();
        if (service.IsRunning() || staging.active) {
            // IsRunning() covers the spawn window; staging.active the tail after Finish.
            return RespondJsonError(req, "400 Bad Request", "discovery_active",
                                    "A discovery scan is running — wait for it to complete");
        }
        if (!staging.done) {
            return RespondJsonError(req, "400 Bad Request", "no_scan",
                                    "No completed discovery scan — run a scan first");
        }
        std::set<std::string> seen;  // duplicate ids in one request -> skipped
        for (ParsedEntry& entry : parsed) {
            const hub_core::Device* candidate = nullptr;
            for (const hub_core::Device& c : staging.candidates) {
                if (std::string_view(c.id) == entry.id) {
                    candidate = &c;
                    break;
                }
            }
            if (candidate == nullptr) {
                char message[128];
                snprintf(message, sizeof(message),
                         "Unknown device id '%.80s' — not a staged "
                         "candidate",
                         entry.id.c_str());
                return RespondJsonError(req, "400 Bad Request", "invalid_param", message);
            }
            if (!seen.insert(entry.id).second) {
                // Duplicate id in one request: counted as skipped (idempotent).
                ++skipped;
                continue;
            }
            // Registry-existing ids forward too: the pre-fetch stages the
            // richer model that triggers CommitSelection's refresh.
            entry.commit = true;
            if (staging.detail_loaded.count(entry.id) == 0) {
                pending_detail.push_back(entry.id);
            }
        }
    }

    // Stage 3: fetch-before-commit — the full model for every selected
    // device whose detail was never loaded (sequential, one task).
    if (!pending_detail.empty()) {
        std::shared_ptr<WorkerTaskJob> job = RunOnWorkerTask([ids = std::move(pending_detail)]() {
            hub_core::DiscoveryService& svc = hub_core::DiscoveryService::GetInstance();
            for (const std::string& dev_id : ids) {
                hub_core::DetailFetchStatus status = hub_core::DetailFetchStatus::kBusy;
                int degraded_left = kDetailDegradedRetries;
                for (int attempt = 0; attempt <= kDetailBusyRetries; ++attempt) {
                    if (attempt > 0) {
                        vTaskDelay(pdMS_TO_TICKS(kDetailBusyRetryMs));
                    }
                    status = svc.FetchCandidateDetail(dev_id);
                    // kBusy waits out the single-flight window; a degraded
                    // fetch gets bounded extra attempts before give-up.
                    if (status == hub_core::DetailFetchStatus::kDegraded && degraded_left > 0) {
                        --degraded_left;
                        continue;
                    }
                    break;
                }
                if (status == hub_core::DetailFetchStatus::kBusy) {
                    // Give-up never blocks the commit — degraded intended.
                    ESP_LOGW(TAG,
                             "commit: device %s still detail-busy after %d retries — "
                             "committing with scan-stage model",
                             dev_id.c_str(), kDetailBusyRetries);
                } else if (status == hub_core::DetailFetchStatus::kDegraded) {
                    ESP_LOGW(TAG,
                             "commit: device %s detail fetch still degraded after %d "
                             "retries — committing with partial model",
                             dev_id.c_str(), kDetailDegradedRetries);
                }
            }
        });
        if (job == nullptr) {
            return RespondJsonError(req, "500 Internal Server Error", "internal",
                                    "Failed to start the detail pre-fetch");
        }
        if (xSemaphoreTake(job->done, pdMS_TO_TICKS(kCommitPrefetchWaitMs)) != pdTRUE) {
            // Fetches keep running server-side; nothing is committed yet.
            return RespondJsonError(req, "504 Gateway Timeout", "timeout",
                                    "Discovery commit timed out");
        }
    }

    // Stage 4: build the entries from the POST-fetch staging — the absent-
    // entities fill must see the fetched model, not the scan-stage row.
    auto job = std::make_shared<DiscoveryCommitJob>();
    {
        const hub_core::DiscoveryStaging fresh = service.GetStaging();
        if (service.IsRunning() || fresh.active || !fresh.done) {
            // A scan raced in during the pre-fetch.
            return RespondJsonError(req, "400 Bad Request", "discovery_active",
                                    "A discovery scan is running — wait for it to complete");
        }
        for (const ParsedEntry& entry : parsed) {
            if (!entry.commit) {
                continue;
            }
            const hub_core::Device* candidate = nullptr;
            for (const hub_core::Device& c : fresh.candidates) {
                if (std::string_view(c.id) == entry.id) {
                    candidate = &c;
                    break;
                }
            }
            if (candidate == nullptr) {
                // The staging was rebuilt mid-commit — count as skipped.
                ++skipped;
                continue;
            }
            hub_core::DiscoveryCommitEntry commit_entry;
            commit_entry.id = entry.id;
            if (!entry.has_entities) {
                // Absent entities field = "all enabled" (the full model).
                commit_entry.enabled_entities = CollectEntityCodes(*candidate);
            } else {
                commit_entry.enabled_entities = entry.entities;
            }
            job->entries.push_back(std::move(commit_entry));
        }
    }

    // Stage 5: the registry mutation on the main event loop.
    const MainLoopResult result = RunOnMainLoop(
        [job]() {
            job->result = hub_core::DiscoveryService::GetInstance().CommitSelection(job->entries);
        },
        kCommitTimeoutMs);
    if (result != MainLoopResult::kCompleted) {
        return RespondMainLoopError(req, result, "Discovery commit timed out");
    }
    if (job->result.scan_active) {
        // A scan raced in between the validation and the marshalled job.
        return RespondJsonError(req, "400 Bad Request", "discovery_active",
                                "A discovery scan is running — wait for it to complete");
    }
    cJSON* resp = cJSON_CreateObject();
    if (resp == nullptr) {
        return RespondJsonError(req, "500 Internal Server Error", "internal", "Out of memory");
    }
    cJSON_AddBoolToObject(resp, "success", true);
    // Refreshes report as applied: the registry write happened.
    cJSON_AddNumberToObject(resp, "added",
                            static_cast<double>(job->result.added + job->result.refreshed));
    cJSON_AddNumberToObject(resp, "skipped", static_cast<double>(skipped + job->result.skipped));
    return SendJson(req, resp);
}

}  // namespace rest_api
}  // namespace web_server
