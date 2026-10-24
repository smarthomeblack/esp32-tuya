// httpd request/response plumbing + main-loop/worker-task marshalling.
// JSON via cJSON only; errors are {"success":false,"error":{...}}.

#include "rest_api_internal.h"

#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_system.h>
#include <freertos/task.h>

#include <cstring>
#include <memory>

#include "tuya_crypto/json_guard.h"

static const char* TAG = "RestApi";

namespace web_server {
namespace rest_api {

namespace {

/// Worker-task profile for every RunOnWorkerTask job (HTTPS + cJSON, same
/// class as the login task).
constexpr uint32_t kWorkerTaskStackBytes = 12288;
constexpr UBaseType_t kWorkerTaskPriority = 3;

/// Marshalled job; the shared_ptr keeps it alive through the timeout path.
struct MainLoopJob {
    std::function<void()> body;
    SemaphoreHandle_t done = nullptr;

    ~MainLoopJob() {
        if (done != nullptr) {
            vSemaphoreDelete(done);
        }
    }
};

// Never-drop hook state (registered via SetCriticalScheduler).
std::mutex g_critical_scheduler_mutex;
Scheduler g_critical_scheduler;

}  // namespace

// ---- Response helpers ----

esp_err_t RespondJson(httpd_req_t* req, const char* body, const char* status) {
    httpd_resp_set_status(req, status);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, body, HTTPD_RESP_USE_STRLEN);
}

esp_err_t RespondJsonError(httpd_req_t* req, const char* status, const char* code,
                           const char* message) {
    cJSON* root = cJSON_CreateObject();
    if (root == nullptr) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Out of memory");
        return ESP_FAIL;
    }
    cJSON_AddBoolToObject(root, "success", false);
    cJSON* error = cJSON_AddObjectToObject(root, "error");
    if (error != nullptr) {
        cJSON_AddStringToObject(error, "code", code);
        cJSON_AddStringToObject(error, "message", message);
    }
    char* body = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (body == nullptr) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Out of memory");
        return ESP_FAIL;
    }
    httpd_resp_set_status(req, status);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    esp_err_t err = httpd_resp_send(req, body, HTTPD_RESP_USE_STRLEN);
    cJSON_free(body);
    return err;
}

esp_err_t SendJson(httpd_req_t* req, cJSON* root, const char* status) {
    if (root == nullptr) {
        return RespondJsonError(req, "500 Internal Server Error", "internal", "Out of memory");
    }
    char* body = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (body == nullptr) {
        return RespondJsonError(req, "500 Internal Server Error", "internal", "Out of memory");
    }
    esp_err_t err = RespondJson(req, body, status);
    cJSON_free(body);
    return err;
}

// ---- Request helpers ----

bool ReadBody(httpd_req_t* req, std::string& out, size_t max_len, bool allow_empty) {
    if (req->content_len == 0) {
        if (allow_empty) {
            out.clear();
            return true;
        }
        RespondJsonError(req, "400 Bad Request", "invalid_param", "Empty body");
        return false;
    }
    if (req->content_len > max_len) {
        RespondJsonError(req, "400 Bad Request", "invalid_param", "Body too large");
        return false;
    }
    out.resize(req->content_len);
    size_t total = 0;
    while (total < req->content_len) {
        int ret = httpd_req_recv(req, out.data() + total, req->content_len - total);
        if (ret == HTTPD_SOCK_ERR_TIMEOUT) {
            httpd_resp_send_408(req);
            return false;
        }
        if (ret <= 0) {
            RespondJsonError(req, "400 Bad Request", "invalid_param", "recv_failed");
            return false;
        }
        total += static_cast<size_t>(ret);
    }
    return true;
}

std::string UrlPath(const httpd_req_t* req) {
    std::string uri(req->uri);
    const size_t q = uri.find('?');
    if (q != std::string::npos) {
        uri.resize(q);
    }
    return uri;
}

bool BodyNestingOk(const std::string& body) {
    return tuya_crypto::JsonNestingWithinLimit(body.data(), body.size(),
                                               tuya_crypto::kMaxJsonDepth);
}

