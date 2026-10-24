#pragma once

#include <esp_netif_ip_addr.h>

#include <atomic>

namespace sys {

/// UDP DNS responder for the provisioning AP: every :53 query resolves to
/// the AP address (captive-portal redirect); recv loop in its own task.
class DnsServer {
public:
    DnsServer() = default;
    DnsServer(const DnsServer&) = delete;
    DnsServer& operator=(const DnsServer&) = delete;

    /// Starts serving (no-op when already running); every A query resolves
    /// to @p answer_ip.
    bool Start(const esp_ip4_addr_t& answer_ip);

    /// Best-effort stop: closes the socket; the task exits on its next
    /// recvfrom().
    void Stop();

private:
    void Run();

    int fd_ = -1;
    esp_ip4_addr_t answer_ip_{};
    std::atomic<bool> running_{false};
};

}  // namespace sys
