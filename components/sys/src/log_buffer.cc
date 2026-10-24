#include "sys/log_buffer.h"

#include <esp_heap_caps.h>
#include <esp_log.h>

#include <algorithm>
#include <cstring>

static const char* TAG = "LogBuffer";

namespace sys {

LogBuffer::~LogBuffer() {
    // The process-wide buffer outlives everything; safe to stack in tests.
    heap_caps_free(ring_);
}

void LogBuffer::Configure(size_t capacity_bytes) {
    // No ESP_LOGx under mutex_ — the installed capture hook would re-enter and self-deadlock.
    uint32_t caps = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT;
    bool alloc_failed = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (ring_ != nullptr || capacity_bytes == 0) {
            return;  // one-shot configure
        }
        uint8_t* buf = static_cast<uint8_t*>(heap_caps_malloc(capacity_bytes, caps));
        if (buf == nullptr) {
            caps = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
            buf = static_cast<uint8_t*>(heap_caps_malloc(capacity_bytes, caps));
        }
        if (buf != nullptr) {
            ring_ = buf;
            capacity_ = capacity_bytes;
            alloc_caps_ = caps;
            records_ = std::make_unique<LogRecord[]>(kMaxRecords);  // zero-initialized
        } else {
            alloc_failed = true;  // fall through: warn outside the lock
        }
    }
    if (alloc_failed) {
        ESP_LOGW(TAG, "ring alloc failed (%u bytes) — log capture disabled",
                 static_cast<unsigned>(capacity_bytes));
    } else {
        ESP_LOGI(TAG, "log ring: %u bytes (%s)", static_cast<unsigned>(capacity_bytes),
                 (caps & MALLOC_CAP_SPIRAM) != 0 ? "PSRAM" : "internal");
    }
}

void LogBuffer::AppendFragment(const char* data, size_t len) {
    if (data == nullptr || len == 0) {
        return;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    if (ring_ == nullptr) {
        return;  // disabled — never disturb the log path
    }
    size_t pos = 0;
    while (pos < len) {
        const char* chunk = data + pos;
        const size_t remaining = len - pos;
        const void* nl = memchr(chunk, '\n', remaining);
        const size_t take =
            nl != nullptr ? static_cast<size_t>(static_cast<const char*>(nl) - chunk) : remaining;
        AppendPendingLocked(chunk, take);
        if (nl == nullptr) {
            break;  // fragment ends mid-line — wait for more
        }
        PushLineLocked(pending_, pending_len_);
        pending_len_ = 0;
        pos += take + 1;
    }
}

std::vector<std::string> LogBuffer::GetLines(size_t max_lines) const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<std::string> out;
    if (ring_ == nullptr || record_count_ == 0 || max_lines == 0) {
        return out;
    }
    const size_t count = std::min(record_count_, max_lines);
    const size_t skip = record_count_ - count;  // oldest records dropped first
    out.reserve(count);
    for (size_t i = 0; i < count; ++i) {
        const LogRecord& rec = records_[(record_head_ + skip + i) % kMaxRecords];
        std::string line;
        line.resize(rec.len);
        const size_t first =
            std::min(static_cast<size_t>(rec.len), capacity_ - static_cast<size_t>(rec.offset));
        memcpy(line.data(), ring_ + rec.offset, first);
        if (static_cast<size_t>(rec.len) > first) {
            memcpy(line.data() + first, ring_, rec.len - first);
        }
        out.push_back(std::move(line));
    }
    return out;
}

void LogBuffer::Stats(size_t* total_appended) const {
    if (total_appended == nullptr) {
        return;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    *total_appended = total_appended_;
}

void LogBuffer::AppendPendingLocked(const char* data, size_t len) {
    if (pending_len_ >= kMaxLineBytes) {
        return;  // line already truncated
    }
    const size_t room = kMaxLineBytes - pending_len_;
    const size_t take = std::min(len, room);
    memcpy(pending_ + pending_len_, data, take);
    pending_len_ += take;
}

void LogBuffer::PushLineLocked(const char* data, size_t len) {
    if (len == 0) {
        return;  // empty line — nothing worth a record
    }
    if (len > capacity_) {
        len = capacity_;
    }
    // Drop-oldest until both a record slot and the bytes fit.
    while (record_count_ >= kMaxRecords || used_bytes_ + len > capacity_) {
        if (record_count_ == 0) {
            break;  // empty ring always fits (len <= capacity_)
        }
        EvictLocked();
    }
    // Records are circularly contiguous; copy with wrap.
    const size_t first = std::min(len, capacity_ - write_pos_);
    memcpy(ring_ + write_pos_, data, first);
    if (len > first) {
        memcpy(ring_, data + first, len - first);
    }
    LogRecord& rec = records_[(record_head_ + record_count_) % kMaxRecords];
    rec.offset = static_cast<uint32_t>(write_pos_);
    rec.len = static_cast<uint16_t>(len);
    write_pos_ = (write_pos_ + len) % capacity_;
    used_bytes_ += len;
    record_count_++;
    total_appended_++;
}

void LogBuffer::EvictLocked() {
    const LogRecord& rec = records_[record_head_];
    used_bytes_ -= rec.len;
    record_head_ = (record_head_ + 1) % kMaxRecords;
    record_count_--;
}

size_t ScrubLogFragment(const char* data, size_t len, char* out) {
    size_t kept = 0;
    for (size_t i = 0; i < len; ++i) {
        const unsigned char c = static_cast<unsigned char>(data[i]);
        if (c == '\n' || (c >= 0x20 && c != 0x7F)) {
            out[kept++] = static_cast<char>(c);
        }
    }
    return kept;
}

bool ParseLogLine(const std::string& raw, char prev_level, LogLineFields* out) {
    if (out == nullptr) {
        return false;
    }
    // Continuation defaults: inherited level, no header fields.
    out->ms = 0;
    out->tag.clear();
    out->msg = raw;
    if (!out->msg.empty() && out->msg.back() == '\r') {
        out->msg.pop_back();
    }
    out->level = prev_level != '\0' ? prev_level : 'I';
    // Header: "L (ms) TAG: msg".
    const size_t n = raw.size();
    if (n < 9) {
        return false;  // shorter than "I (1) T: "
    }
    const char level = raw[0];
    if (strchr("EIWVD", level) == nullptr || raw[1] != ' ' || raw[2] != '(') {
        return false;
    }
    size_t i = 3;
    uint64_t ms = 0;
    size_t digits = 0;
    while (i < n && raw[i] >= '0' && raw[i] <= '9') {
        ms = ms * 10 + static_cast<uint64_t>(raw[i] - '0');
        ++i;
        ++digits;
    }
    if (digits == 0 || i + 1 >= n || raw[i] != ')' || raw[i + 1] != ' ') {
        return false;
    }
    i += 2;
    // Tag runs up to ": " and may not itself contain ':'.
    const size_t colon = raw.find(':', i);
    if (colon == std::string::npos || colon == i || colon + 1 >= n || raw[colon + 1] != ' ') {
        return false;
    }
    out->level = level;
    out->ms = static_cast<uint32_t>(ms);
    out->tag = raw.substr(i, colon - i);
    out->msg = raw.substr(colon + 2);
    if (!out->msg.empty() && out->msg.back() == '\r') {
        out->msg.pop_back();
    }
    return true;
}

}  // namespace sys