uint8_t* ReadBinaryBody(httpd_req_t* req, size_t max_len) {
    if (req->content_len == 0) {
        RespondJsonError(req, "400 Bad Request", "invalid_param", "Content-Length required");
        return nullptr;
    }
    if (req->content_len > max_len) {
        RespondJsonError(req, "400 Bad Request", "too_large", "Body exceeds the maximum size");
        return nullptr;
    }
    uint8_t* buf = static_cast<uint8_t*>(heap_caps_malloc(req->content_len, MALLOC_CAP_SPIRAM));
    if (buf == nullptr) {
        RespondJsonError(req, "500 Internal Server Error", "internal", "Out of memory");
        return nullptr;
    }
    size_t total = 0;
    while (total < req->content_len) {
        int ret =
            httpd_req_recv(req, reinterpret_cast<char*>(buf + total), req->content_len - total);
        if (ret == HTTPD_SOCK_ERR_TIMEOUT) {
            httpd_resp_send_408(req);
            heap_caps_free(buf);
            return nullptr;
        }
        if (ret <= 0) {
            RespondJsonError(req, "400 Bad Request", "invalid_param", "recv_failed");
            heap_caps_free(buf);
            return nullptr;
        }
        total += static_cast<size_t>(ret);
    }
    return buf;
}

bool ParseIdPath(const httpd_req_t* req, const char* prefix, const char* suffix, std::string& id) {
    const std::string path = UrlPath(req);
    const size_t prefix_len = strlen(prefix);
    const size_t suffix_len = strlen(suffix);
    if (path.size() <= prefix_len + suffix_len) {
        return false;  // no room for an id
    }
    if (path.compare(0, prefix_len, prefix) != 0) {
        return false;
    }
    if (path.compare(path.size() - suffix_len, suffix_len, suffix) != 0) {
        return false;
    }
    id = path.substr(prefix_len, path.size() - prefix_len - suffix_len);
    if (id.empty() || id.size() > kMaxIdLength) {
        return false;
    }
    // Device ids are [A-Za-z0-9-]; a slash is a nested path we do not serve.
    return id.find('/') == std::string::npos;
}

std::string HomeIdFilter(httpd_req_t* req) {
    char query[128] = {0};
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK) {
        return "";
    }
    char value[kMaxIdLength + 1] = {0};
    if (httpd_query_key_value(query, "home_id", value, sizeof(value)) != ESP_OK) {
        return "";
    }
    return value;
}

void ScheduleReboot() {
    BaseType_t ok = xTaskCreate(
        [](void*) {
            vTaskDelay(pdMS_TO_TICKS(kRebootDelayMs));
            ESP_LOGW(TAG, "Rebooting (web request)");
            esp_restart();
        },
        "hub_web_reboot", 3072, nullptr, 5, nullptr);
    if (ok != pdPASS) {
        ESP_LOGE(TAG, "Failed to create the reboot task — rebooting immediately");
        esp_restart();
    }
}

// ---- Main-loop marshalling (Schedule + semaphore) ----

MainLoopResult RunOnMainLoop(std::function<void()> body, uint32_t timeout_ms) {
    auto job = std::make_shared<MainLoopJob>();
    if (job == nullptr) {
        return MainLoopResult::kNoMem;
    }
    job->body = std::move(body);
    job->done = xSemaphoreCreateBinary();
    if (job->done == nullptr) {
        return MainLoopResult::kNoMem;
    }
    Scheduler scheduler;
    {
        std::lock_guard<std::mutex> lock(g_scheduler_mutex);
        scheduler = g_scheduler;
    }
    if (!scheduler) {
        return MainLoopResult::kNotWired;
    }
    scheduler([job]() {
        job->body();
        xSemaphoreGive(job->done);
    });
    if (xSemaphoreTake(job->done, pdMS_TO_TICKS(timeout_ms)) != pdTRUE) {
        ESP_LOGW(TAG, "Main-loop job timed out after %u ms — answering 504",
                 static_cast<unsigned>(timeout_ms));
        return MainLoopResult::kTimeout;
    }
    return MainLoopResult::kCompleted;
}

