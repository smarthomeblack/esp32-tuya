#include "sys/wifi_provision.h"

#include <cJSON.h>
#include <esp_http_server.h>
#include <esp_log.h>
#include <esp_mac.h>
#include <esp_netif.h>
#include <esp_random.h>
#include <esp_system.h>
#include <esp_timer.h>
#include <esp_wifi.h>
#include <freertos/task.h>
#include <string.h>

#include "sys/settings.h"
#include "sys/wifi_station.h"
#include "tuya_crypto/json_guard.h"

// bootloader_random.h is not exported to this component; bootloader_support is
// linked anyway, so declare the two entropy helpers used by the AP passphrase.
extern "C" void bootloader_random_enable(void);
extern "C" void bootloader_random_disable(void);

// EMBED_TXTFILES symbol for the captive portal page.
extern const char wifi_portal_html_start[] asm("_binary_wifi_portal_html_start");
extern const char wifi_portal_html_end[] asm("_binary_wifi_portal_html_end");

static const char* TAG = "WifiProvision";

namespace sys {

namespace {

constexpr const char* kApIp = "192.168.4.1";
constexpr const char* kSsidPrefix = "TuyaHub";
constexpr uint8_t kApChannel = 1;
constexpr uint8_t kApMaxConnections = 4;
constexpr uint32_t kScanBudgetMs = 4000;  // total /api/wifi/scan time budget
constexpr size_t kMaxConnectBody = 512;
// wifi_sta_config_t.ssid is NUL-terminated → effective max is 31; WPA passphrase max 63.
constexpr size_t kMaxSsidLen = 31;
constexpr size_t kMinPasswordLen = 8;  // WPA2-PSK minimum
constexpr size_t kMaxPasswordLen = 63;
constexpr int kRebootDelayMs = 800;               // let the HTTP response reach the client
constexpr EventBits_t kExitBit = BIT0;            // never set — Start() blocks forever
constexpr int64_t kApRecoveryUs = 300 * 1000000;  // 5 min in AP, then reboot-retry STA

httpd_handle_t s_server = nullptr;  // lives for the whole provisioning session

// One-shot "ap_recovery" timer: its expiry reboots the hub, so it is never deleted.
esp_timer_handle_t s_ap_recovery_timer = nullptr;

// Random 8-char WPA2 passphrase for the provisioning AP, persisted in NVS
// ("wifi"/"ap_pass") so reboots and forced reprovisioning reuse it.
std::string LoadOrCreateApPassphrase() {
    constexpr size_t kPassLen = 8;
    constexpr char kAlphabet[] = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz";
    Settings settings("wifi", true);
    std::string pass = settings.GetString("ap_pass");
    if (pass.length() != kPassLen) {
        uint8_t rnd[kPassLen];
        // Bootloader RNG: the only validated entropy source before Wi-Fi starts.
        bootloader_random_enable();
        esp_fill_random(rnd, sizeof(rnd));
        bootloader_random_disable();
        pass.clear();
        for (size_t i = 0; i < kPassLen; ++i) {
            pass.push_back(kAlphabet[rnd[i] % (sizeof(kAlphabet) - 1)]);
        }
        settings.SetString("ap_pass", pass);
    }
    return pass;
}

// Respond first, then reboot from a detached task — never inside an httpd handler.
void ScheduleReboot() {
    BaseType_t ok = xTaskCreate(
        [](void*) {
            vTaskDelay(pdMS_TO_TICKS(kRebootDelayMs));
            ESP_LOGW(TAG, "Rebooting to apply new Wi-Fi credentials");
            esp_restart();
        },
        "hub_reboot", 3072, nullptr, 5, nullptr);
    if (ok != pdPASS) {
        ESP_LOGE(TAG, "Failed to create the reboot task — rebooting immediately");
        esp_restart();
    }
}

esp_err_t RespondJson(httpd_req_t* req, const char* body, const char* status) {
    httpd_resp_set_status(req, status);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, body, HTTPD_RESP_USE_STRLEN);
}

esp_err_t RespondPortalError(httpd_req_t* req, const char* error, httpd_err_code_t code) {
    const char* status = "400 Bad Request";
    switch (code) {
        case HTTPD_500_INTERNAL_SERVER_ERROR: status = "500 Internal Server Error"; break;
        case HTTPD_400_BAD_REQUEST: status = "400 Bad Request"; break;
        default: break;
    }
    cJSON* root = cJSON_CreateObject();
    if (root == nullptr) {
        httpd_resp_send_err(req, code, "Out of memory");
        return ESP_FAIL;
    }
    cJSON_AddBoolToObject(root, "success", false);
    cJSON_AddStringToObject(root, "error", error);
    char* body = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (body == nullptr) {
        httpd_resp_send_err(req, code, "Out of memory");
        return ESP_FAIL;
    }
    httpd_resp_set_status(req, status);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    esp_err_t err = httpd_resp_send(req, body, HTTPD_RESP_USE_STRLEN);
    cJSON_free(body);
    return err;
}

// The portal page; also serves "/" and every unmatched GET (captive-portal probes).
// While the initial key is un-rotated, its placeholder line is filled in.
esp_err_t HandlePortal(httpd_req_t* req) {
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_set_hdr(req, "Connection", "close");
    std::string initial_key;
    {
        Settings settings("hub");
        if (settings.GetBool("key_is_initial", false)) {
            initial_key = settings.GetString("initial_api_key", "");
        }
    }
    const size_t page_len = wifi_portal_html_end - wifi_portal_html_start;
    if (initial_key.empty()) {
        return httpd_resp_send(req, wifi_portal_html_start, page_len);
    }
    // The generated key is [A-Za-z0-9]{8} — safe to splice into the HTML.
    std::string page(wifi_portal_html_start, page_len);
    constexpr const char* kKeyToken = "__INITIAL_API_KEY__";
    constexpr const char* kHiddenToken = "\"apikey-box\" hidden";
    const size_t key_pos = page.find(kKeyToken);
    const size_t hidden_pos = page.find(kHiddenToken);
    if (key_pos == std::string::npos || hidden_pos == std::string::npos) {
        return httpd_resp_send(req, page.c_str(), page.size());
    }
    page.replace(key_pos, strlen(kKeyToken), initial_key);
    page.replace(hidden_pos, strlen(kHiddenToken), "\"apikey-box\"");
    return httpd_resp_send(req, page.c_str(), page.size());
}

// Blocking scan bounded by the budget; the handler stays well under 5s.
esp_err_t HandleScanApi(httpd_req_t* req) {
    std::vector<WifiApRecord> aps = WifiStation::GetInstance().Scan(kScanBudgetMs);
    if (aps.size() == 1 && !aps[0].valid) {
        return RespondPortalError(req, "scan_failed", HTTPD_500_INTERNAL_SERVER_ERROR);
    }

    cJSON* root = cJSON_CreateObject();
    if (root == nullptr) {
        return RespondPortalError(req, "out_of_memory", HTTPD_500_INTERNAL_SERVER_ERROR);
    }
    cJSON_AddBoolToObject(root, "success", true);
    cJSON* ap_list = cJSON_AddArrayToObject(root, "ap_list");
    if (ap_list == nullptr) {
        cJSON_Delete(root);
        return RespondPortalError(req, "out_of_memory", HTTPD_500_INTERNAL_SERVER_ERROR);
    }
    for (const WifiApRecord& ap : aps) {
        cJSON* item = cJSON_CreateObject();
        if (item == nullptr) {
            continue;  // skip this entry; the rest of the list stays usable
        }
        cJSON_AddStringToObject(item, "ssid", ap.ssid.c_str());
        cJSON_AddNumberToObject(item, "rssi", ap.rssi);
        cJSON_AddBoolToObject(item, "open", ap.open);
        cJSON_AddItemToArray(ap_list, item);
    }

    char* body = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (body == nullptr) {
        return RespondPortalError(req, "out_of_memory", HTTPD_500_INTERNAL_SERVER_ERROR);
    }
    esp_err_t err = RespondJson(req, body, "200 OK");
    cJSON_free(body);
    return err;
}

// Save credentials, respond, then reboot (see ScheduleReboot).
esp_err_t HandleConnectApi(httpd_req_t* req) {
    if (req->content_len > kMaxConnectBody) {
        return RespondPortalError(req, "payload_too_large", HTTPD_400_BAD_REQUEST);
    }
    // ret <= 0 = the peer closed mid-body; looping again would busy-spin.
    char body[kMaxConnectBody + 1] = {};
    size_t total = 0;
    while (total < req->content_len) {
        int ret = httpd_req_recv(req, body + total, req->content_len - total);
        if (ret == HTTPD_SOCK_ERR_TIMEOUT) {
            httpd_resp_send_408(req);
            return ESP_FAIL;
        }
        if (ret <= 0) {
            return RespondPortalError(req, "recv_failed", HTTPD_400_BAD_REQUEST);
        }
        total += static_cast<size_t>(ret);
    }
    body[total] = '\0';

    if (!tuya_crypto::JsonNestingWithinLimit(body, total, tuya_crypto::kMaxJsonDepth)) {
        return RespondPortalError(req, "invalid_json", HTTPD_400_BAD_REQUEST);
    }
    cJSON* json = cJSON_Parse(body);
    if (json == nullptr) {
        return RespondPortalError(req, "invalid_json", HTTPD_400_BAD_REQUEST);
    }
    const cJSON* ssid_item = cJSON_GetObjectItemCaseSensitive(json, "ssid");
    const cJSON* password_item = cJSON_GetObjectItemCaseSensitive(json, "password");

    std::string ssid;
    std::string password;
    bool valid = cJSON_IsString(ssid_item) && ssid_item->valuestring != nullptr;
    if (valid) {
        ssid = ssid_item->valuestring;
        valid = !ssid.empty() && ssid.length() <= kMaxSsidLen;
    }
    const char* error = "invalid_ssid_or_password";
    if (valid) {
        if (password_item == nullptr || cJSON_IsNull(password_item)) {
            password.clear();  // open network
        } else if (cJSON_IsString(password_item) && password_item->valuestring != nullptr) {
            password = password_item->valuestring;
            // The portal JS always sends a string; "" means open network.
            valid = password.empty() ||
                    (password.length() >= kMinPasswordLen && password.length() <= kMaxPasswordLen);
            if (!valid) {
                error = "password must be 8-63 characters";
            }
        } else {
            valid = false;
        }
    }
    if (!valid) {
        cJSON_Delete(json);
        return RespondPortalError(req, error, HTTPD_400_BAD_REQUEST);
    }

    // Persist credentials (RAII commit).
    {
        Settings settings("wifi", true);
        settings.SetString("ssid", ssid);
        settings.SetString("password", password);
        settings.SetBool("force_ap", false);
    }
    cJSON_Delete(json);

    // Writes are fail-soft; read back all three — a partial write must not look like success.
    {
        Settings check("wifi", false);
        if (check.GetString("ssid") != ssid || check.GetString("password") != password ||
            check.GetBool("force_ap", true) != false) {
            ESP_LOGE(TAG, "Credentials not saved — read-back mismatch");
            return RespondPortalError(req, "storage write failed — credentials not saved",
                                      HTTPD_500_INTERNAL_SERVER_ERROR);
        }
    }

    ESP_LOGI(TAG, "Received credentials for '%s' (password %u bytes) — rebooting", ssid.c_str(),
             static_cast<unsigned>(password.length()));
    esp_err_t err = RespondJson(req, "{\"success\":true}", "200 OK");
    ScheduleReboot();
    return err;
}

esp_err_t HandleRebootApi(httpd_req_t* req) {
    ESP_LOGI(TAG, "Reboot requested via /api/reboot");
    esp_err_t err = RespondJson(req, "{\"success\":true}", "200 OK");
    ScheduleReboot();
    return err;
}

}  // namespace

