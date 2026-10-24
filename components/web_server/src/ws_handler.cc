// /ws endpoint: session tracking, heartbeats, broadcast. IDF v6 completes
// the handshake pre-handler, so "connect" must be the first client frame.

#include "ws_handler.h"

#include <esp_log.h>
#include <esp_timer.h>

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "rest_api_internal.h"
#include "web_server/api_auth.h"

static const char* TAG = "WsHandler";

namespace web_server {
namespace ws {

namespace {

static_assert(web_server::ws::kMaxOpenSockets < CONFIG_LWIP_UDP_RECVMBOX_SIZE,
              "EvictAllSessions queues one ctrl message per socket; the ctrl-msg semaphore must "
              "exceed max_open_sockets (10 < 16)");

/// Clients only send heartbeats; larger frames are dropped.
constexpr size_t kMaxClientFrame = 256;
/// 4x the 15 s client heartbeat (web_assets/src/js/core/ws.js); fds silent
/// this long are dead — fd reuse is gated per-send by the fd-info check.
constexpr int64_t kClientStaleMs = 60000;

/// A tracked WS socket; last_seen_ms refreshes on every inbound frame.
struct Client {
    int fd;
    int64_t last_seen_ms;
};

std::map<int, Client> g_clients;
std::mutex g_clients_mutex;
/// Fds PruneStale dropped this round, handed to the off-task closer.
std::vector<int> g_pruned_fds;

/// Monotonic milliseconds (uptime) for the liveness timestamps.
int64_t NowMs() {
    return esp_timer_get_time() / 1000;
}

/// One queued broadcast (owned by the work item).
struct BroadcastCtx {
    httpd_handle_t server = nullptr;
    std::string frame;
};

void AddClient(int fd) {
    std::lock_guard<std::mutex> lock(g_clients_mutex);
    if (g_clients.find(fd) != g_clients.end()) {
        return;  // already tracked — TouchClient refreshes its liveness
    }
    g_clients[fd] = Client{fd, NowMs()};
    ESP_LOGI(TAG, "WS client connected (fd=%d, total=%u)", fd,
             static_cast<unsigned>(g_clients.size()));
}

/// Refreshes liveness; an untracked fd stays untracked.
void TouchClient(int fd) {
    std::lock_guard<std::mutex> lock(g_clients_mutex);
    auto it = g_clients.find(fd);
    if (it != g_clients.end()) {
        it->second.last_seen_ms = NowMs();
    }
}

void RemoveClient(int fd) {
    std::lock_guard<std::mutex> lock(g_clients_mutex);
    if (g_clients.erase(fd) > 0) {
        ESP_LOGI(TAG, "WS client removed (fd=%d, total=%u)", fd,
                 static_cast<unsigned>(g_clients.size()));
    }
}

/// Drops entries silent past kClientStaleMs; the caller holds the mutex.
/// A dropped fd may still hold its httpd session slot — see CloseStaleSessions.
void PruneStale(int64_t now) {
    for (auto it = g_clients.begin(); it != g_clients.end();) {
        if (now - it->second.last_seen_ms <= kClientStaleMs) {
            ++it;
            continue;
        }
        ESP_LOGW(TAG, "WS client stale (fd=%d) — untracking", it->first);
        g_pruned_fds.push_back(it->first);
        it = g_clients.erase(it);
    }
}

/// True when PruneStale has dropped fds no closer has drained yet.
bool HasPrunedFds() {
    std::lock_guard<std::mutex> lock(g_clients_mutex);
    return !g_pruned_fds.empty();
}

/// Snapshot of the live fds (stale entries pruned first).
std::vector<int> LiveFdsLocked(int64_t now) {
    PruneStale(now);
    std::vector<int> fds;
    fds.reserve(g_clients.size());
    for (const auto& entry : g_clients) {
        fds.push_back(entry.first);
    }
    return fds;
}

/// Every open session fd from httpd (tracked WS + in-flight plain HTTP).
std::vector<int> AllSessionFds(httpd_handle_t server) {
    std::vector<int> fds;
    if (server == nullptr) {
        return fds;
    }
    // No size-query mode in this API: INVALID_ARG also fires when the
    // session list outgrows the buffer, so double the capacity and retry.
    size_t cap = 8;
    for (;;) {
        int* raw = static_cast<int*>(malloc(cap * sizeof(int)));
        if (raw == nullptr) {
            break;
        }
        size_t count = cap;
        const esp_err_t err = httpd_get_client_list(server, &count, raw);
        if (err == ESP_OK) {
            fds.assign(raw, raw + count);
            free(raw);
            break;
        }
        free(raw);
        if (err != ESP_ERR_INVALID_ARG) {
            break;
        }
        cap *= 2;
    }
    return fds;
}

/// Answers one WS frame: PING echo, app-level heartbeats, CLOSE
/// bookkeeping; always consumes the frame.
esp_err_t HandleWebSocket(httpd_req_t* req) {
    const int sockfd = httpd_req_to_sockfd(req);

    // Length-first recv BEFORE any fd-info guard: recv's INVALID_STATE =
    // plain HTTP GET (no upgrade).
    httpd_ws_frame_t pkt = {};
    pkt.type = HTTPD_WS_TYPE_TEXT;
    esp_err_t ret = httpd_ws_recv_frame(req, &pkt, 0);
    if (ret == ESP_ERR_INVALID_STATE) {
        // Plain HTTP GET /ws: the bytes stay unread, so returning ESP_OK makes
        // httpd re-run this handler on the same session forever. Fail = close.
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "WebSocket upgrade required");
        RemoveClient(sockfd);
        return ESP_FAIL;
    }
    if (ret != ESP_OK) {
        RemoveClient(sockfd);
        return ret;
    }
    if (pkt.len > kMaxClientFrame) {
        ESP_LOGW(TAG, "WS frame too large (%u bytes) — dropping client fd=%d",
                 static_cast<unsigned>(pkt.len), sockfd);
        RemoveClient(sockfd);
        return ESP_FAIL;
    }

