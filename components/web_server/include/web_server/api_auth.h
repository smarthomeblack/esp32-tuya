#pragma once

#include <esp_http_server.h>

#include <cstddef>

namespace web_server {
namespace rest_api {

/// Boot-time init (call once before networking): when no key hash is
/// stored, generates a random per-device initial key (hash + flag +
/// plaintext in NVS "hub") and prints it on the serial log; reprints it
/// on every later boot while the key stays un-rotated.
void InitApiKey();

/// True until the user rotates the key (NVS "hub"/"key_is_initial").
bool ApiKeyIsInitial();

/// True when `key` equals the stored key (SHA-256 hex in NVS
/// "hub"/"api_key_sha"; absent = nothing matches; constant-time compare).
bool ApiKeyMatches(const char* key, size_t len);

/// Validates `key` (8..64 printable chars) and stores its SHA-256 hex;
/// a successful rotation drops the initial-key bookkeeping.
bool ApiKeyStore(const char* key, size_t len);

/// Per-IP brute-force lockout shared by the REST and /ws gates: more
/// than 10 failed key checks within 60 s -> 300 s of fast 401s.
bool AuthLockedOut(httpd_req_t* req);

/// Counts one failed key check (may engage the lockout; logs once).
void RecordAuthFailure(httpd_req_t* req);

/// Resets the peer's failure window after a successful key check.
void RecordAuthSuccess(httpd_req_t* req);

/// Bearer-token gate for /api/* handlers: 401 JSON + WWW-Authenticate on
/// failure (response already sent — return ESP_FAIL) with no delay; locked-out
/// peers fail before the key check.
esp_err_t RequireAuth(httpd_req_t* req);

/// /ws handshake gate: true when the `?token=` query param matches the
/// stored key (percent-decoded, compared through the same SHA-256 path).
bool WsTokenOk(httpd_req_t* req);

}  // namespace rest_api
}  // namespace web_server
