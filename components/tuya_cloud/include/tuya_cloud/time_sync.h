#pragma once

#include <cstdint>

namespace tuya_cloud {

/// Wall-clock time source for the request pipeline (milliseconds everywhere; esp_timer for
/// durations). NowMs() reports the boot epoch until a successful SyncBlocking() (which needs the
/// network up).
class TimeSync {
public:
    /// Milliseconds since the Unix epoch from gettimeofday. Not monotonic
    /// (an SNTP step jumps it); use esp_timer for durations.
    static int64_t NowMs();

    /// Blocking SNTP sync against CONFIG_TUYA_NTP_SERVER; repeatable (re-polls immediately).
    /// Returns false on timeout; requires the network connected.
    bool SyncBlocking(uint32_t timeout_ms);
};

}  // namespace tuya_cloud