    // First frame == the connect event; a CLOSE frame never registers.
    if (pkt.type != HTTPD_WS_TYPE_CLOSE) {
        AddClient(sockfd);
    }

    // Redundant with httpd_sess_process's own LRU update; kept for defense in depth.
    httpd_sess_update_lru_counter(req->handle, sockfd);

    std::vector<uint8_t> buf;
    if (pkt.len > 0) {
        buf.resize(pkt.len + 1);
        pkt.payload = buf.data();
        ret = httpd_ws_recv_frame(req, &pkt, pkt.len);
        if (ret != ESP_OK) {
            RemoveClient(sockfd);
            return ret;
        }
        buf[pkt.len] = 0;  // NUL-terminate for the text comparisons below
    }

    // Every inbound frame — PING, app heartbeat, PONG, CLOSE — proves liveness.
    TouchClient(sockfd);

    switch (pkt.type) {
        case HTTPD_WS_TYPE_PING:
            // PING is answered here (RFC 6455: echo the payload in a PONG).
            {
                httpd_ws_frame_t pong = {};
                pong.type = HTTPD_WS_TYPE_PONG;
                pong.payload = pkt.payload;
                pong.len = pkt.len;
                httpd_ws_send_frame(req, &pong);
            }
            break;
        case HTTPD_WS_TYPE_CLOSE:
            // httpd closes the socket after the handler returns — just untrack.
            RemoveClient(sockfd);
            break;
        case HTTPD_WS_TYPE_TEXT:
            // App-level heartbeat: raw text "ping" -> "pong".
            if (pkt.len == 4 && memcmp(buf.data(), "ping", 4) == 0) {
                httpd_ws_frame_t resp = {};
                resp.type = HTTPD_WS_TYPE_TEXT;
                resp.payload = (uint8_t*)"pong";
                resp.len = 4;
                httpd_ws_send_frame(req, &resp);
                break;
            }
            // App-level heartbeat: {"type":"ping"} -> {"event":"pong"}.
            if (pkt.len == 15 && memcmp(buf.data(), "{\"type\":\"ping\"}", 15) == 0) {
                httpd_ws_frame_t resp = {};
                resp.type = HTTPD_WS_TYPE_TEXT;
                resp.payload = (uint8_t*)"{\"event\":\"pong\"}";
                resp.len = 16;
                httpd_ws_send_frame(req, &resp);
                break;
            }
            break;
        default:
            // PONG and BINARY frames are consumed silently.
            break;
    }
    return ESP_OK;
}

/// Runs on the httpd task (queued by Broadcast); a failed send or a
/// non-WS fd untracks the client.
void BroadcastWork(void* arg) {
    std::unique_ptr<BroadcastCtx> ctx(static_cast<BroadcastCtx*>(arg));

    std::vector<int> fds;
    {
        std::lock_guard<std::mutex> lock(g_clients_mutex);
        fds = LiveFdsLocked(NowMs());
    }

    httpd_ws_frame_t pkt = {};
    pkt.type = HTTPD_WS_TYPE_TEXT;
    pkt.payload = (uint8_t*)ctx->frame.c_str();
    pkt.len = ctx->frame.size();

    for (int fd : fds) {
        // send_frame_async has no handshake check — a stale fd reused by
        // plain HTTP would get WS bytes spliced into its response.
        if (httpd_ws_get_fd_info(ctx->server, fd) != HTTPD_WS_CLIENT_WEBSOCKET) {
            RemoveClient(fd);
            continue;
        }
        if (httpd_ws_send_frame_async(ctx->server, fd, &pkt) != ESP_OK) {
            RemoveClient(fd);
        }
    }

    // Off the httpd task: each close takes a ctrl-msg slot only that task
    // returns, and BroadcastWork itself runs there. Skip the spawn when idle.
    if (!HasPrunedFds()) {
        return;
    }
    if (rest_api::RunOnWorkerTask([server = ctx->server]() { CloseStaleSessions(server); }) ==
        nullptr) {
        ESP_LOGW(TAG, "worker spawn failed — zombie sessions left to the LRU purge");
    }
}

}  // namespace

/// WS gate: runs before the 101 goes out; wrong/absent ?token= answers
/// 401 and returns ESP_FAIL (httpd closes the socket, upgrade never completes).
static esp_err_t WsPreHandshake(httpd_req_t* req) {
    // A locked-out peer fails even with a valid token (shared counter).
    const bool locked = rest_api::AuthLockedOut(req);
    if (!locked && rest_api::WsTokenOk(req)) {
        rest_api::RecordAuthSuccess(req);
        return ESP_OK;
    }
    if (!locked) {
        rest_api::RecordAuthFailure(req);
        // The raw target carries ?token=...; log the path only.
        std::string path(req->uri);
        const size_t query = path.find('?');
        if (query != std::string::npos) {
            path.resize(query);
        }
        ESP_LOGW(TAG, "WS auth failed: %s", path.c_str());
        // No stall: a synchronous delay here would freeze the shared httpd task.
    }
    httpd_resp_set_status(req, "401 Unauthorized");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "WWW-Authenticate", "Bearer");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_sendstr(
        req, "{\"success\":false,\"error\":\"unauthorized\",\"message\":\"Invalid API key\"}");
    return ESP_FAIL;
}

