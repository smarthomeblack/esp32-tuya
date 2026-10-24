// Host stubs for the tuya_cloud transport/persistence seam: the pure-logic
// self-test suites never perform HTTP, SNTP or NVS I/O, so these only
// satisfy link-time references of the compiled client TUs.

#include "tuya_cloud/credentials.h"
#include "tuya_cloud/http_client.h"
#include "tuya_cloud/time_sync.h"

#include <sys/time.h>

namespace tuya_cloud {

HttpClient::Result HttpClient::Request(const std::string& url, HttpMethod method,
                                       const std::vector<Header>& headers,
                                       const std::string& body) {
    (void)url;
    (void)method;
    (void)headers;
    (void)body;
    return Result{};  // network_ok = false
}

int64_t TimeSync::NowMs() {
    struct timeval tv;
    gettimeofday(&tv, nullptr);
    return static_cast<int64_t>(tv.tv_sec) * 1000 + tv.tv_usec / 1000;
}

bool TimeSync::SyncBlocking(uint32_t timeout_ms) {
    (void)timeout_ms;
    return false;  // no network on the host runner
}

bool OpenApiCredentialsStore::Load(OpenApiCredentials& creds) {
    (void)creds;
    return false;
}

void OpenApiCredentialsStore::Save(const OpenApiCredentials& creds) {
    (void)creds;
}

void OpenApiCredentialsStore::SaveToken(const OpenApiToken& token) {
    (void)token;
}

bool OpenApiCredentialsStore::LoadToken(OpenApiToken& token) {
    (void)token;
    return false;
}

void OpenApiCredentialsStore::ClearToken() {}

void OpenApiCredentialsStore::ClearAll() {}

}  // namespace tuya_cloud
