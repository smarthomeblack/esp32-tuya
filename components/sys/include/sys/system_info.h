#pragma once

#include <cstdint>
#include <string>

namespace sys {

/// Read-only system information helpers (safe from any task).
class SystemInfo {
public:
    /// Base MAC (efuse) formatted as "AA:BB:CC:DD:EE:FF".
    static std::string GetMacAddress();

    /// ESP-IDF version string, e.g. "v6.0.1".
    static std::string GetIdfVersion();

    /// Application version from the app descriptor (VERSION file).
    static std::string GetAppVersion();

    /// Short reset reason string, e.g. "POWERON", "SW", "PANIC".
    static std::string GetResetReason();

    static uint32_t GetFreeHeapInternal();

    /// Free PSRAM heap in bytes (0 when no PSRAM).
    static uint32_t GetFreeHeapPsram();

    /// Lowest ever free internal heap (watermark).
    static uint32_t GetMinFreeHeapInternal();

    /// Total size of all internal heap regions in bytes.
    static uint32_t GetTotalHeapInternal();

    /// Largest free internal heap block in bytes.
    static uint32_t GetLargestFreeHeapInternal();

    /// Total PSRAM heap in bytes (0 when no PSRAM).
    static uint32_t GetTotalHeapPsram();

    /// Largest free PSRAM block in bytes (0 when no PSRAM).
    static uint32_t GetLargestFreeHeapPsram();

    static uint64_t GetUptimeMs();
};

}  // namespace sys
