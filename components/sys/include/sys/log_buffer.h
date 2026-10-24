#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace sys {

/// Fixed-capacity record in the ring: line bytes live at `offset`,
/// wrapping around `capacity` (copied out in at most two pieces).
struct LogRecord {
    uint32_t offset;
    uint16_t len;
};

/// Thread-safe drop-oldest byte ring of reassembled log lines.
/// Disabled until Configure(); every method is safe from any task.
class LogBuffer {
public:
    /// 512B pending-line buffer -> at most 511 stored line bytes.
    static constexpr size_t kMaxLineBytes = 511;
    /// Line-count cap (records index size).
    static constexpr size_t kMaxRecords = 500;

    LogBuffer() = default;
    ~LogBuffer();

    LogBuffer(const LogBuffer&) = delete;
    LogBuffer& operator=(const LogBuffer&) = delete;

    /// Allocates the ring (PSRAM preferred, internal fallback); a failed
    /// allocation leaves the buffer disabled — the log path never crashes.
    void Configure(size_t capacity_bytes);

    /// Appends one vprintf fragment; '\n'-terminated pieces become records.
    /// A no-op before Configure().
    void AppendFragment(const char* data, size_t len);

    /// Oldest->newest copies of the last `max_lines` records.
    std::vector<std::string> GetLines(size_t max_lines) const;

    /// Monotonic count of lines ever appended (evictions included).
    void Stats(size_t* total_appended) const;

private:
    void PushLineLocked(const char* data, size_t len);
    void AppendPendingLocked(const char* data, size_t len);
    void EvictLocked();

    mutable std::mutex mutex_;
    uint8_t* ring_ = nullptr;
    size_t capacity_ = 0;
    uint32_t alloc_caps_ = 0;  // caps the ring actually landed in
    size_t write_pos_ = 0;     // next free byte (mod capacity_)
    size_t used_bytes_ = 0;
    // Heap-allocated in Configure: 500 records inline would make every
    // LogBuffer instance ~4.6KB (stack-unsafe for callers).
    std::unique_ptr<LogRecord[]> records_;
    size_t record_head_ = 0;  // index of the oldest record
    size_t record_count_ = 0;
    size_t total_appended_ = 0;
    char pending_[kMaxLineBytes + 1] = {};
    size_t pending_len_ = 0;
};

/// Fields extracted from one captured line.
struct LogLineFields {
    uint32_t ms;
    char level;
    std::string tag;
    std::string msg;
};

/// Copies `data` into `out` (>= len bytes) dropping C0 controls and DEL except
/// '\n', so a format-controlled value cannot forge a line break or ANSI escape
/// in the captured transcript. Returns the scrubbed length.
size_t ScrubLogFragment(const char* data, size_t len, char* out);

/// Parses "L (ms) TAG: msg" (esp_log format, colors off); a non-matching
/// line is a continuation (prev_level, empty tag, msg=raw). True on header.
bool ParseLogLine(const std::string& raw, char prev_level, LogLineFields* out);

}  // namespace sys