void RegisterHandler(httpd_handle_t server) {
    // Plain assignments (not designated initializers): httpd_uri_t's
    // trailing fields would trip -Wextra.
    httpd_uri_t uri_ws = {};
    uri_ws.uri = "/ws";
    uri_ws.method = HTTP_GET;
    uri_ws.handler = &HandleWebSocket;
    uri_ws.user_ctx = nullptr;
    uri_ws.is_websocket = true;
    uri_ws.handle_ws_control_frames = true;
#ifdef CONFIG_HTTPD_WS_PRE_HANDSHAKE_CB_SUPPORT
    uri_ws.ws_pre_handshake_cb = &WsPreHandshake;
#else
    // The config gate must not silently disable the /ws auth check.
#error "CONFIG_HTTPD_WS_PRE_HANDSHAKE_CB_SUPPORT is required (sdkconfig.defaults)"
#endif

    esp_err_t err = httpd_register_uri_handler(server, &uri_ws);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "register /ws: %s", esp_err_to_name(err));
    }
}

void ClearClients() {
    std::lock_guard<std::mutex> lock(g_clients_mutex);
    g_clients.clear();
    g_pruned_fds.clear();
}

/// Closes the httpd session of a pruned fd so the zombie stops holding a
/// socket slot; off the httpd task — each close queues a ctrl message.
void CloseStaleSessions(httpd_handle_t server) {
    std::vector<int> candidates;
    {
        // Untrack first; a connect frame racing us re-adds the fd.
        std::lock_guard<std::mutex> lock(g_clients_mutex);
        candidates.assign(g_pruned_fds.begin(), g_pruned_fds.end());
        g_pruned_fds.clear();
    }
    for (int fd : candidates) {
        {
            std::lock_guard<std::mutex> lock(g_clients_mutex);
            if (g_clients.find(fd) != g_clients.end()) {
                continue;  // re-registered since the probe — a live client
            }
        }
        // lwIP may have reused the fd for a fresh plain-HTTP session — only
        // a still-upgraded socket may be closed.
        if (httpd_ws_get_fd_info(server, fd) == HTTPD_WS_CLIENT_WEBSOCKET) {
            const esp_err_t err = httpd_sess_trigger_close(server, fd);
            if (err == ESP_OK) {
                ESP_LOGW(TAG, "closed zombie WS session (fd=%d)", fd);
            } else {
                ESP_LOGW(TAG, "stale session close failed (fd=%d): %s", fd, esp_err_to_name(err));
            }
        }
    }
}

