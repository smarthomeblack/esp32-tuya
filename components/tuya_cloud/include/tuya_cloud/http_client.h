#pragma once

#include <string>
#include <vector>

#include "tuya_cloud/http_method.h"

namespace tuya_cloud {

/// Blocking HTTP/HTTPS wrapper over esp_http_client (ESP-IDF cert bundle,
/// single attempt, no retries). Thread-safe: TLS connections are pooled and
/// reused per scheme://host[:port]; requests to one host serialize on it.
class HttpClient {
public:
    struct Header {
        std::string name;
        std::string value;
    };

    struct Result {
        /// True when the response was received completely; status_code is valid only then.
        bool network_ok = false;
        /// HTTP status code (0 when the request never got that far).
        int status_code = 0;
        std::string body;
    };

    /// One blocking request. `body` is sent for POST/PUT only; Content-Type: application/json
    /// is auto-added with a body. Custom headers are applied verbatim, including empty values.
    Result Request(const std::string& url, HttpMethod method, const std::vector<Header>& headers,
                   const std::string& body);
};

}  // namespace tuya_cloud