WifiProvision::WifiProvision() {
    event_group_ = xEventGroupCreate();
    if (event_group_ == nullptr) {
        // Cannot happen under normal operation (OOM at boot) — unrecoverable.
        ESP_LOGE(TAG, "Failed to create event group");
        abort();
    }
}

void WifiProvision::Start() {
    bool expected = false;
    if (!started_.compare_exchange_strong(expected, true)) {
        ESP_LOGW(TAG, "Start() called more than once — ignoring");
        return;
    }

    // esp_netif_init() is safe when already initialized (e.g. after a failed WifiStation::Start()).
    esp_err_t err = esp_netif_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_netif_init: %s", esp_err_to_name(err));
    }

    StartAccessPoint();
    StartWebServer();

    ESP_LOGI(TAG, "WiFi provisioning AP '%s' — connect and open http://%s", ap_ssid_.c_str(),
             kApIp);

    // Stored credentials + router was down at boot → reboot retries STA;
    // a fresh setup (empty ssid or force_ap) stays up for the user.
    {
        Settings settings("wifi");
        const std::string stored_ssid = settings.GetString("ssid");
        const bool force_ap = settings.GetBool("force_ap", false);
        if (!stored_ssid.empty() && !force_ap) {
            const esp_timer_create_args_t timer_args = {
                .callback = [](void*) { esp_restart(); },
                .arg = nullptr,
                .dispatch_method = ESP_TIMER_TASK,
                .name = "ap_recovery",
                .skip_unhandled_events = true,
            };
            err = esp_timer_create(&timer_args, &s_ap_recovery_timer);
            if (err == ESP_OK) {
                err = esp_timer_start_once(s_ap_recovery_timer, kApRecoveryUs);
            }
            if (err != ESP_OK) {
                ESP_LOGW(TAG, "ap_recovery timer: %s — AP stays up", esp_err_to_name(err));
            }
        }
    }

    // Block forever: the only exit is a reboot triggered by the portal.
    xEventGroupWaitBits(event_group_, kExitBit, pdFALSE, pdFALSE, portMAX_DELAY);
}

