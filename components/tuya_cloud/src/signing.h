#pragma once

// Src-internal signing/pipeline stage declarations; definitions live in
// customer_api.cc / openapi_client.cc. Consumed by the self-test TUs only.

#include <cstdint>
#include <string>
#include <string_view>

#include "tuya_cloud/openapi_client.h"

namespace tuya_cloud {

/// RFC 3986 percent-encoding; space becomes %20 (NOT '+'), hex uppercase.
std::string PercentEncode(std::string_view value);

/// Wire-level signing result; `token` "" omits X-token, `sid` always "" (sent but sign-skipped).
struct BuiltRequest {
    std::string url;         // endpoint + path [+ "?encdata=" + PercentEncode(query_encdata)]
    std::string body;        // {"encdata":"..."} or ""
    std::string app_key;     // X-appKey
    std::string request_id;  // X-requestId
    std::string sid;         // X-sid (always "")
    std::string time;        // X-time (ms, decimal string)
    std::string token;       // X-token ("" -> header not sent)
    std::string sign;        // X-sign
};

/// Pure signing stage: secret derivation, AES-GCM encdata packing, X-sign, url/body assembly.
/// `nonce12` must be 12 chars; params and body need independent nonces (IV reuse leaks XOR).
BuiltRequest BuildSignedRequest(const std::string& endpoint, std::string_view path,
                                const std::string& rid, const std::string& access_token,
                                const std::string& refresh_token, int64_t time_ms,
                                const std::string& nonce12, const std::string& params_json,
                                const std::string& body_json);

// Pure, golden-vector-testable stages of the OpenAPI pipeline.

/// Sign path: path + (params empty ? "" : "?" + pairs sorted by name, "k=v" joined with "&",
/// values RAW — no percent-encoding).
std::string BuildOpenApiSignPath(const std::string& path, const OpenApiParams& params);

/// Request URL: endpoint + path + params percent-encoded in sign order.
std::string BuildOpenApiUrl(const std::string& endpoint, const std::string& path,
                            const OpenApiParams& params);

/// Replaces every "{uid}", "{home_id}" and "{room_id}" placeholder (raw);
/// "{id}" is resolved by the caller.
std::string ResolveOpenApiTemplate(const std::string& path_template, const std::string& uid,
                                   const std::string& home_id = "",
                                   const std::string& room_id = "");

/// HMAC-SHA256 sign, hex UPPERCASE: client_id + access_token + t + method + "\n" +
/// sha256_hex(body_json) + "\n\n" + path_with_params. Empty access_token is omitted.
std::string BuildOpenApiSign(const std::string& client_id, const std::string& access_token,
                             const std::string& access_secret, int64_t t_ms,
                             const std::string& method, const std::string& body_json,
                             const std::string& path_with_params);

/// True when the token is absent/unusable or within its last 5 minutes of validity.
bool OpenApiTokenNeedsRefresh(const OpenApiToken& token, int64_t now_ms);

/// SMART_HOME login body (compact JSON); the password is MD5 lowercase hex.
std::string BuildOpenApiLoginBodySmartHome(const std::string& username, const std::string& password,
                                           const std::string& country_code,
                                           const std::string& schema);

/// CUSTOM login body; the password is SHA-256 lowercase hex.
std::string BuildOpenApiLoginBodyCustom(const std::string& username, const std::string& password);

}  // namespace tuya_cloud
