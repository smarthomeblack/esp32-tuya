#include "sys/self_test.h"

#include <esp_log.h>
#include <nvs.h>

#include <cstdio>
#include <string>
#include <vector>

#include "sys/cpu_monitor.h"
#include "sys/log_buffer.h"
#include "sys/settings.h"

static const char* TAG = "SysTest";

namespace sys {

namespace {

struct TestResult {
    int total = 0;
    int passed = 0;
};

void CheckTrue(TestResult& r, const char* name, bool condition) {
    ++r.total;
    if (condition) {
        ++r.passed;
        return;
    }
    ESP_LOGE(TAG, "FAIL %s", name);
}

void CheckEqStr(TestResult& r, const char* name, const std::string& expected,
                const std::string& got) {
    ++r.total;
    if (expected == got) {
        ++r.passed;
        return;
    }
    ESP_LOGE(TAG, "FAIL %s: expected=\"%.64s\" got=\"%.64s\"", name, expected.c_str(), got.c_str());
}

void CheckEqInt(TestResult& r, const char* name, int expected, int got) {
    ++r.total;
    if (expected == got) {
        ++r.passed;
        return;
    }
    ESP_LOGE(TAG, "FAIL %s: expected=%d got=%d", name, expected, got);
}

// A boot-shaped line + a body long enough to exercise truncation.
std::string LongLine(size_t body_len) {
    std::string line = "I (1000) LongTag: ";
    line.reserve(line.size() + body_len);
    for (size_t i = 0; i < body_len; ++i) {
        line.push_back(static_cast<char>('a' + (i % 26)));
    }
    return line;
}

// ---- Section A: LogBuffer fragment reassembly + ring eviction ----

void TestLogBufferReassembly(TestResult& r) {
    // Unconfigured: everything is a no-op, GetLines stays empty.
    {
        LogBuffer buffer;
        buffer.AppendFragment("I (1) T: hi\n", 12);
        CheckTrue(r, "unconfigured_getlines_empty", buffer.GetLines(10).empty());
    }
    // The canonical 3-fragment line: prefix, body, "\n".
    {
        LogBuffer buffer;
        buffer.Configure(4096);
        buffer.AppendFragment("I (123) Tag: ", 13);
        buffer.AppendFragment("hello", 5);
        buffer.AppendFragment("\n", 1);
        const std::vector<std::string> lines = buffer.GetLines(10);
        CheckEqInt(r, "reassembly_count", 1, static_cast<int>(lines.size()));
        CheckEqStr(r, "reassembly_line", "I (123) Tag: hello", lines.empty() ? "" : lines.front());
    }
    // One fragment carrying multiple lines splits at every '\n'; the
    // unterminated tail stays pending until its own terminator arrives.
    {
        LogBuffer buffer;
        buffer.Configure(4096);
        buffer.AppendFragment("one\ntwo\nthree", 13);
        std::vector<std::string> lines = buffer.GetLines(10);
        CheckEqInt(r, "multinl_count", 2, static_cast<int>(lines.size()));
        CheckEqStr(r, "multinl_first", "one", lines.empty() ? "" : lines.front());
        CheckEqStr(r, "multinl_second", "two", lines.size() < 2 ? "" : lines[1]);
        buffer.AppendFragment("!\n", 2);
        lines = buffer.GetLines(10);
        CheckEqInt(r, "multinl_pending_count", 3, static_cast<int>(lines.size()));
        CheckEqStr(r, "multinl_pending_line", "three!", lines.size() < 3 ? "" : lines[2]);
    }
    // A line longer than the 512B pending buffer truncates to 511 bytes.
    {
        LogBuffer buffer;
        buffer.Configure(4096);
        const std::string long_line = LongLine(600);
        buffer.AppendFragment(long_line.c_str(), long_line.size());
        buffer.AppendFragment("\n", 1);
        const std::vector<std::string> lines = buffer.GetLines(10);
        CheckEqInt(r, "truncate_count", 1, static_cast<int>(lines.size()));
        CheckEqInt(r, "truncate_len", static_cast<int>(LogBuffer::kMaxLineBytes),
                   lines.empty() ? -1 : static_cast<int>(lines.front().size()));
    }
}

void TestLogBufferEviction(TestResult& r) {
    // 1KB ring, 60 x ~34B lines: the oldest are evicted, the newest stay.
    LogBuffer buffer;
    buffer.Configure(1024);
    for (int i = 0; i < 60; ++i) {
        char line[48];
        const int n = snprintf(line, sizeof(line), "I (%d) Evict: line %d", i, i);
        buffer.AppendFragment(line, static_cast<size_t>(n));
        buffer.AppendFragment("\n", 1);
    }
    size_t total = 0;
    buffer.Stats(&total);
    CheckEqInt(r, "evict_total_appended", 60, static_cast<int>(total));
    const std::vector<std::string> lines = buffer.GetLines(500);
    CheckTrue(r, "evict_newest_retrievable", lines.size() < 60 && !lines.empty());
    CheckEqStr(r, "evict_newest_line", "I (59) Evict: line 59", lines.empty() ? "" : lines.back());
    // "I (0)" can no longer fit a 1KB ring next to 59 siblings.
    const std::string first = lines.empty() ? "" : lines.front();
    CheckTrue(r, "evict_oldest_gone", first != "I (0) Evict: line 0");

    // GetLines(max_lines) returns the NEWEST `max_lines` records only.
    LogBuffer bounded;
    bounded.Configure(4096);
    for (int i = 0; i < 10; ++i) {
        char line[48];
        const int n = snprintf(line, sizeof(line), "I (%d) Cap: n%d", i, i);
        bounded.AppendFragment(line, static_cast<size_t>(n));
        bounded.AppendFragment("\n", 1);
    }
    const std::vector<std::string> tail = bounded.GetLines(3);
    CheckEqInt(r, "maxlines_count", 3, static_cast<int>(tail.size()));
    CheckEqStr(r, "maxlines_oldest", "I (7) Cap: n7", tail.empty() ? "" : tail.front());
    CheckEqStr(r, "maxlines_newest", "I (9) Cap: n9", tail.size() < 2 ? "" : tail.back());
}

// ---- Section E: capture scrub (cloud-controlled names must not forge lines) ----

void TestLogScrub(TestResult& r) {
    // '\n' survives so the capture still splits records; CR/ESC/NUL do not.
    const std::string control =
        std::string("a") + '\x1B' + "[31mred" + '\x00' + '\x07' + '\x7F' + " \xC3\xA9\n";
    std::string out(control.size(), '\0');
    const size_t kept = ScrubLogFragment(control.data(), control.size(), out.data());
    out.resize(kept);
    CheckEqStr(r, "scrub_c0_del_keep_utf8", "a[31mred \xC3\xA9\n", out);
}

// ---- Section B: ParseLogLine ----

void TestParseLogLine(TestResult& r) {
    // Standard line: full header parses.
    {
        LogLineFields out;
        CheckTrue(r, "parse_std_ok", ParseLogLine("I (123) Tag: hello", '\0', &out));
        CheckEqInt(r, "parse_std_level", 'I', out.level);
        CheckEqInt(r, "parse_std_ms", 123, static_cast<int>(out.ms));
        CheckEqStr(r, "parse_std_tag", "Tag", out.tag);
        CheckEqStr(r, "parse_std_msg", "hello", out.msg);
    }
    // No header: continuation inherits the hint's level, tag/ms reset.
    {
        LogLineFields out;
        CheckTrue(r, "parse_cont_not_header", !ParseLogLine("just some text", 'W', &out));
        CheckEqInt(r, "parse_cont_level", 'W', out.level);
        CheckEqStr(r, "parse_cont_tag", "", out.tag);
        CheckEqInt(r, "parse_cont_ms", 0, static_cast<int>(out.ms));
        CheckEqStr(r, "parse_cont_msg", "just some text", out.msg);
    }
    // No hint: continuation falls back to 'I'.
    {
        LogLineFields out;
        CheckTrue(r, "parse_nohint_not_header", !ParseLogLine("continuation", '\0', &out));
        CheckEqInt(r, "parse_nohint_level", 'I', out.level);
    }
    // Other levels parse through.
    {
        LogLineFields out;
        CheckTrue(r, "parse_err_ok", ParseLogLine("E (5) Http: fail", '\0', &out));
        CheckEqInt(r, "parse_err_level", 'E', out.level);
        CheckEqStr(r, "parse_err_msg", "fail", out.msg);
    }
    {
        LogLineFields out;
        CheckTrue(r, "parse_warn_ok", ParseLogLine("W (77) Wifi: rssi -78", '\0', &out));
        CheckEqInt(r, "parse_warn_ms", 77, static_cast<int>(out.ms));
        CheckEqStr(r, "parse_warn_msg", "rssi -78", out.msg);
    }
    // Empty msg and a trailing '\r' (CRLF sources).
    {
        LogLineFields out;
        CheckTrue(r, "parse_emptymsg_ok", ParseLogLine("W (7) Tuya: ", '\0', &out));
        CheckEqStr(r, "parse_emptymsg_tag", "Tuya", out.tag);
        CheckEqStr(r, "parse_emptymsg_msg", "", out.msg);
    }
    {
        LogLineFields out;
        CheckTrue(r, "parse_cr_ok", ParseLogLine("I (9) T: hi\r", '\0', &out));
        CheckEqStr(r, "parse_cr_msg", "hi", out.msg);
    }
    // Missing ": " terminator -> continuation, not a header.
    {
        LogLineFields out;
        CheckTrue(r, "parse_nocolon_not_header", !ParseLogLine("D (12) no colon here", '\0', &out));
        CheckEqStr(r, "parse_nocolon_msg", "D (12) no colon here", out.msg);
    }
}

// ---- Section C: CPU usage vector table ----

void TestComputeCpuUsage(TestResult& r) {
    // 1s window: core0 idle 300ms, core1 fully busy -> 85% overall.
    const CpuMonitor::CpuSample a{0, 0, 0};
    const CpuMonitor::CpuSample b{1000000, 300000, 0};
    CheckEqInt(r, "cpu_usage_pct", 85, CpuMonitor::ComputeCpuUsagePct(a, b));
    CheckEqInt(r, "cpu_core0_pct", 70, CpuMonitor::ComputeCoreUsagePct(a, b, 0));
    CheckEqInt(r, "cpu_core1_pct", 100, CpuMonitor::ComputeCoreUsagePct(a, b, 1));
    // Fully idle window.
    const CpuMonitor::CpuSample idle{1000000, 1000000, 1000000};
    CheckEqInt(r, "cpu_usage_idle", 0, CpuMonitor::ComputeCpuUsagePct(a, idle));
    CheckEqInt(r, "cpu_core0_idle", 0, CpuMonitor::ComputeCoreUsagePct(a, idle, 0));
    // Invalid window (non-advancing clock) and invalid core.
    CheckEqInt(r, "cpu_usage_invalid", -1, CpuMonitor::ComputeCpuUsagePct(b, a));
    CheckEqInt(r, "cpu_usage_equal_t", -1, CpuMonitor::ComputeCpuUsagePct(a, a));
    CheckEqInt(r, "cpu_core_invalid", -1, CpuMonitor::ComputeCoreUsagePct(a, b, 2));
    // Idle delta beyond the window (counter glitch) clamps to 0, not negative.
    const CpuMonitor::CpuSample glitch{1000, 0, 0};
    const CpuMonitor::CpuSample glitch2{2000, 100000, 0};
    CheckEqInt(r, "cpu_usage_glitch_clamped", 0, CpuMonitor::ComputeCpuUsagePct(glitch, glitch2));
}

// ---- Section D: Settings NVS strings ----

// True when the NVS storage behind Settings is usable (degraded boot: not).
bool NvsStorageUsable() {
    nvs_handle_t probe = 0;
    const bool usable = nvs_open("selftest", NVS_READWRITE, &probe) == ESP_OK;
    if (probe != 0) {
        nvs_close(probe);
    }
    return usable;
}

void TestSettingsStrings(TestResult& r) {
    if (!NvsStorageUsable()) {
        // Degraded boot: fail-soft is the contract (defaults, no abort).
        Settings settings("selftest", true);
        CheckEqStr(r, "settings_degraded_default", "fallback",
                   settings.GetString("no_such_key", "fallback"));
        settings.SetString("str_key", "hello nvs");  // ignored: storage down
        CheckEqStr(r, "settings_degraded_write_ignored", "fallback",
                   settings.GetString("str_key", "fallback"));
        CheckTrue(r, "settings_degraded_getbool_default", !settings.GetBool("flag", false));
        return;
    }
    // Round-trip through the real NVS ("selftest" namespace).
    {
        Settings settings("selftest", true);
        settings.SetString("str_key", "hello nvs");
        CheckEqStr(r, "settings_roundtrip", "hello nvs", settings.GetString("str_key"));
        settings.SetString("str_key", "");
        CheckEqStr(r, "settings_roundtrip_empty", "", settings.GetString("str_key"));
    }
    // Missing key -> the caller's default (the getter never aborts).
    {
        Settings settings("selftest", false);
        CheckEqStr(r, "settings_missing_default", "fallback",
                   settings.GetString("no_such_key", "fallback"));
    }
}

// ---- Section F: NVS entry enumeration (migration mechanism) ----

// The one-shot encryption migration snapshots the partition via
// nvs_entry_find; this exercises the same enumeration on the live partition.
void TestNvsEnumeration(TestResult& r) {
    if (!NvsStorageUsable()) {
        // Degraded boot: enumeration over unavailable storage must fail soft.
        nvs_iterator_t it = nullptr;
        const esp_err_t err = nvs_entry_find(NVS_DEFAULT_PART_NAME, nullptr, NVS_TYPE_ANY, &it);
        CheckTrue(r, "nvs_enumeration_failsoft", err != ESP_OK && it == nullptr);
        return;
    }
    Settings settings("selftest", true);
    settings.SetString("str_key", "enum probe");
    bool found = false;
    nvs_type_t found_type = NVS_TYPE_BLOB;  // anything != STR
    nvs_iterator_t it = nullptr;
    esp_err_t next = nvs_entry_find(NVS_DEFAULT_PART_NAME, nullptr, NVS_TYPE_ANY, &it);
    while (next == ESP_OK) {
        nvs_entry_info_t info = {};  // namespace_name may be left unset
        nvs_entry_info(it, &info);
        if (std::string(info.namespace_name) == "selftest" && std::string(info.key) == "str_key") {
            found = true;
            found_type = info.type;
        }
        next = nvs_entry_next(&it);
    }
    nvs_release_iterator(it);  // NULL when the iterator exhausted itself
    // Read back through the handle; the enumerated entry must agree.
    const std::string value = settings.GetString("str_key");
    CheckTrue(r, "nvs_enumeration_roundtrip",
              found && found_type == NVS_TYPE_STR && value == "enum probe");
    settings.SetString("str_key", "");
}

}  // namespace

bool RunSelfTest() {
    TestResult r;
    TestLogBufferReassembly(r);
    TestLogBufferEviction(r);
    TestLogScrub(r);
    TestParseLogLine(r);
    TestComputeCpuUsage(r);
    TestSettingsStrings(r);
    TestNvsEnumeration(r);
    if (r.passed == r.total) {
        ESP_LOGI(TAG, "SYS SELFTEST PASS (%d checks)", r.total);
        return true;
    }
    ESP_LOGE(TAG, "SYS SELFTEST FAIL (%d/%d)", r.passed, r.total);
    return false;
}

}  // namespace sys