void SetCriticalScheduler(Scheduler scheduler) {
    std::lock_guard<std::mutex> lock(g_critical_scheduler_mutex);
    g_critical_scheduler = std::move(scheduler);
}

MainLoopResult RunOnMainLoopCritical(std::function<void()> body, uint32_t timeout_ms) {
    auto job = std::make_shared<MainLoopJob>();
    if (job == nullptr) {
        return MainLoopResult::kNoMem;
    }
    job->body = std::move(body);
    job->done = xSemaphoreCreateBinary();
    if (job->done == nullptr) {
        return MainLoopResult::kNoMem;
    }
    Scheduler scheduler;
    {
        std::lock_guard<std::mutex> lock(g_critical_scheduler_mutex);
        scheduler = g_critical_scheduler;
    }
    if (!scheduler) {
        ESP_LOGE(TAG, "Critical scheduler not registered — job rejected");
        return MainLoopResult::kNotWired;
    }
    scheduler([job]() {
        job->body();
        xSemaphoreGive(job->done);
    });
    if (xSemaphoreTake(job->done, pdMS_TO_TICKS(timeout_ms)) != pdTRUE) {
        ESP_LOGW(TAG, "Main-loop job timed out after %u ms — answering 504",
                 static_cast<unsigned>(timeout_ms));
        return MainLoopResult::kTimeout;
    }
    return MainLoopResult::kCompleted;
}

esp_err_t RespondMainLoopError(httpd_req_t* req, MainLoopResult result, const char* what) {
    switch (result) {
        case MainLoopResult::kTimeout:
            return RespondJsonError(req, "504 Gateway Timeout", "timeout", what);
        case MainLoopResult::kNotWired:
        case MainLoopResult::kNoMem:
        default:
            return RespondJsonError(req, "500 Internal Server Error", "internal",
                                    "Main-loop scheduler unavailable");
    }
}

// ---- Worker-task marshalling (blocking cloud I/O) ----

std::shared_ptr<WorkerTaskJob> RunOnWorkerTask(std::function<void()> body) {
    auto job = std::make_shared<WorkerTaskJob>();
    if (job == nullptr) {
        return nullptr;
    }
    job->body = std::move(body);
    job->done = xSemaphoreCreateBinary();
    if (job->done == nullptr) {
        return nullptr;
    }
    auto holder = std::make_unique<std::shared_ptr<WorkerTaskJob>>(job);
    const BaseType_t ok = xTaskCreate(
        [](void* arg) {
            auto* held = static_cast<std::shared_ptr<WorkerTaskJob>*>(arg);
            (*held)->body();
            xSemaphoreGive((*held)->done);
            delete held;  // the task's reference — released after the give
            vTaskDelete(nullptr);
        },
        "web_worker", kWorkerTaskStackBytes, holder.get(), kWorkerTaskPriority, nullptr);
    if (ok != pdPASS) {
        ESP_LOGE(TAG, "Failed to create the worker task");
        return nullptr;  // holder frees via the unique_ptr
    }
    holder.release();  // handed off — the trampoline owns the reference
    return job;
}

WorkerResult RunOnWorkerAndWait(std::function<void()> body, uint32_t timeout_ms) {
    std::shared_ptr<WorkerTaskJob> job = RunOnWorkerTask(std::move(body));
    if (job == nullptr) {
        return WorkerResult::kSpawnFailed;
    }
    if (xSemaphoreTake(job->done, pdMS_TO_TICKS(timeout_ms)) != pdTRUE) {
        ESP_LOGW(TAG, "Worker job timed out after %u ms — answering 504",
                 static_cast<unsigned>(timeout_ms));
        return WorkerResult::kTimeout;
    }
    return WorkerResult::kCompleted;
}

esp_err_t RespondWorkerError(httpd_req_t* req, WorkerResult result, const char* what) {
    switch (result) {
        case WorkerResult::kTimeout:
            return RespondJsonError(req, "504 Gateway Timeout", "timeout", what);
        case WorkerResult::kSpawnFailed:
        default:
            return RespondJsonError(req, "500 Internal Server Error", "internal",
                                    "Failed to start the worker job");
    }
}

}  // namespace rest_api
}  // namespace web_server