void WifiProvision::StartAccessPoint() {
    ap_netif_ = esp_netif_create_default_wifi_ap();  // aborts internally on failure

    // Static AP address 192.168.4.1/24.
    esp_netif_ip_info_t ip_info = {};
    esp_netif_set_ip4_addr(&ip_info.ip, 192, 168, 4, 1);
    esp_netif_set_ip4_addr(&ip_info.gw, 192, 168, 4, 1);
    esp_netif_set_ip4_addr(&ip_info.netmask, 255, 255, 255, 0);
    esp_err_t err = esp_netif_dhcps_stop(ap_netif_);
    if (err != ESP_OK && err != ESP_ERR_ESP_NETIF_DHCP_ALREADY_STOPPED) {
        ESP_LOGW(TAG, "esp_netif_dhcps_stop: %s", esp_err_to_name(err));
    }
    err = esp_netif_set_ip_info(ap_netif_, &ip_info);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_netif_set_ip_info: %s", esp_err_to_name(err));
    }
    err = esp_netif_dhcps_start(ap_netif_);
    if (err != ESP_OK && err != ESP_ERR_ESP_NETIF_DHCP_ALREADY_STARTED) {
        ESP_LOGE(TAG, "esp_netif_dhcps_start: %s", esp_err_to_name(err));
    }

    wifi_init_config_t init_cfg = WIFI_INIT_CONFIG_DEFAULT();
    err = esp_wifi_init(&init_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_init: %s — the AP cannot come up", esp_err_to_name(err));
        return;
    }

    // Log phones joining/leaving.
    err = esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                              &WifiProvision::WifiEventHandler, this,
                                              &wifi_event_instance_);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "register WIFI_EVENT handler: %s", esp_err_to_name(err));
    }

    // SSID "TuyaHub-XXXX", XXXX = last 4 hex digits of the base MAC.
    uint8_t mac[6] = {0};
    err = esp_read_mac(mac, ESP_MAC_BASE);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "esp_read_mac: %s", esp_err_to_name(err));
    }
    char ssid[16];
    snprintf(ssid, sizeof(ssid), "%s-%04X", kSsidPrefix,
             static_cast<unsigned>((mac[4] << 8) | mac[5]));
    ap_ssid_ = ssid;

    wifi_config_t wifi_config = {};
    strlcpy(reinterpret_cast<char*>(wifi_config.ap.ssid), ssid, sizeof(wifi_config.ap.ssid));
    wifi_config.ap.ssid_len = static_cast<uint8_t>(strlen(ssid));
    wifi_config.ap.channel = kApChannel;
    wifi_config.ap.max_connection = kApMaxConnections;
    const std::string ap_pass = LoadOrCreateApPassphrase();
    wifi_config.ap.authmode = WIFI_AUTH_WPA2_PSK;
    strlcpy(reinterpret_cast<char*>(wifi_config.ap.password), ap_pass.c_str(),
            sizeof(wifi_config.ap.password));

    // APSTA: the STA side is what /api/wifi/scan scans with.
    err = esp_wifi_set_mode(WIFI_MODE_APSTA);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_set_mode(APSTA): %s", esp_err_to_name(err));
        return;
    }
    err = esp_wifi_set_config(WIFI_IF_AP, &wifi_config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_set_config: %s", esp_err_to_name(err));
        return;
    }
    err = esp_wifi_set_ps(WIFI_PS_NONE);  // snappier captive portal
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "esp_wifi_set_ps: %s", esp_err_to_name(err));
    }
    err = esp_wifi_start();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_start: %s", esp_err_to_name(err));
        return;
    }
    ESP_LOGI(TAG, "Access point up: ssid=%s pass=%s ip=%s", ssid, ap_pass.c_str(), kApIp);

    // Hijack all DNS lookups to the AP address.
    esp_ip4_addr_t dns_answer = {};
    esp_netif_set_ip4_addr(&dns_answer, 192, 168, 4, 1);
    if (!dns_server_.Start(dns_answer)) {
        ESP_LOGE(TAG, "DNS server failed to start — captive-portal probes may fail");
    }
}

