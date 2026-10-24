/**
 * @brief Bearer API-key auth: SHA-256 key storage + /api and /ws gates.
 */

#include "web_server/api_auth.h"

#include <bootloader_random.h>
#include <esp_log.h>
#include <esp_random.h>
#include <esp_timer.h>

#include <cstdint>
#include <cstring>
#include <mutex>
#include <string>
#include <strings.h>

#include <lwip/sockets.h>

#include "sys/settings.h"
#include "tuya_crypto/sha256.h"

static const char* TAG = "ApiAuth";

namespace web_server {
namespace rest_api {

namespace {

constexpr size_t kMinApiKeyLen = 8;
constexpr size_t kMaxApiKeyLen = 64;
/// Random initial key: 8 chars over [A-Za-z0-9] (the boot log + setup
/// portal display format).
constexpr size_t kInitialKeyLen = 8;
constexpr char kKeyAlphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789";
/// SHA-256 hex digest length (the NVS storage format).
constexpr size_t kShaHexLen = 64;
/// Authorization header buffer (scheme + key + slack).
constexpr size_t kAuthHdrLen = 128;
/// Per-IP lockout: more than kMaxAuthFails failed checks within 60 s buys
/// 300 s of instant 401s; a success resets the window.
constexpr uint32_t kMaxAuthFails = 10;
constexpr int64_t kFailWindowMs = 60000;
constexpr int64_t kLockoutMs = 300000;
/// Per-IP slots; eviction prefers an unlocked slot, so live bans survive a spray.
constexpr size_t kFailSlots = 8;

int HexDigit(char c) {
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}

// Cached NVS value — the hash changes only through ApiKeyStore or
// InitApiKey (factory reset erases it, then reboots). Mutex: httpd + WS
// handshake may race.
std::mutex hash_cache_mutex;
std::string hash_cache;
bool hash_cache_loaded = false;

std::string StoredHash() {
    std::lock_guard<std::mutex> lock(hash_cache_mutex);
    if (!hash_cache_loaded) {
        sys::Settings settings("hub");
        hash_cache = settings.GetString("api_key_sha", "");
        hash_cache_loaded = true;
    }
    return hash_cache;
}

// Brute-force counter shared by the REST + WS gates (httpd tasks and the
// WS pre-handshake run concurrently — same mutex convention as the cache).
struct FailSlot {
    uint32_t ip = 0;              // network byte order; 0 = unknown peer
    uint32_t fails = 0;           // failures since window_ms
    int64_t window_ms = 0;        // window start, 0 = reset
    int64_t locked_until_ms = 0;  // 0 = not locked
};
std::mutex fail_mutex;
FailSlot fail_slots[kFailSlots];

int64_t NowMs() {
    return esp_timer_get_time() / 1000;
}

/// Peer IPv4 (network order); 0 when unavailable (shared fallback slot).
uint32_t PeerIp(httpd_req_t* req) {
    struct sockaddr_in peer;
    socklen_t len = sizeof(peer);
    memset(&peer, 0, sizeof(peer));
    if (getpeername(httpd_req_to_sockfd(req), reinterpret_cast<struct sockaddr*>(&peer), &len) !=
            0 ||
        peer.sin_family != AF_INET) {
        return 0;
    }
    return peer.sin_addr.s_addr;
}

/// "a.b.c.d" for logs; thread-safe reentrant form.
std::string IpText(uint32_t ip) {
    struct in_addr addr;
    addr.s_addr = ip;
    char buf[INET_ADDRSTRLEN];
    inet_ntoa_r(addr, buf, sizeof(buf));
    return std::string(buf);
}

/// The slot for `ip`; a full table evicts an unlocked slot (stalest window)
/// before spilling a live lockout (the one expiring soonest).
FailSlot* SlotFor(uint32_t ip, int64_t now) {
    for (FailSlot& s : fail_slots) {
        if (s.ip == ip)
            return &s;
    }
    FailSlot* unlocked = nullptr;
    FailSlot* banned = nullptr;
    for (FailSlot& s : fail_slots) {
        if (s.locked_until_ms > now) {
            if (banned == nullptr || s.locked_until_ms < banned->locked_until_ms) {
                banned = &s;
            }
        } else if (unlocked == nullptr || s.window_ms < unlocked->window_ms) {
            unlocked = &s;
        }
    }
    FailSlot* victim = unlocked != nullptr ? unlocked : banned;
    *victim = FailSlot{};
    victim->ip = ip;
    return victim;
}

/// Constant-time equality for fixed-shape lowercase hex digests; the
/// length mismatch also folds into the same accumulate loop.
bool HexEqualConstTime(const std::string& a, const std::string& b) {
    uint8_t diff = static_cast<uint8_t>(a.size() ^ b.size());
    const size_t len = a.size() < b.size() ? a.size() : b.size();
    for (size_t i = 0; i < len; ++i) {
        diff |= static_cast<uint8_t>(a[i]) ^ static_cast<uint8_t>(b[i]);
    }
    return diff == 0;
}

/// Random 8-char [A-Za-z0-9] key; rejection-sampled so every alphabet
/// char stays equally likely (256 % 62 != 0).
std::string GenerateInitialKey() {
    constexpr size_t kAlphabetSize = sizeof(kKeyAlphabet) - 1;
    constexpr uint8_t kAcceptLimit = 256 - (256 % kAlphabetSize);
    std::string key;
    key.reserve(kInitialKeyLen);
    while (key.size() < kInitialKeyLen) {
        uint8_t rnd[kInitialKeyLen * 2];
        // Bootloader RNG: the only validated entropy source before Wi-Fi/BT start.
        bootloader_random_enable();
        esp_fill_random(rnd, sizeof(rnd));
        bootloader_random_disable();
        for (size_t i = 0; i < sizeof(rnd) && key.size() < kInitialKeyLen; ++i) {
            if (rnd[i] < kAcceptLimit) {
                key.push_back(kKeyAlphabet[rnd[i] % kAlphabetSize]);
            }
        }
    }
    return key;
}

bool KeyIsPrintable(const char* key, size_t len) {
    for (size_t i = 0; i < len; ++i) {
        const unsigned char c = static_cast<unsigned char>(key[i]);
        if (c < 0x20 || c > 0x7E) {
            return false;
        }
    }
    return true;
}

/// Parses "Authorization: Bearer <key>" (case-insensitive scheme, extra
/// spaces OK); NUL-terminated key out, ESP_FAIL when absent/malformed.
esp_err_t ParseBearer(httpd_req_t* req, char* key, size_t key_size) {
    char header[kAuthHdrLen] = {0};
    const esp_err_t err = httpd_req_get_hdr_value_str(req, "Authorization", header, sizeof(header));
    if (err != ESP_OK) {
        return ESP_FAIL;
    }
    const char* p = header;
    while (*p == ' ' || *p == '\t') {
        ++p;
    }
    constexpr char kScheme[] = "Bearer";
    constexpr size_t kSchemeLen = sizeof(kScheme) - 1;
    if (strncasecmp(p, kScheme, kSchemeLen) != 0) {
        return ESP_FAIL;
    }
    p += kSchemeLen;
    if (*p != '\0' && *p != ' ' && *p != '\t') {
        return ESP_FAIL;  // "BearerX..." is not the Bearer scheme
    }
    while (*p == ' ' || *p == '\t') {
        ++p;
    }
    size_t len = strlen(p);
    while (len > 0 && (p[len - 1] == ' ' || p[len - 1] == '\t')) {
        --len;
    }
    if (len == 0 || len >= key_size) {
        return ESP_FAIL;
    }
    memcpy(key, p, len);
    key[len] = '\0';
    return ESP_OK;
}

/// The 401 body (flat shape; never echoes the key).
constexpr char kUnauthorizedBody[] =
    "{\"success\":false,\"error\":\"unauthorized\",\"message\":\"Invalid API key\"}";

/// Sends the flat 401; always ESP_FAIL so the caller's handler body never
/// runs (a successful 401 send itself returns ESP_OK — auth bypass).
esp_err_t SendUnauthorized(httpd_req_t* req) {
    httpd_resp_set_status(req, "401 Unauthorized");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "WWW-Authenticate", "Bearer");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_send(req, kUnauthorizedBody, HTTPD_RESP_USE_STRLEN);
    return ESP_FAIL;
}

}  // namespace

bool ApiKeyMatches(const char* key, size_t len) {
    if (key == nullptr || len == 0 || len > kMaxApiKeyLen) {
        return false;
    }
    std::string hex;
    if (!tuya_crypto::Sha256Hex(std::string(key, len), hex)) {
        return false;
    }
    const std::string stored = StoredHash();
    return stored.size() == kShaHexLen && HexEqualConstTime(hex, stored);
}

bool ApiKeyIsInitial() {
    sys::Settings settings("hub");
    return settings.GetBool("key_is_initial", false);
}

void InitApiKey() {
    std::string key;
    bool generate = false;
    {
        sys::Settings settings("hub");
        const std::string stored = settings.GetString("api_key_sha", "");
        const bool initial = settings.GetBool("key_is_initial", false);
        key = settings.GetString("initial_api_key", "");
        // Absent hash = first boot (or factory reset); a set flag with a
        // lost plaintext cannot be displayed — regenerate in both cases.
        generate = stored.size() != kShaHexLen || (initial && key.empty());
        if (!generate && !initial) {
            return;  // rotated key — nothing to print or store
        }
    }
    if (generate) {
        key = GenerateInitialKey();
        std::string hex;
        if (!tuya_crypto::Sha256Hex(key, hex)) {
            ESP_LOGE(TAG, "Initial key hashing failed — auth unavailable this boot");
            return;
        }
        {
            sys::Settings settings("hub", true);
            settings.SetString("api_key_sha", hex);
            settings.SetBool("key_is_initial", true);
            settings.SetString("initial_api_key", key);
        }
        {
            std::lock_guard<std::mutex> lock(hash_cache_mutex);
            hash_cache.clear();
            hash_cache_loaded = false;
        }
        if (StoredHash() != hex) {
            ESP_LOGE(TAG, "Initial key NVS write failed — auth unavailable this boot");
            return;
        }
        // Generation boot only: a loaded-but-unrotated key is never re-printed.
        ESP_LOGI(TAG, "Initial API key: %s — change it after first login", key.c_str());
    }
}

bool ApiKeyStore(const char* key, size_t len) {
    if (key == nullptr || len < kMinApiKeyLen || len > kMaxApiKeyLen || !KeyIsPrintable(key, len)) {
        return false;
    }
    // Leading/trailing whitespace would be trimmed by ParseBearer but not
    // by the WS gate — reject so every stored key round-trips both paths.
    if (key[0] == ' ' || key[0] == '\t' || key[len - 1] == ' ' || key[len - 1] == '\t') {
        return false;
    }
    std::string hex;
    if (!tuya_crypto::Sha256Hex(std::string(key, len), hex)) {
        return false;
    }
    {
        sys::Settings settings("hub", true);
        settings.SetString("api_key_sha", hex);
    }
    // Drop the cache BEFORE the verify re-read, so it reflects NVS (a
    // failed write re-caches the still-live old value).
    {
        std::lock_guard<std::mutex> lock(hash_cache_mutex);
        hash_cache.clear();
        hash_cache_loaded = false;
    }
    if (StoredHash() != hex) {
        return false;  // NVS write failed (full/errored) — key unchanged
    }
    // Rotation closes the initial-key window; a failed erase only leaves
    // a stale display flag, never a working credential.
    {
        sys::Settings settings("hub", true);
        settings.EraseKey("initial_api_key");
        settings.EraseKey("key_is_initial");
    }
    ESP_LOGI(TAG, "API key stored (hash only)");
    return true;
}

bool AuthLockedOut(httpd_req_t* req) {
    const uint32_t ip = PeerIp(req);
    const int64_t now = NowMs();
    std::lock_guard<std::mutex> lock(fail_mutex);
    for (FailSlot& s : fail_slots) {
        if (s.ip != ip || s.locked_until_ms == 0) {
            continue;
        }
        if (now < s.locked_until_ms) {
            return true;
        }
        s.locked_until_ms = 0;
        s.fails = 0;
        s.window_ms = 0;
        ESP_LOGW(TAG, "Auth lockout expired for %s", IpText(ip).c_str());
        return false;
    }
    return false;
}

void RecordAuthFailure(httpd_req_t* req) {
    const uint32_t ip = PeerIp(req);
    const int64_t now = NowMs();
    std::lock_guard<std::mutex> lock(fail_mutex);
    FailSlot* s = SlotFor(ip, now);
    if (s->fails == 0 || now - s->window_ms >= kFailWindowMs) {
        s->window_ms = now;
        s->fails = 0;
    }
    ++s->fails;
    if (s->fails > kMaxAuthFails && s->locked_until_ms == 0) {
        s->locked_until_ms = now + kLockoutMs;
        ESP_LOGW(TAG, "Auth lockout engaged for %s (%u failures in %lld s)", IpText(ip).c_str(),
                 s->fails, static_cast<long long>(kFailWindowMs / 1000));
    }
}

void RecordAuthSuccess(httpd_req_t* req) {
    const uint32_t ip = PeerIp(req);
    std::lock_guard<std::mutex> lock(fail_mutex);
    for (FailSlot& s : fail_slots) {
        if (s.ip == ip && s.fails != 0) {
            s.fails = 0;
            s.window_ms = 0;
        }
    }
}

esp_err_t RequireAuth(httpd_req_t* req) {
    if (AuthLockedOut(req)) {
        return SendUnauthorized(req);  // locked: fail fast, no key check
    }
    char key[kMaxApiKeyLen + 1] = {0};
    if (ParseBearer(req, key, sizeof(key)) == ESP_OK && ApiKeyMatches(key, strlen(key))) {
        RecordAuthSuccess(req);
        return ESP_OK;
    }
    RecordAuthFailure(req);
    ESP_LOGW(TAG, "API auth failed: %s", req->uri);
    return SendUnauthorized(req);
}

bool WsTokenOk(httpd_req_t* req) {
    // Fits "token=" + a fully %XX-escaped 64-char key + slack.
    char query[kMaxApiKeyLen * 3 + 8] = {0};
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK) {
        return false;
    }
    char token[kMaxApiKeyLen * 3 + 1] = {0};
    if (httpd_query_key_value(query, "token", token, sizeof(token)) != ESP_OK) {
        return false;
    }
    // Percent-decode (encodeURIComponent output): "%XX" only; '+' stays
    // literal (encodeURIComponent never encodes a space as '+').
    char decoded[kMaxApiKeyLen + 1] = {0};
    size_t out = 0;
    bool overflow = false;
    for (size_t i = 0; token[i] != '\0'; ++i) {
        char c = token[i];
        if (c == '%' && HexDigit(token[i + 1]) >= 0 && HexDigit(token[i + 2]) >= 0) {
            c = static_cast<char>((HexDigit(token[i + 1]) << 4) | HexDigit(token[i + 2]));
            i += 2;
        }
        if (out < sizeof(decoded) - 1) {
            decoded[out++] = c;
        } else {
            overflow = true;  // longer than any legal key — reject, never clip
        }
    }
    if (overflow || out == 0) {
        return false;
    }
    return ApiKeyMatches(decoded, out);
}

}  // namespace rest_api
}  // namespace web_server
