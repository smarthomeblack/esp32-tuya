#pragma once

#include <cstdint>

namespace sys {

namespace CpuMonitor {

/// Cached sample result (valid after the first 5 s tick).
struct Snapshot {
    bool valid;
    int usage_pct;
    int core0_pct;
    int core1_pct;
};

/// One raw runtime-stats reading (counters are esp_timer us).
struct CpuSample {
    uint64_t t_us;
    uint64_t idle0_us;
    uint64_t idle1_us;
};

/// Overall usage across both cores, clamped 0..100; -1 on an invalid
/// window (b.t_us <= a.t_us).
int ComputeCpuUsagePct(const CpuSample& a, const CpuSample& b);

/// Single-core usage (core 0 or 1), clamped 0..100; -1 on an invalid
/// window or core.
int ComputeCoreUsagePct(const CpuSample& a, const CpuSample& b, int core);

/// Starts the 5 s periodic sampler (base sample at Start). Idempotent.
void Start();

/// Latest snapshot; {valid=false, 0, 0, 0} until the first tick (or when
/// runtime stats are compiled out).
Snapshot Get();

}  // namespace CpuMonitor

}  // namespace sys