void WifiProvision::StartWebServer() {
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = 80;
    config.stack_size = 8192;
    config.max_uri_handlers = 8;
    config.uri_match_fn = httpd_uri_match_wildcard;

    esp_err_t err = httpd_start(&s_server, &config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "httpd_start: %s — provisioning portal unavailable", esp_err_to_name(err));
        return;
    }

    // The wildcard catch-all must register LAST or httpd rejects the
    // specific URIs. Plain assignments: designated initializers trip -Wextra.
    httpd_uri_t scan_api = {};
    scan_api.uri = "/api/wifi/scan";
    scan_api.method = HTTP_GET;
    scan_api.handler = &HandleScanApi;
    scan_api.user_ctx = nullptr;

    httpd_uri_t connect_api = {};
    connect_api.uri = "/api/wifi/connect";
    connect_api.method = HTTP_POST;
    connect_api.handler = &HandleConnectApi;
    connect_api.user_ctx = nullptr;

    httpd_uri_t reboot_api = {};
    reboot_api.uri = "/api/reboot";
    reboot_api.method = HTTP_POST;
    reboot_api.handler = &HandleRebootApi;
    reboot_api.user_ctx = nullptr;

    // Captive-portal probes (and "/") all land here.
    httpd_uri_t portal = {};
    portal.uri = "/*";
    portal.method = HTTP_GET;
    portal.handler = &HandlePortal;
    portal.user_ctx = nullptr;

    err = httpd_register_uri_handler(s_server, &scan_api);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "register /api/wifi/scan: %s", esp_err_to_name(err));
    }
    err = httpd_register_uri_handler(s_server, &connect_api);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "register /api/wifi/connect: %s", esp_err_to_name(err));
    }
    err = httpd_register_uri_handler(s_server, &reboot_api);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "register /api/reboot: %s", esp_err_to_name(err));
    }
    err = httpd_register_uri_handler(s_server, &portal);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "register portal wildcard: %s", esp_err_to_name(err));
    }
}

void WifiProvision::WifiEventHandler(void* arg, esp_event_base_t event_base, int32_t event_id,
                                     void* event_data) {
    (void)arg;
    (void)event_base;  // registered for WIFI_EVENT only
    if (event_id == WIFI_EVENT_AP_STACONNECTED) {
        auto* event = static_cast<wifi_event_ap_staconnected_t*>(event_data);
        ESP_LOGI(TAG, "Client " MACSTR " joined (aid=%d)", MAC2STR(event->mac),
                 static_cast<int>(event->aid));
    } else if (event_id == WIFI_EVENT_AP_STADISCONNECTED) {
        auto* event = static_cast<wifi_event_ap_stadisconnected_t*>(event_data);
        ESP_LOGI(TAG, "Client " MACSTR " left (aid=%d)", MAC2STR(event->mac),
                 static_cast<int>(event->aid));
    }
}

}  // namespace sys
