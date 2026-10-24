#pragma once

#include <cstdint>
#include <string>

#include "tuya_cloud/credentials.h"

namespace tuya_cloud {

/// Outcome of the QR-token request.
/// `network_ok` = HTTP cycle completed AND the body parsed as JSON; `success` is the raw top-level
/// flag.
struct LoginQrResult {
    bool network_ok = false;
    bool success = false;
    /// result.qrcode — the QR token.
    std::string qrcode;
    int64_t code = 0;
    std::string msg;
    int64_t t = 0;  // server timestamp, ms
};

/// PLAINTEXT login-gateway POST (kPathQrToken): no body, no X-* headers, no signature, no
/// encryption. Query values are percent-encoded (user_code is user-typed). Pure function; any
/// single task.
LoginQrResult LoginControlQrRequest(const std::string& user_code);

/// Outcome of the login-result poll (same network_ok/success contract as LoginQrResult).
/// success=false while the user has not scanned/approved is the NORMAL waiting state, not an error.
struct LoginPollResult {
    bool network_ok = false;
    bool success = false;
    std::string msg;
    int64_t code = 0;
    // ---- filled when success ----
    /// TokenInfo from the poll result (expire_time SECONDS, t MILLISECONDS).
    TokenInfo token;
    std::string terminal_id;
    std::string endpoint;  // per-account API base URL, used verbatim later
    std::string username;  // display name of the linked account
};

/// PLAINTEXT login-gateway GET (kPathQrTokenPoll); the QR token goes into the path percent-encoded.
/// Pure function, same threading rules as LoginControlQrRequest.
LoginPollResult LoginControlPoll(const std::string& qr_token, const std::string& user_code);

}  // namespace tuya_cloud
