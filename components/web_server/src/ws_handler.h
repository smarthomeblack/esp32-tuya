/// INTERNAL (src-only) — WebSocket session tracking + broadcast.
/// Broadcast is safe from any task (sends defer to the httpd task).

#pragma once

#include <cJSON.h>
#include <esp_http_server.h>

namespace web_server {
namespace ws {

/// httpd socket cap shared with WebServer::Start (config.max_open_sockets);
/// headroom over 1 WS + a 4-way REST burst so the LRU purge never picks the WS.
constexpr int kMaxOpenSockets = 10;

/// Registers GET /ws; register BEFORE the static-asset wildcard.
void RegisterHandler(httpd_handle_t server);

/// Forgets every tracked client (WebServer::Stop).
void ClearClients();

/// Closes every live WS session (API-key rotation must evict old-key
/// clients); safe from any task — normally off httpd, OOM fallback on httpd.
void EvictAllSessions(httpd_handle_t server);

/// Closes the sessions of fds PruneStale dropped — must run OFF the httpd task.
void CloseStaleSessions(httpd_handle_t server);

/// Pushes {"event","data","timestamp"} to every client; owns `data` on
/// every path (freed even on the no-op paths).
void Broadcast(httpd_handle_t server, const char* event, cJSON* data);

}  // namespace ws
}  // namespace web_server
