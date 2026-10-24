#include "hub_core/psram_resource.h"

#include <esp_heap_caps.h>
#include <esp_log.h>

#include <cstddef>
#include <cstdlib>

static const char* TAG = "HubPsram";

namespace hub_core {

namespace {

constexpr uint32_t kPsramCaps = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT;

/// Upstream: one heap_caps PSRAM block per do_allocate call.
class PsramUpstreamResource final : public std::pmr::memory_resource {
public:
    void* do_allocate(std::size_t bytes, std::size_t alignment) override {
        if (bytes == 0) {
            bytes = 1;
        }
        // heap_caps_aligned_alloc: power-of-two alignment, size a multiple.
        const std::size_t align = alignment < 4 ? 4 : alignment;
        bytes = (bytes + align - 1) & ~(align - 1);
        void* p = heap_caps_aligned_alloc(align, bytes, kPsramCaps);
        if (p == nullptr) {
            // Device storage exhausted — unrecoverable for the hub.
            ESP_LOGE(TAG, "PSRAM pmr upstream OOM (%zu B)", bytes);
            abort();
        }
        return p;
    }

    void do_deallocate(void* p, std::size_t /*bytes*/, std::size_t /*alignment*/) override {
        heap_caps_free(p);
    }

    bool do_is_equal(const std::pmr::memory_resource& other) const noexcept override {
        return this == &other;
    }
};

}  // namespace

std::pmr::memory_resource* GetHubPsramResource() {
    // Leak-on-exit: process-lifetime pool, never torn down (no static
    // destruction-order hazard; devices live until reboot).
    static PsramUpstreamResource* const upstream = new PsramUpstreamResource();
    static const std::pmr::pool_options opts = [] {
        // Nodes/small strings pooled; larger blocks go straight upstream.
        std::pmr::pool_options o{};
        o.max_blocks_per_chunk = 16;
        o.largest_required_pool_block = 256;
        return o;
    }();
    // synchronized: Device copies escape the registry mutex (alloc/free
    // on httpd/mqtt/transport tasks) — an unsync pool would race.
    static std::pmr::synchronized_pool_resource* const pool =
        new std::pmr::synchronized_pool_resource(opts, upstream);
    return pool;
}

void InitHubPsramPmrDefault() {
    // Two-tier OOM policy: cJSON/malloc allocations degrade gracefully
    // (null-checked); pmr/default-registry allocations abort — unrecoverable design.
    std::pmr::set_default_resource(GetHubPsramResource());
    ESP_LOGI(TAG, "pmr default resource -> PSRAM pool");
}

}  // namespace hub_core
