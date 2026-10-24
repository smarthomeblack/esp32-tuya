#pragma once

#include <cJSON.h>

#include <cstdint>
#include <string>
#include <utility>

namespace tuya_cloud {

/// Outcome of CustomerApi::Request. `ok` = HTTP 2xx AND top-level success == true AND clean
/// decrypt. `result` is owned (freed by the destructor).
struct ApiResponse {
    bool ok = false;
    /// Raw top-level "success" flag (false on non-2xx / unparseable body).
    bool success = false;
    /// Raw top-level "code" (server error code; 0 when absent).
    int64_t code = 0;
    /// Raw top-level "t" — server timestamp in MILLISECONDS.
    int64_t t = 0;
    /// Server "msg", or a locally generated error description.
    std::string msg;
    /// Decrypted "result" parsed as JSON — OWNED; null when absent/not JSON.
    cJSON* result = nullptr;

    ApiResponse() = default;
    ~ApiResponse() { cJSON_Delete(result); }

    ApiResponse(const ApiResponse&) = delete;
    ApiResponse& operator=(const ApiResponse&) = delete;

    ApiResponse(ApiResponse&& other) noexcept
        : ok(other.ok), success(other.success), code(other.code), t(other.t),
          msg(std::move(other.msg)), result(std::exchange(other.result, nullptr)) {}

    ApiResponse& operator=(ApiResponse&& other) noexcept {
        if (this != &other) {
            ok = other.ok;
            success = other.success;
            code = other.code;
            t = other.t;
            msg = std::move(other.msg);
            cJSON_Delete(result);
            result = std::exchange(other.result, nullptr);
        }
        return *this;
    }
};

}  // namespace tuya_cloud
