#include "login_control.h"

#include <esp_log.h>

#include <cJSON.h>

#include <string>

#include "tuya_cloud/constants.h"
#include "tuya_cloud/http_client.h"
#include "tuya_crypto/json_fields.h"
#include "tuya_crypto/json_guard.h"

#include "signing.h"

static const char* TAG = "LoginControl";

namespace tuya_cloud {

namespace {

/// Builds the plaintext query "?clientid=...&usercode=..."; values percent-encoded.
std::string BuildLoginQuery(const std::string& user_code) {
    std::string query = "?clientid=";
    query += PercentEncode(kClientId);
    query += "&usercode=";
    query += PercentEncode(user_code);
    return query;
}

/// Parses the envelope {success, code, msg, t}; false when the body is not a JSON object.
bool ParseEnvelope(const std::string& body, bool* success, int64_t* code, std::string* msg,
                   int64_t* t, cJSON** root_out) {
    if (!tuya_crypto::JsonNestingWithinLimit(body.c_str(), body.size(),
                                             tuya_crypto::kMaxJsonDepth)) {
        return false;  // callers report "body is not JSON"
    }
    *root_out = cJSON_ParseWithLength(body.c_str(), body.size());
    if (*root_out == nullptr || !cJSON_IsObject(*root_out)) {
        cJSON_Delete(*root_out);
        *root_out = nullptr;
        return false;
    }
    const cJSON* success_item = cJSON_GetObjectItemCaseSensitive(*root_out, "success");
    *success = cJSON_IsTrue(success_item) ? true : false;
    *code = tuya_crypto::GetInt64Field(*root_out, "code");
    *msg = tuya_crypto::GetStringField(*root_out, "msg");
    *t = tuya_crypto::GetInt64Field(*root_out, "t");
    return true;
}

}  // namespace

LoginQrResult LoginControlQrRequest(const std::string& user_code) {
    LoginQrResult result;

    // POST kPathQrToken with clientid/usercode/schema query — no body, no X-* headers.
    std::string url(kLoginHost);
    url += kPathQrToken;
    url += BuildLoginQuery(user_code);
    url += "&schema=";
    url += PercentEncode(kSchema);

    HttpClient http;
    const HttpClient::Result resp = http.Request(url, HttpMethod::kPost, {}, "");

    result.network_ok = resp.network_ok;
    if (!resp.network_ok) {
        result.msg = "network failure";  // details already logged by HttpClient
        return result;
    }

    bool success = false;
    cJSON* root = nullptr;
    if (!ParseEnvelope(resp.body, &success, &result.code, &result.msg, &result.t, &root)) {
        result.network_ok = false;
        result.msg = "http " + std::to_string(resp.status_code) + ": body is not JSON";
        return result;
    }
    tuya_crypto::JsonRoot guard{root};

    result.success = success;
    if (!success) {
        ESP_LOGI(TAG, "QR request rejected: code=%lld msg=%s", static_cast<long long>(result.code),
                 result.msg.c_str());
        return result;
    }

    // {success:true, result:{qrcode}}.
    const cJSON* result_item = cJSON_GetObjectItemCaseSensitive(root, "result");
    if (cJSON_IsObject(result_item)) {
        result.qrcode = tuya_crypto::GetStringField(result_item, "qrcode");
    }
    if (result.qrcode.empty()) {
        // A server-side failure, not a retryable transport error.
        result.success = false;
        result.msg = "success without a qrcode";
        ESP_LOGE(TAG, "QR response had success=true but no result.qrcode");
        return result;
    }

    ESP_LOGI(TAG, "QR token acquired (%u B response)", static_cast<unsigned>(resp.body.size()));
    return result;
}

LoginPollResult LoginControlPoll(const std::string& qr_token, const std::string& user_code) {
    LoginPollResult result;

    // The {token} placeholder is substituted percent-encoded.
    std::string path(kPathQrTokenPoll);
    constexpr std::string_view kTokenPlaceholder = "{token}";
    const size_t pos = path.find(kTokenPlaceholder);
    if (pos != std::string::npos) {
        path.replace(pos, kTokenPlaceholder.size(), PercentEncode(qr_token));
    }
    std::string url(kLoginHost);
    url += path;
    url += BuildLoginQuery(user_code);

    HttpClient http;
    const HttpClient::Result resp = http.Request(url, HttpMethod::kGet, {}, "");

    result.network_ok = resp.network_ok;
    if (!resp.network_ok) {
        result.msg = "network failure";  // details already logged by HttpClient
        return result;
    }

    bool success = false;
    int64_t t = 0;
    cJSON* root = nullptr;
    if (!ParseEnvelope(resp.body, &success, &result.code, &result.msg, &t, &root)) {
        result.network_ok = false;
        result.msg = "http " + std::to_string(resp.status_code) + ": body is not JSON";
        return result;
    }
    tuya_crypto::JsonRoot guard{root};

    result.success = success;
    if (!success) {
        // Normal while the user has not scanned/approved yet.
        ESP_LOGD(TAG, "login not complete yet: code=%lld msg=%s",
                 static_cast<long long>(result.code), result.msg.c_str());
        return result;
    }

    // result fields; expire_time seconds, top-level t ms.
    const cJSON* result_item = cJSON_GetObjectItemCaseSensitive(root, "result");
    if (cJSON_IsObject(result_item)) {
        result.token.access_token = tuya_crypto::GetStringField(result_item, "access_token");
        result.token.refresh_token = tuya_crypto::GetStringField(result_item, "refresh_token");
        result.token.uid = tuya_crypto::GetStringField(result_item, "uid");
        result.token.expire_time =
            tuya_crypto::GetInt64Field(result_item, "expire_time");  // seconds
        result.token.t = t;                                          // ms
        result.terminal_id = tuya_crypto::GetStringField(result_item, "terminal_id");
        result.endpoint = tuya_crypto::GetStringField(result_item, "endpoint");
        result.username = tuya_crypto::GetStringField(result_item, "username");
    }

    // Validate; nothing sensitive is logged — lengths only.
    if (result.token.access_token.empty() || result.token.refresh_token.empty() ||
        result.token.uid.empty() || result.terminal_id.empty() || result.endpoint.empty() ||
        result.token.expire_time <= 0 || result.token.t <= 0) {
        result.success = false;
        result.msg = "login result incomplete";
        ESP_LOGE(TAG,
                 "login success flag set but the result is unusable "
                 "(at=%u rt=%u uid=%u tid=%u ep=%u exp=%lld t=%lld)",
                 static_cast<unsigned>(result.token.access_token.size()),
                 static_cast<unsigned>(result.token.refresh_token.size()),
                 static_cast<unsigned>(result.token.uid.size()),
                 static_cast<unsigned>(result.terminal_id.size()),
                 static_cast<unsigned>(result.endpoint.size()),
                 static_cast<long long>(result.token.expire_time),
                 static_cast<long long>(result.token.t));
        return result;
    }

    ESP_LOGI(TAG, "login approved (uid=%s username=%s, expires in %llds)", result.token.uid.c_str(),
             result.username.c_str(), static_cast<long long>(result.token.expire_time));
    return result;
}

}  // namespace tuya_cloud
