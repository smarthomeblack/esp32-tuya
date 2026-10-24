#include "tuya_cloud/openapi_self_test.h"

#include <esp_log.h>

#include <cstdint>
#include <string>

#include "signing.h"
#include "tuya_cloud/openapi_client.h"
#include "tuya_cloud/openapi_constants.h"
#include "tuya_crypto/sha256.h"

static const char* TAG = "TuyaIoTTest";

namespace tuya_cloud {

namespace {

// Golden vectors — computed with REAL Python 3 (hmac/hashlib/json), never invented.
// The sign vectors pin the exact wire bytes: sign-body == sent-body (compact JSON).

constexpr const char* kClientId = "test123";
constexpr const char* kAccessToken = "tok456";
constexpr const char* kAccessSecret = "testsecret789";
constexpr const char* kUsername = "user@example.com";
constexpr const char* kPassword = "s3cr3t-PW";
constexpr const char* kCountryCode = "1";
constexpr const char* kSchema = "tuyaSmart";

constexpr int64_t kT_A = 1731838024000;
constexpr int64_t kT_B = 1731838025000;
constexpr int64_t kT_C = 1731838026000;
constexpr int64_t kT_D = 1731838027000;

constexpr const char* kPathDevices = "/v1.0/users/abc123/devices";
constexpr const char* kPathLoginSmartHome =
    "/v1.0/iot-01/associated-users/actions/authorized-login";
constexpr const char* kBodyNameValue = "{\"name\":\"value\"}";

// sha256(""), sha256('{"name":"value"}'), md5/sha256 of the password.
constexpr const char* kGoldenSha256Empty =
    "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855";
constexpr const char* kGoldenSha256Body =
    "ae1fca77a81ea8b568ef60cdad1dee6bae1faaf716cddca2f5750b7cdc9b6ed4";
constexpr const char* kGoldenMd5Password = "1b8ba3563ae5a75116fa5d5d18582380";
constexpr const char* kGoldenSha256Password =
    "f1b080cb2d629410776dd0a17349b9b080eb8ff05e6d2194dfc717269658ce02";

// Login bodies: dict order {username, password, country_code, schema}, compact JSON.
constexpr const char* kGoldenLoginBodySmartHome =
    "{\"username\":\"user@example.com\",\"password\":\"1b8ba3563ae5a75116fa5d5d18582380\","
    "\"country_code\":\"1\",\"schema\":\"tuyaSmart\"}";
constexpr const char* kGoldenLoginBodyCustom =
    "{\"username\":\"user@example.com\","
    "\"password\":\"f1b080cb2d629410776dd0a17349b9b080eb8ff05e6d2194dfc717269658ce02\"}";

// HMAC-SHA256(key=access_secret, message) hexdigest UPPERCASE.
constexpr const char* kGoldenSignA =
    "05538B656B7B757AD05C6D83533A1D2E236877996037F1D6A43EFFDDC2EE88E1";  // GET, empty body
constexpr const char* kGoldenSignB =
    "50E8D9792FC1A9C29C430E5AD31A8FAC903447A250581755C630E6E795D22A63";  // POST body
constexpr const char* kGoldenSignC =
    "B04D5BE086233A7BE2CD41CD9CE79C84DA1F2DC21008D3AA7B90019016AA153F";  // GET sorted params
constexpr const char* kGoldenSignD =
    "2296F2EF56F54A106FE4D695B8583BFD2926E61D650B74B4B06D0D56F2E93EC6";  // login, no token

// ---------------------------------------------------------------------------

struct TestResult {
    int total = 0;
    int passed = 0;
};

std::string TruncateForLog(const std::string& s, size_t max_chars = 100) {
    if (s.size() <= max_chars) {
        return s;
    }
    return s.substr(0, max_chars) + "...(" + std::to_string(s.size()) + "B)";
}

void CheckEqStr(TestResult& r, const char* case_name, const char* field,
                const std::string& expected, const std::string& got) {
    ++r.total;
    if (expected == got) {
        ++r.passed;
        return;
    }
    ESP_LOGE(TAG, "FAIL %s.%s: expected=\"%s\" got=\"%s\"", case_name, field,
             TruncateForLog(expected).c_str(), TruncateForLog(got).c_str());
}

void CheckTrue(TestResult& r, const char* case_name, bool ok, const std::string& detail = "") {
    ++r.total;
    if (ok) {
        ++r.passed;
        return;
    }
    ESP_LOGE(TAG, "FAIL %s%s%s", case_name, detail.empty() ? "" : ": ", detail.c_str());
}

void TestSha256Primitive(TestResult& r) {
    // The tuya_crypto primitive the OpenAPI sign depends on.
    std::string got;
    CheckEqStr(r, "sha256", "empty", kGoldenSha256Empty,
               tuya_crypto::Sha256Hex("", got) ? got : "<error logged above>");
    CheckEqStr(r, "sha256", "body_name_value", kGoldenSha256Body,
               tuya_crypto::Sha256Hex(kBodyNameValue, got) ? got : "<error logged above>");
    CheckEqStr(r, "sha256", "password", kGoldenSha256Password,
               tuya_crypto::Sha256Hex(kPassword, got) ? got : "<error logged above>");
}

void TestLoginBodies(TestResult& r) {
    // The SMART_HOME body golden-tests Md5Hex, the CUSTOM body Sha256Hex.
    CheckEqStr(r, "login_body", "smart_home", kGoldenLoginBodySmartHome,
               BuildOpenApiLoginBodySmartHome(kUsername, kPassword, kCountryCode, kSchema));
    CheckEqStr(r, "login_body", "custom", kGoldenLoginBodyCustom,
               BuildOpenApiLoginBodyCustom(kUsername, kPassword));
}

void TestSigns(TestResult& r) {
    // Case A: GET, empty body, no params.
    CheckEqStr(
        r, "sign_case_a", "get_empty_body", kGoldenSignA,
        BuildOpenApiSign(kClientId, kAccessToken, kAccessSecret, kT_A, "GET", "", kPathDevices));
    // Case B: POST with a compact JSON body — the exact-string-signed rule.
    CheckEqStr(r, "sign_case_b", "post_body", kGoldenSignB,
               BuildOpenApiSign(kClientId, kAccessToken, kAccessSecret, kT_B, "POST",
                                kBodyNameValue, kPathLoginSmartHome));
    // Case C: GET with params — the sign path uses SORTED, UNENCODED pairs.
    CheckEqStr(r, "sign_case_c", "get_sorted_params", kGoldenSignC,
               BuildOpenApiSign(kClientId, kAccessToken, kAccessSecret, kT_C, "GET", "",
                                "/v1.0/users/abc123/devices?foo=bar&from=sharing"));
    // Case D: login — EMPTY access_token excluded from the message.
    CheckEqStr(r, "sign_case_d", "login_no_token", kGoldenSignD,
               BuildOpenApiSign(kClientId, "", kAccessSecret, kT_D, "POST",
                                kGoldenLoginBodySmartHome, kPathLoginSmartHome));
    // Empty secret: the HMAC primitive rejects zero-bit keys -> "".
    CheckEqStr(r, "sign_empty_secret", "rejected", "",
               BuildOpenApiSign(kClientId, kAccessToken, "", kT_A, "GET", "", kPathDevices));
}

void TestSignPathAndUrl(TestResult& r) {
    const OpenApiParams no_params;
    // Unsorted input on purpose: the sign path must come out sorted.
    const OpenApiParams params = {{"from", "sharing"}, {"foo", "bar"}};
    // Values that stay raw in the sign path but are percent-encoded in the
    // URL (space, '+').
    const OpenApiParams raw_params = {{"foo", "bar b+az"}, {"from", "sharing"}};
    const OpenApiParams comma_param = {{"dev_ids", "id1,id2"}};

    CheckEqStr(r, "sign_path", "no_params", kPathDevices,
               BuildOpenApiSignPath(kPathDevices, no_params));
    CheckEqStr(r, "sign_path", "sorted", "/v1.0/users/abc123/devices?foo=bar&from=sharing",
               BuildOpenApiSignPath(kPathDevices, params));
    CheckEqStr(r, "sign_path", "raw_values", "/v1.0/users/abc123/devices?foo=bar b+az&from=sharing",
               BuildOpenApiSignPath(kPathDevices, raw_params));

    CheckEqStr(r, "url", "no_params", "https://openapi.tuyaus.com/v1.0/users/abc123/devices",
               BuildOpenApiUrl("https://openapi.tuyaus.com", kPathDevices, no_params));
    CheckEqStr(r, "url", "percent_encoded",
               "https://openapi.tuyaus.com/v1.0/users/abc123/devices?foo=bar%20b%2Baz&from=sharing",
               BuildOpenApiUrl("https://openapi.tuyaus.com", kPathDevices, raw_params));
    CheckEqStr(r, "url", "comma", "https://openapi.tuyaus.com/t?dev_ids=id1%2Cid2",
               BuildOpenApiUrl("https://openapi.tuyaus.com", "/t", comma_param));
}

void TestTemplate(TestResult& r) {
    CheckEqStr(r, "template", "uid", "/v1.0/users/ay1538982340877gXKm/devices",
               ResolveOpenApiTemplate("/v1.0/users/{uid}/devices", "ay1538982340877gXKm"));
    // {id} placeholders are caller-resolved; the client must leave them
    // alone.
    CheckEqStr(r, "template", "id_untouched", "/v2.0/cloud/thing/{id}/model",
               ResolveOpenApiTemplate("/v2.0/cloud/thing/{id}/model", "u1"));
    CheckEqStr(r, "template", "no_placeholder", "/v1.0/token",
               ResolveOpenApiTemplate("/v1.0/token", "u1"));
    // Home/room placeholders are resolved from the caller's ids.
    CheckEqStr(r, "template", "home_rooms", "/v1.0/homes/263448032/rooms",
               ResolveOpenApiTemplate(std::string(kOpenApiHomeRooms), "u1", "263448032"));
    CheckEqStr(
        r, "template", "room_devices", "/v1.0/homes/263448032/rooms/178870394/devices",
        ResolveOpenApiTemplate(std::string(kOpenApiRoomDevices), "u1", "263448032", "178870394"));
    // Every occurrence is replaced (no partial template survives).
    CheckEqStr(r, "template", "repeated", "/h/1/r/2/r/2",
               ResolveOpenApiTemplate("/h/{home_id}/r/{room_id}/r/{room_id}", "u1", "1", "2"));
}

void TestTokenExpiry(TestResult& r) {
    // Expiry 1731841624000; the 5-minute margin boundary is 1731841324000.
    OpenApiToken tok;
    tok.access_token = "at";
    tok.uid = "uid";
    tok.t = 1731838024000;
    tok.expire_time = 3600;

    CheckTrue(r, "expiry.fresh", !OpenApiTokenNeedsRefresh(tok, 1731838024000));
    CheckTrue(r, "expiry.one_ms_before_margin", !OpenApiTokenNeedsRefresh(tok, 1731841323999));
    CheckTrue(r, "expiry.at_margin", OpenApiTokenNeedsRefresh(tok, 1731841324000));
    CheckTrue(r, "expiry.at_expiry", OpenApiTokenNeedsRefresh(tok, 1731841624000));

    OpenApiToken absent;
    CheckTrue(r, "expiry.absent_token", OpenApiTokenNeedsRefresh(absent, 1731838024000));
    OpenApiToken no_expire = tok;
    no_expire.expire_time = 0;
    CheckTrue(r, "expiry.zero_expire_time", OpenApiTokenNeedsRefresh(no_expire, 1731838024000));
}

}  // namespace

bool RunOpenApiSelfTest() {
    TestResult r;
    TestSha256Primitive(r);
    TestLoginBodies(r);
    TestSigns(r);
    TestSignPathAndUrl(r);
    TestTemplate(r);
    TestTokenExpiry(r);

    if (r.passed == r.total) {
        ESP_LOGI(TAG, "TUYA IOT SELFTEST PASS (%d checks)", r.total);
        return true;
    }
    ESP_LOGE(TAG, "TUYA IOT SELFTEST FAIL (%d/%d)", r.passed, r.total);
    return false;
}

}  // namespace tuya_cloud