void EvictAllSessions(httpd_handle_t server) {
    {
        // Closed sessions never send a WS CLOSE frame — clear or broadcasts
        // target dead fds until the stale prune.
        std::lock_guard<std::mutex> lock(g_clients_mutex);
        g_clients.clear();
        g_pruned_fds.clear();
    }
    // Key rotation: close every open WS session — a handshaked-but-silent
    // old-key client is invisible to g_clients. Plain HTTP is left alone.
    // QUEUE_WORK_BLOCKING: each close takes a ctrl-msg slot only the httpd
    // task returns — callers run this on a worker task (OOM fallback excepted).
    const std::vector<int> fds = AllSessionFds(server);
    unsigned closed = 0;
    for (int fd : fds) {
        if (httpd_ws_get_fd_info(server, fd) != HTTPD_WS_CLIENT_WEBSOCKET) {
            continue;  // plain HTTP: REST on this page, or a post-rotation peer
        }
        const esp_err_t err = httpd_sess_trigger_close(server, fd);
        if (err != ESP_OK) {
            // QUEUE_WORK_BLOCKING blocks instead of dropping the work — a
            // failure here is a ctrl-socket send error, not a full queue.
            ESP_LOGW(TAG,
                     "close request failed (queue/send error, fd=%d): %s — session may close later",
                     fd, esp_err_to_name(err));
            continue;
        }
        ++closed;
    }
    if (!fds.empty()) {
        ESP_LOGW(TAG, "API key changed — closed %u of %u open session(s)", closed,
                 static_cast<unsigned>(fds.size()));
    }
}

void Broadcast(httpd_handle_t server, const char* event, cJSON* data) {
    if (data == nullptr) {
        data = cJSON_CreateObject();  // empty data object
    }
    {
        // Nobody listening — `data` is freed on every path.
        std::lock_guard<std::mutex> lock(g_clients_mutex);
        if (server == nullptr || g_clients.empty()) {
            cJSON_Delete(data);
            return;
        }
    }

    cJSON* root = cJSON_CreateObject();
    if (root == nullptr) {
        cJSON_Delete(data);
        return;
    }
    cJSON_AddStringToObject(root, "event", event);
    if (cJSON_AddItemToObject(root, "data", data) == 0) {
        // Attach failed (OOM) — the item was NOT consumed by the parent.
        cJSON_Delete(data);
        cJSON_Delete(root);
        return;
    }
    cJSON_AddNumberToObject(root, "timestamp", (double)NowMs());

    char* str = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (str == nullptr) {
        return;
    }

    std::unique_ptr<BroadcastCtx> ctx(new (std::nothrow) BroadcastCtx());
    if (ctx == nullptr) {
        cJSON_free(str);
        return;
    }
    ctx->server = server;
    ctx->frame = str;
    cJSON_free(str);

    // httpd_ws_send_frame_async is httpd-task-only — queue the work
    // (QUEUE_WORK_BLOCKING: failure = a ctrl-socket send error, not a drop).
    if (httpd_queue_work(server, BroadcastWork, ctx.get()) != ESP_OK) {
        ESP_LOGW(TAG, "broadcast queue/send failed");
        return;  // `ctx` frees here — the queue never took it
    }
    ctx.release();  // the queued BroadcastWork takes ownership
}

}  // namespace ws
}  // namespace web_server
