#include "sys/dns_server.h"

#include <errno.h>
#include <lwip/sockets.h>

#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <cstdint>
#include <cstring>

static const char* TAG = "DnsServer";

namespace sys {

namespace {

constexpr uint16_t kDnsPort = 53;
constexpr size_t kDnsHeaderLen = 12;
// name pointer (2) + type/class/TTL/rdlen (10) + IPv4 rdata (4)
constexpr size_t kDnsAnswerLen = 16;
constexpr size_t kDnsMaxPacket = 512;
constexpr uint16_t kDnsTypeA = 1;
constexpr uint16_t kDnsClassIn = 1;

/// End offset (past QTYPE+QCLASS) of the first question; 0 when malformed.
size_t QuestionEnd(const uint8_t* buf, size_t len) {
    size_t pos = kDnsHeaderLen;
    for (;;) {
        if (pos >= len) {
            return 0;
        }
        const uint8_t label = buf[pos];
        if (label == 0) {
            ++pos;
            break;
        }
        if ((label & 0xc0) != 0 || static_cast<size_t>(label) + 1 > len - pos) {
            return 0;  // compression pointer / label overruns the datagram
        }
        pos += 1 + label;
    }
    if (len - pos < 4) {
        return 0;  // QTYPE + QCLASS must fit
    }
    return pos + 4;
}

}  // namespace

bool DnsServer::Start(const esp_ip4_addr_t& answer_ip) {
    if (running_.load()) {
        ESP_LOGW(TAG, "Start() called while already running — ignoring");
        return true;
    }
    answer_ip_.addr = answer_ip.addr;

    fd_ = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (fd_ < 0) {
        ESP_LOGE(TAG, "socket: errno %d", errno);
        return false;
    }

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(kDnsPort);
    if (bind(fd_, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) < 0) {
        ESP_LOGE(TAG, "bind(port %u): errno %d", static_cast<unsigned>(kDnsPort), errno);
        close(fd_);
        fd_ = -1;
        return false;
    }

    running_.store(true);
    BaseType_t ok = xTaskCreate([](void* arg) { static_cast<DnsServer*>(arg)->Run(); },
                                "dns_server", 4096, this, 5, nullptr);
    if (ok != pdPASS) {
        ESP_LOGE(TAG, "Failed to create the DNS task");
        running_.store(false);
        close(fd_);
        fd_ = -1;
        return false;
    }

    ESP_LOGI(TAG, "DNS hijack on UDP %u (all names resolve to the AP)",
             static_cast<unsigned>(kDnsPort));
    return true;
}

void DnsServer::Stop() {
    if (!running_.load()) {
        return;
    }
    running_.store(false);
    shutdown(fd_, SHUT_RDWR);  // unblocks the recvfrom() in Run()
    close(fd_);
    fd_ = -1;
    ESP_LOGI(TAG, "DNS server stopped");
}

void DnsServer::Run() {
    uint8_t buffer[kDnsMaxPacket];
    while (true) {
        struct sockaddr_in client;
        socklen_t client_len = sizeof(client);
        int len = recvfrom(fd_, buffer, sizeof(buffer), 0,
                           reinterpret_cast<struct sockaddr*>(&client), &client_len);
        if (len < 0) {
            if (running_.load()) {
                ESP_LOGE(TAG, "recvfrom: errno %d", errno);
                continue;
            }
            break;  // Stop() closed the socket
        }
        // Malformed / oversized datagrams are dropped (512-byte response cap).
        if (static_cast<size_t>(len) < kDnsHeaderLen ||
            static_cast<size_t>(len) + kDnsAnswerLen > sizeof(buffer)) {
            continue;
        }
        if (buffer[4] == 0 && buffer[5] == 0) {
            continue;  // QDCOUNT == 0: nothing to echo
        }
        const size_t qend = QuestionEnd(buffer, len);
        if (qend == 0) {
            continue;
        }
        const uint16_t qtype = static_cast<uint16_t>((buffer[qend - 4] << 8) | buffer[qend - 3]);
        const uint16_t qclass = static_cast<uint16_t>((buffer[qend - 2] << 8) | buffer[qend - 1]);
        const bool answer_a = qtype == kDnsTypeA && qclass == kDnsClassIn;

        buffer[2] = static_cast<uint8_t>(buffer[2] | 0x80);  // QR = response
        buffer[3] = static_cast<uint8_t>(buffer[3] | 0x80);  // RA = recursion available
        buffer[4] = 0;                                       // QDCOUNT = 1, the question echoed
        buffer[5] = 1;
        // NS/AR stale counts (an EDNS OPT record we never copy) break the answer.
        buffer[8] = 0;
        buffer[9] = 0;
        buffer[10] = 0;
        buffer[11] = 0;

        int out_len = static_cast<int>(qend);  // header + question only
        if (answer_a) {
            buffer[6] = 0;
            buffer[7] = 1;                            // ANCOUNT = 1
            memcpy(&buffer[out_len], "\xc0\x0c", 2);  // name pointer to the question
            out_len += 2;
            // type=A, class=IN, TTL=60, rdlength=4
            memcpy(&buffer[out_len], "\x00\x01\x00\x01\x00\x00\x00\x3c\x00\x04", 10);
            out_len += 10;
            memcpy(&buffer[out_len], &answer_ip_.addr, 4);  // already network order
            out_len += 4;
        } else {
            buffer[6] = 0;
            buffer[7] = 0;  // ANCOUNT = 0 — a non-A/IN query is answered empty
        }
        sendto(fd_, buffer, out_len, 0, reinterpret_cast<struct sockaddr*>(&client), client_len);
    }
    vTaskDelete(nullptr);
}

}  // namespace sys
