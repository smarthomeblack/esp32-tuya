#include "hub_services/discovery_test.h"

#include <esp_log.h>

#include <string>
#include <vector>

#include "discovery_internal.h"
#include "hub_core/device.h"
#include "hub_core/device_registry.h"
#include "hub_core/dp_value.h"
#include "hub_services/discovery.h"

static const char* TAG = "DiscoverySelfTest";

namespace hub_core {

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

void CheckEqInt(TestResult& r, const char* name, long long expected, long long got) {
    ++r.total;
    if (expected == got) {
        ++r.passed;
        return;
    }
    ESP_LOGE(TAG, "FAIL %s: expected=%lld got=%lld", name, expected, got);
}

long long AsInt(DetailFetchStatus status) {
    return static_cast<long long>(status);
}

long long AsInt(CommitApplyResult result) {
    return static_cast<long long>(result);
}

// ---- fixtures ----

/// A scan-stage row: one live status code, no model maps yet.
Device ScanRow(const char* id) {
    Device device;
    device.id = id;
    device.name = "Scan Row";
    device.status["switch_1"] = DpValue::FromBool(true);
    return device;
}

void AddFunction(Device& device, const char* code) {
    DeviceFunction fn;
    fn.code = code;
    fn.type = "Enum";
    device.function[code] = fn;
}

void AddRange(Device& device, const char* code) {
    DeviceStatusRange range;
    range.code = code;
    range.type = "Integer";
    device.status_range[code] = range;
}

DiscoveryStaging OneCandidateStaging() {
    DiscoveryStaging staging;
    staging.done = true;
    staging.generation = 7;
    staging.candidates.push_back(ScanRow("dev-a"));
    return staging;
}

// Test sections

/// Degraded (failed calls) publishes the partial model but never caches
/// the id; a later fetch retries and replaces the staged model.
void TestDegradedPublish(TestResult& r) {
    DiscoveryStaging staging = OneCandidateStaging();

    Device partial = ScanRow("dev-a");
    AddFunction(partial, "switch_1");
    const DetailFetchStatus st = PublishDetailResult(staging, partial, 7, 1, false);

    CheckEqInt(r, "degraded.status", AsInt(DetailFetchStatus::kDegraded), AsInt(st));
    CheckEqInt(r, "degraded.not_cached", 0,
               static_cast<long long>(staging.detail_loaded.count("dev-a")));
    CheckTrue(r, "degraded.partial_published",
              staging.candidates[0].function.count("switch_1") == 1);

    // A later degraded re-fetch replaces the staged partial model.
    Device partial2 = ScanRow("dev-a");
    AddRange(partial2, "bright_value");
    const DetailFetchStatus st2 = PublishDetailResult(staging, partial2, 7, 3, false);

    CheckEqInt(r, "degraded.retry_status", AsInt(DetailFetchStatus::kDegraded), AsInt(st2));
    CheckEqInt(r, "degraded.retry_not_cached", 0,
               static_cast<long long>(staging.detail_loaded.count("dev-a")));
    CheckTrue(r, "degraded.retry_replaces",
              staging.candidates[0].status_range.count("bright_value") == 1 &&
                  staging.candidates[0].function.count("switch_1") == 0);

    // Dead auth also leaves the id uncached (retry stays possible).
    Device partial3 = ScanRow("dev-a");
    AddRange(partial3, "work_mode");
    const DetailFetchStatus st3 = PublishDetailResult(staging, partial3, 7, 0, true);

    CheckEqInt(r, "authdead.status", AsInt(DetailFetchStatus::kDegraded), AsInt(st3));
    CheckEqInt(r, "authdead.not_cached", 0,
               static_cast<long long>(staging.detail_loaded.count("dev-a")));
    CheckTrue(r, "authdead.partial_published",
              staging.candidates[0].status_range.count("work_mode") == 1);
}

/// A fully successful fetch publishes the model and caches the id; a
/// stale generation (or active/undone staging) publishes nothing.
void TestSuccessfulPublish(TestResult& r) {
    DiscoveryStaging staging = OneCandidateStaging();

    Device full = ScanRow("dev-a");
    AddFunction(full, "switch_1");
    AddRange(full, "bright_value");
    const DetailFetchStatus st = PublishDetailResult(staging, full, 7, 0, false);

    CheckEqInt(r, "success.status", AsInt(DetailFetchStatus::kLoaded), AsInt(st));
    CheckEqInt(r, "success.cached", 1,
               static_cast<long long>(staging.detail_loaded.count("dev-a")));
    CheckTrue(r, "success.model_published",
              staging.candidates[0].function.count("switch_1") == 1 &&
                  staging.candidates[0].status_range.count("bright_value") == 1);

    // A stale generation discards the result — nothing is published.
    Device stale = ScanRow("dev-a");
    AddFunction(stale, "work_mode");
    const DetailFetchStatus st2 = PublishDetailResult(staging, stale, 8, 0, false);

    CheckEqInt(r, "stalegen.status", AsInt(DetailFetchStatus::kScanActive), AsInt(st2));
    CheckEqInt(r, "stalegen.row_untouched", 0,
               static_cast<long long>(staging.candidates[0].function.count("work_mode")));
    CheckEqInt(r, "stalegen.cache_untouched", 1,
               static_cast<long long>(staging.detail_loaded.count("dev-a")));

    Device again = ScanRow("dev-a");
    AddFunction(again, "work_mode");
    staging.active = true;
    CheckEqInt(r, "activegen.status", AsInt(DetailFetchStatus::kScanActive),
               AsInt(PublishDetailResult(staging, again, 7, 0, false)));
    staging.active = false;
    staging.done = false;
    CheckEqInt(r, "undonegen.status", AsInt(DetailFetchStatus::kScanActive),
               AsInt(PublishDetailResult(staging, again, 7, 0, false)));
}

/// Fresh commit: set_up forced on, disabled = codes - enabled_entities.
void TestCommitAdd(TestResult& r) {
    DeviceRegistry& registry = DeviceRegistry::GetInstance();

    Device candidate;
    candidate.id = "selftest-disc-add";
    candidate.name = "Fresh Add";
    candidate.status["switch_1"] = DpValue::FromBool(true);
    AddRange(candidate, "bright_value");
    AddFunction(candidate, "work_mode");

    DiscoveryCommitEntry entry;
    entry.id = "selftest-disc-add";
    entry.enabled_entities.insert("switch_1");
    entry.enabled_entities.insert("bright_value");

    const CommitApplyResult result = ApplyCommitEntry(registry, candidate, entry,
                                                      /*detail_loaded=*/false);

    Device stored;
    CheckEqInt(r, "add.result", AsInt(CommitApplyResult::kAdded), AsInt(result));
    CheckTrue(r, "add.stored", registry.GetDevice("selftest-disc-add", stored));
    CheckTrue(r, "add.set_up", stored.set_up);
    CheckTrue(r, "add.work_mode_disabled", stored.disabled_entities.count("work_mode") == 1);
    CheckTrue(r, "add.enabled_live",
              stored.disabled_entities.count("switch_1") == 0 &&
                  stored.disabled_entities.count("bright_value") == 0);
    CheckTrue(r, "add.maps_kept",
              stored.function.count("work_mode") == 1 &&
                  stored.status_range.count("bright_value") == 1 &&
                  stored.status.count("switch_1") == 1);
    CheckTrue(r, "add.cleanup", registry.RemoveDevice("selftest-disc-add"));
}

/// A strictly-richer staged model refreshes an existing device: model
/// maps replaced, identity/runtime preserved, disabled choices merged.
void TestCommitRefresh(TestResult& r) {
    DeviceRegistry& registry = DeviceRegistry::GetInstance();

    Device existing;
    existing.id = "selftest-disc-refresh";
    existing.name = "Kept Name";
    existing.home_id = "home-9";
    existing.online = true;
    existing.set_up = true;
    existing.support_local = true;
    existing.status["switch_1"] = DpValue::FromBool(false);
    existing.dp_timestamps["switch_1"] = 12345;
    existing.disabled_entities.insert("switch_1");
    AddFunction(existing, "switch_1");
    registry.UpsertDevice(existing);

    // Staged: strictly more entity codes (3 vs 1) spread across the maps.
    Device candidate;
    candidate.id = "selftest-disc-refresh";
    candidate.name = "Staged Name";
    candidate.online = false;
    candidate.support_local = true;
    candidate.status["switch_1"] = DpValue::FromBool(true);
    AddRange(candidate, "bright_value");
    AddFunction(candidate, "work_mode");
    StrategyConfig strategy;
    strategy.dp_id = 1;
    strategy.status_code = "switch_1";
    candidate.local_strategy[1] = strategy;

    DiscoveryCommitEntry entry;
    entry.id = "selftest-disc-refresh";
    entry.enabled_entities.insert("switch_1");
    entry.enabled_entities.insert("bright_value");
    entry.enabled_entities.insert("work_mode");

    const CommitApplyResult result = ApplyCommitEntry(registry, candidate, entry,
                                                      /*detail_loaded=*/true);

    Device stored;
    CheckEqInt(r, "refresh.result", AsInt(CommitApplyResult::kRefreshed), AsInt(result));
    CheckTrue(r, "refresh.stored", registry.GetDevice("selftest-disc-refresh", stored));
    CheckTrue(r, "refresh.identity_kept", stored.name == "Kept Name" && stored.home_id == "home-9");
    CheckTrue(r, "refresh.runtime_kept", stored.online && stored.set_up && stored.support_local);
    CheckTrue(r, "refresh.timestamps_kept",
              stored.dp_timestamps.count("switch_1") == 1 &&
                  stored.dp_timestamps["switch_1"] == 12345);
    CheckTrue(r, "refresh.model_replaced",
              stored.function.count("work_mode") == 1 && stored.function.count("switch_1") == 0 &&
                  stored.status_range.count("bright_value") == 1);
    CheckTrue(r, "refresh.status_replaced", stored.status["switch_1"].b);
    CheckTrue(r, "refresh.strategy_replaced", stored.local_strategy.count(1) == 1);
    CheckTrue(r, "refresh.existing_choice_kept", stored.disabled_entities.count("switch_1") == 1);
    CheckTrue(r, "refresh.new_codes_enabled",
              stored.disabled_entities.count("bright_value") == 0 &&
                  stored.disabled_entities.count("work_mode") == 0);
    CheckEqInt(r, "refresh.disabled_count", 1,
               static_cast<long long>(stored.disabled_entities.size()));
    CheckTrue(r, "refresh.cleanup", registry.RemoveDevice("selftest-disc-refresh"));
}

/// Staged codes without an existing choice follow the entry selection:
/// an empty enabled_entities disables every uncovered code.
void TestCommitNewCodeSelection(TestResult& r) {
    DeviceRegistry& registry = DeviceRegistry::GetInstance();

    Device existing;
    existing.id = "selftest-disc-sel";
    existing.set_up = true;
    existing.disabled_entities.insert("switch_1");
    AddFunction(existing, "switch_1");
    registry.UpsertDevice(existing);

    Device candidate;
    candidate.id = "selftest-disc-sel";
    candidate.status["switch_1"] = DpValue::FromBool(true);
    AddRange(candidate, "bright_value");
    AddFunction(candidate, "work_mode");

    DiscoveryCommitEntry entry;  // enabled_entities empty — none enabled
    entry.id = "selftest-disc-sel";

    const CommitApplyResult result = ApplyCommitEntry(registry, candidate, entry,
                                                      /*detail_loaded=*/true);

    Device stored;
    CheckEqInt(r, "sel.result", AsInt(CommitApplyResult::kRefreshed), AsInt(result));
    CheckTrue(r, "sel.stored", registry.GetDevice("selftest-disc-sel", stored));
    CheckEqInt(r, "sel.disabled_count", 3, static_cast<long long>(stored.disabled_entities.size()));
    CheckTrue(r, "sel.existing_choice_kept", stored.disabled_entities.count("switch_1") == 1);
    CheckTrue(r, "sel.uncovered_follow_entry",
              stored.disabled_entities.count("bright_value") == 1 &&
                  stored.disabled_entities.count("work_mode") == 1);
    CheckTrue(r, "sel.cleanup", registry.RemoveDevice("selftest-disc-sel"));
}

/// A model that is not strictly richer in any map keeps the idempotent
/// skip: the registry row is untouched either way.
void TestCommitSkipEqualAndRicher(TestResult& r) {
    DeviceRegistry& registry = DeviceRegistry::GetInstance();

    Device existing;
    existing.id = "selftest-disc-equal";
    AddFunction(existing, "fn_a");
    AddFunction(existing, "fn_b");
    registry.UpsertDevice(existing);

    Device candidate;
    candidate.id = "selftest-disc-equal";
    AddFunction(candidate, "fn_c");
    candidate.status["st_d"] = DpValue::FromBool(true);  // 2 codes — equal count
    DiscoveryCommitEntry entry;
    entry.id = "selftest-disc-equal";
    entry.enabled_entities.insert("fn_c");
    entry.enabled_entities.insert("st_d");

    const CommitApplyResult result = ApplyCommitEntry(registry, candidate, entry,
                                                      /*detail_loaded=*/true);
    Device stored;
    CheckEqInt(r, "equal.result", AsInt(CommitApplyResult::kSkippedExisting), AsInt(result));
    CheckTrue(r, "equal.stored", registry.GetDevice("selftest-disc-equal", stored));
    CheckTrue(r, "equal.registry_untouched",
              stored.function.count("fn_a") == 1 && stored.function.count("fn_b") == 1 &&
                  stored.function.count("fn_c") == 0 && stored.status.count("st_d") == 0);
    CheckTrue(r, "equal.cleanup", registry.RemoveDevice("selftest-disc-equal"));

    Device rich;
    rich.id = "selftest-disc-richer";
    AddFunction(rich, "f1");
    AddFunction(rich, "f2");
    AddFunction(rich, "f3");
    registry.UpsertDevice(rich);

    Device poorer;
    poorer.id = "selftest-disc-richer";
    poorer.status["s1"] = DpValue::FromBool(true);  // 1 code vs 3 — richer existing
    DiscoveryCommitEntry poor_entry;
    poor_entry.id = "selftest-disc-richer";
    poor_entry.enabled_entities.insert("s1");

    const CommitApplyResult result2 = ApplyCommitEntry(registry, poorer, poor_entry,
                                                       /*detail_loaded=*/true);
    Device stored2;
    CheckEqInt(r, "richer.result", AsInt(CommitApplyResult::kSkippedExisting), AsInt(result2));
    CheckTrue(r, "richer.stored", registry.GetDevice("selftest-disc-richer", stored2));
    CheckTrue(r, "richer.registry_untouched",
              stored2.function.size() == 3 && stored2.status.count("s1") == 0);
    CheckTrue(r, "richer.cleanup", registry.RemoveDevice("selftest-disc-richer"));
}

/// A model with MORE metadata but the same code union still refreshes: a
/// device can carry every code in `status` while its range/function/strategy
/// maps are thin.
void TestCommitEqualCountRicherMaps(TestResult& r) {
    DeviceRegistry& registry = DeviceRegistry::GetInstance();

    // Same 1 code on both sides; only the range map grows.
    Device existing;
    existing.id = "selftest-disc-eqrange";
    existing.status["switch_1"] = DpValue::FromBool(false);
    registry.UpsertDevice(existing);

    Device candidate;
    candidate.id = "selftest-disc-eqrange";
    candidate.status["switch_1"] = DpValue::FromBool(true);
    AddRange(candidate, "switch_1");

    DiscoveryCommitEntry entry;
    entry.id = "selftest-disc-eqrange";
    entry.enabled_entities.insert("switch_1");

    const CommitApplyResult ranged = ApplyCommitEntry(registry, candidate, entry,
                                                      /*detail_loaded=*/true);
    Device stored;
    const bool ranged_stored = registry.GetDevice("selftest-disc-eqrange", stored);
    CheckEqInt(r, "eqrange.result", AsInt(CommitApplyResult::kRefreshed), AsInt(ranged));
    CheckTrue(r, "eqrange.model_replaced",
              ranged_stored && stored.status_range.count("switch_1") == 1 &&
                  stored.function.count("switch_1") == 0);
    CheckTrue(r, "eqrange.status_replaced",
              ranged_stored && stored.status["switch_1"].b && stored.disabled_entities.empty());
    CheckTrue(r, "eqrange.cleanup", registry.RemoveDevice("selftest-disc-eqrange"));

    // Same 1 code on both sides; only the function map grows.
    existing = Device{};
    existing.id = "selftest-disc-eqfunc";
    existing.status["switch_1"] = DpValue::FromBool(false);
    registry.UpsertDevice(existing);

    candidate = Device{};
    candidate.id = "selftest-disc-eqfunc";
    candidate.status["switch_1"] = DpValue::FromBool(true);
    AddFunction(candidate, "switch_1");

    DiscoveryCommitEntry func_entry;
    func_entry.id = "selftest-disc-eqfunc";
    func_entry.enabled_entities.insert("switch_1");

    const CommitApplyResult funcs =
        ApplyCommitEntry(registry, candidate, func_entry, /*detail_loaded=*/true);
    CheckEqInt(r, "eqfunc.result", AsInt(CommitApplyResult::kRefreshed), AsInt(funcs));
    CheckTrue(r, "eqfunc.model_replaced",
              registry.GetDevice("selftest-disc-eqfunc", stored) &&
                  stored.function.count("switch_1") == 1 &&
                  stored.status_range.count("switch_1") == 0);
    CheckTrue(r, "eqfunc.cleanup", registry.RemoveDevice("selftest-disc-eqfunc"));

    // Same 1 code on both sides; only the local strategy grows.
    existing = Device{};
    existing.id = "selftest-disc-eqstrat";
    existing.status["switch_1"] = DpValue::FromBool(false);
    AddFunction(existing, "switch_1");
    registry.UpsertDevice(existing);

    candidate = Device{};
    candidate.id = "selftest-disc-eqstrat";
    candidate.status["switch_1"] = DpValue::FromBool(true);
    AddFunction(candidate, "switch_1");
    candidate.support_local = true;
    StrategyConfig strategy;
    strategy.dp_id = 1;
    strategy.status_code = "switch_1";
    candidate.local_strategy[1] = strategy;

    DiscoveryCommitEntry strat_entry;
    strat_entry.id = "selftest-disc-eqstrat";
    strat_entry.enabled_entities.insert("switch_1");

    const CommitApplyResult strat =
        ApplyCommitEntry(registry, candidate, strat_entry, /*detail_loaded=*/true);
    CheckEqInt(r, "eqstrat.result", AsInt(CommitApplyResult::kRefreshed), AsInt(strat));
    CheckTrue(r, "eqstrat.strategy_added",
              registry.GetDevice("selftest-disc-eqstrat", stored) && stored.support_local &&
                  stored.local_strategy.count(1) == 1);
    CheckTrue(r, "eqstrat.cleanup", registry.RemoveDevice("selftest-disc-eqstrat"));
}

/// A refresh never shrinks a map: an equal code union with a richer range
/// still skips when `status` or the strategy would lose entries.
void TestCommitNoShrinkGate(TestResult& r) {
    DeviceRegistry& registry = DeviceRegistry::GetInstance();

    Device existing;
    existing.id = "selftest-disc-noshrink";
    existing.support_local = true;
    existing.status["switch_1"] = DpValue::FromBool(false);
    existing.status["switch_2"] = DpValue::FromBool(false);
    StrategyConfig s1;
    s1.dp_id = 1;
    s1.status_code = "switch_1";
    StrategyConfig s2;
    s2.dp_id = 2;
    s2.status_code = "switch_2";
    existing.local_strategy[1] = s1;
    existing.local_strategy[2] = s2;
    registry.UpsertDevice(existing);

    // Equal code union (2 = 2): the range map grows while `status` and the
    // strategy shrink — a refresh would drop live values / local routing.
    Device shrinking;
    shrinking.id = "selftest-disc-noshrink";
    shrinking.support_local = true;
    shrinking.status["switch_1"] = DpValue::FromBool(true);
    AddRange(shrinking, "switch_2");
    StrategyConfig s3;
    s3.dp_id = 3;
    s3.status_code = "switch_2";
    shrinking.local_strategy[3] = s3;

    DiscoveryCommitEntry entry;
    entry.id = "selftest-disc-noshrink";
    entry.enabled_entities.insert("switch_1");
    entry.enabled_entities.insert("switch_2");

    const CommitApplyResult skipped =
        ApplyCommitEntry(registry, shrinking, entry, /*detail_loaded=*/true);
    Device stored;
    CheckEqInt(r, "noshrink.result", AsInt(CommitApplyResult::kSkippedExisting), AsInt(skipped));
    CheckTrue(r, "noshrink.stored", registry.GetDevice("selftest-disc-noshrink", stored));
    CheckTrue(r, "noshrink.status_kept",
              stored.status.size() == 2 && stored.status.count("switch_1") == 1 &&
                  stored.status.count("switch_2") == 1 && !stored.status["switch_1"].b &&
                  !stored.status["switch_2"].b);
    CheckTrue(r, "noshrink.strategy_kept",
              stored.support_local && stored.local_strategy.size() == 2 &&
                  stored.local_strategy.count(1) == 1 && stored.local_strategy.count(2) == 1 &&
                  stored.local_strategy.count(3) == 0);
    CheckTrue(r, "noshrink.range_kept", stored.status_range.count("switch_2") == 0);

    // Positive control: every map is at least as rich — the refresh applies.
    Device grown;
    grown.id = "selftest-disc-noshrink";
    grown.support_local = true;
    grown.status["switch_1"] = DpValue::FromBool(true);
    grown.status["switch_2"] = DpValue::FromBool(true);
    AddRange(grown, "switch_2");
    StrategyConfig g1;
    g1.dp_id = 1;
    g1.status_code = "switch_1";
    StrategyConfig g2;
    g2.dp_id = 2;
    g2.status_code = "switch_2";
    StrategyConfig g3;
    g3.dp_id = 3;
    g3.status_code = "switch_2";
    grown.local_strategy[1] = g1;
    grown.local_strategy[2] = g2;
    grown.local_strategy[3] = g3;

    const CommitApplyResult refreshed =
        ApplyCommitEntry(registry, grown, entry, /*detail_loaded=*/true);
    CheckEqInt(r, "noshrink.control_result", AsInt(CommitApplyResult::kRefreshed),
               AsInt(refreshed));
    CheckTrue(r, "noshrink.control_maps",
              registry.GetDevice("selftest-disc-noshrink", stored) &&
                  stored.status_range.count("switch_2") == 1 && stored.status["switch_1"].b &&
                  stored.status["switch_2"].b && stored.local_strategy.size() == 3 &&
                  stored.local_strategy.count(3) == 1);
    CheckTrue(r, "noshrink.cleanup", registry.RemoveDevice("selftest-disc-noshrink"));
}

/// Degraded (detail not loaded) richer candidates never touch the registry;
/// a loaded one refreshes, and an unknown strategy never erases a known one.
void TestCommitDetailGate(TestResult& r) {
    DeviceRegistry& registry = DeviceRegistry::GetInstance();

    Device existing;
    existing.id = "selftest-disc-gate";
    existing.support_local = true;
    existing.status["switch_1"] = DpValue::FromBool(false);
    AddFunction(existing, "existing_fn");
    StrategyConfig existing_strategy;
    existing_strategy.dp_id = 1;
    existing_strategy.status_code = "switch_1";
    existing.local_strategy[1] = existing_strategy;
    registry.UpsertDevice(existing);

    Device candidate;
    candidate.id = "selftest-disc-gate";
    candidate.support_local = true;
    candidate.status["switch_1"] = DpValue::FromBool(true);
    AddRange(candidate, "bright_value");
    AddFunction(candidate, "work_mode");
    StrategyConfig candidate_strategy;
    candidate_strategy.dp_id = 2;
    candidate_strategy.status_code = "bright_value";
    candidate.local_strategy[2] = candidate_strategy;

    DiscoveryCommitEntry entry;
    entry.id = "selftest-disc-gate";
    entry.enabled_entities.insert("switch_1");
    entry.enabled_entities.insert("bright_value");
    entry.enabled_entities.insert("work_mode");

    // Strictly more codes, but the detail fetch was degraded -> skip.
    const CommitApplyResult degraded = ApplyCommitEntry(registry, candidate, entry,
                                                        /*detail_loaded=*/false);
    Device stored;
    CheckEqInt(r, "gate.degraded.result", AsInt(CommitApplyResult::kSkippedExisting),
               AsInt(degraded));
    CheckTrue(r, "gate.degraded.stored", registry.GetDevice("selftest-disc-gate", stored));
    CheckTrue(r, "gate.degraded.maps_untouched",
              stored.function.count("existing_fn") == 1 &&
                  stored.function.count("work_mode") == 0 &&
                  stored.status_range.count("bright_value") == 0 && !stored.status["switch_1"].b);
    CheckTrue(r, "gate.degraded.strategy_untouched",
              stored.support_local && stored.local_strategy.count(1) == 1 &&
                  stored.local_strategy.count(2) == 0);

    // The same candidate with its detail loaded refreshes.
    const CommitApplyResult loaded = ApplyCommitEntry(registry, candidate, entry,
                                                      /*detail_loaded=*/true);
    CheckEqInt(r, "gate.loaded.result", AsInt(CommitApplyResult::kRefreshed), AsInt(loaded));
    CheckTrue(r, "gate.loaded.stored", registry.GetDevice("selftest-disc-gate", stored));
    CheckTrue(r, "gate.loaded.maps_replaced",
              stored.function.count("work_mode") == 1 &&
                  stored.function.count("existing_fn") == 0 &&
                  stored.status_range.count("bright_value") == 1 && stored.status["switch_1"].b);
    CheckTrue(r, "gate.loaded.strategy_replaced",
              stored.support_local && stored.local_strategy.count(2) == 1 &&
                  stored.local_strategy.count(1) == 0);
    CheckTrue(r, "gate.cleanup", registry.RemoveDevice("selftest-disc-gate"));

    // A loaded candidate that does not know the strategy keeps the known one.
    Device keep_existing;
    keep_existing.id = "selftest-disc-keep";
    keep_existing.support_local = true;
    keep_existing.status["switch_1"] = DpValue::FromBool(false);
    StrategyConfig keep_strategy;
    keep_strategy.dp_id = 7;
    keep_strategy.status_code = "switch_1";
    keep_existing.local_strategy[7] = keep_strategy;
    registry.UpsertDevice(keep_existing);

    Device no_strategy;
    no_strategy.id = "selftest-disc-keep";
    no_strategy.support_local = false;
    no_strategy.status["switch_1"] = DpValue::FromBool(true);
    AddRange(no_strategy, "bright_value");
    AddFunction(no_strategy, "work_mode");

    DiscoveryCommitEntry keep_entry;
    keep_entry.id = "selftest-disc-keep";
    keep_entry.enabled_entities.insert("switch_1");
    keep_entry.enabled_entities.insert("bright_value");
    keep_entry.enabled_entities.insert("work_mode");

    const CommitApplyResult keep_result =
        ApplyCommitEntry(registry, no_strategy, keep_entry, /*detail_loaded=*/true);
    CheckEqInt(r, "keep.result", AsInt(CommitApplyResult::kRefreshed), AsInt(keep_result));
    CheckTrue(r, "keep.stored", registry.GetDevice("selftest-disc-keep", stored));
    CheckTrue(r, "keep.maps_replaced",
              stored.function.count("work_mode") == 1 &&
                  stored.status_range.count("bright_value") == 1 && stored.status["switch_1"].b);
    CheckTrue(r, "keep.strategy_kept",
              stored.support_local && stored.local_strategy.count(7) == 1 &&
                  stored.local_strategy[7].status_code == "switch_1");
    CheckTrue(r, "keep.cleanup", registry.RemoveDevice("selftest-disc-keep"));
}

/// A support_local=false candidate with a non-empty strategy unions new
/// dpIds into the stored map; existing entries keep their old values.
void TestCommitUnionMergeStrategy(TestResult& r) {
    DeviceRegistry& registry = DeviceRegistry::GetInstance();

    Device existing;
    existing.id = "selftest-disc-union";
    existing.status["switch_led"] = DpValue::FromBool(true);
    StrategyConfig old_cfg;
    old_cfg.dp_id = 1;
    old_cfg.status_code = "switch_led";
    old_cfg.use_open_api = true;
    existing.local_strategy[1] = old_cfg;
    registry.UpsertDevice(existing);

    Device candidate;
    candidate.id = "selftest-disc-union";
    candidate.status["switch_led"] = DpValue::FromBool(false);
    AddFunction(candidate, "countdown");
    StrategyConfig conflict;
    conflict.dp_id = 1;
    conflict.status_code = "bright_value";
    conflict.use_open_api = false;
    conflict.property_update = true;
    StrategyConfig added;
    added.dp_id = 7;
    added.status_code = "countdown";
    added.use_open_api = true;
    added.property_update = true;
    candidate.local_strategy[1] = conflict;
    candidate.local_strategy[7] = added;

    DiscoveryCommitEntry entry;
    entry.id = "selftest-disc-union";
    entry.enabled_entities.insert("switch_led");
    entry.enabled_entities.insert("countdown");

    const CommitApplyResult result = ApplyCommitEntry(registry, candidate, entry,
                                                      /*detail_loaded=*/true);
    Device stored;
    CheckEqInt(r, "union.result", AsInt(CommitApplyResult::kRefreshed), AsInt(result));
    CheckTrue(
        r, "union.existing_entry_kept",
        registry.GetDevice("selftest-disc-union", stored) && stored.local_strategy.size() == 2 &&
            !stored.support_local && stored.local_strategy[1].status_code == "switch_led" &&
            stored.local_strategy[1].use_open_api && !stored.local_strategy[1].property_update);
    CheckTrue(r, "union.new_entry_merged",
              stored.local_strategy.count(7) == 1 &&
                  stored.local_strategy[7].status_code == "countdown" &&
                  stored.local_strategy[7].use_open_api && stored.function.count("countdown") == 1);
    CheckTrue(r, "union.cleanup", registry.RemoveDevice("selftest-disc-union"));
}

/// A refresh back-fills a missing home_id from the scanned candidate.
void TestCommitRefreshBackfillsHomeId(TestResult& r) {
    DeviceRegistry& registry = DeviceRegistry::GetInstance();

    Device existing;
    existing.id = "selftest-disc-homefill";
    existing.status["switch_1"] = DpValue::FromBool(false);
    registry.UpsertDevice(existing);

    Device candidate;
    candidate.id = "selftest-disc-homefill";
    candidate.home_id = "home-7";
    candidate.status["switch_1"] = DpValue::FromBool(true);
    AddRange(candidate, "bright_value");
    AddFunction(candidate, "work_mode");

    DiscoveryCommitEntry entry;
    entry.id = "selftest-disc-homefill";
    entry.enabled_entities.insert("switch_1");
    entry.enabled_entities.insert("bright_value");
    entry.enabled_entities.insert("work_mode");

    const CommitApplyResult result = ApplyCommitEntry(registry, candidate, entry,
                                                      /*detail_loaded=*/true);
    Device stored;
    const bool got = registry.GetDevice("selftest-disc-homefill", stored);
    const bool removed = registry.RemoveDevice("selftest-disc-homefill");
    CheckTrue(r, "homefill.backfilled",
              AsInt(CommitApplyResult::kRefreshed) == AsInt(result) && got &&
                  stored.home_id == "home-7" && removed);
}

/// Service guards without a scan: kNoScan and the zero-commit.
void TestServiceNoScan(TestResult& r) {
    DiscoveryService& service = DiscoveryService::GetInstance();
    CheckEqInt(r, "service.no_scan", AsInt(DetailFetchStatus::kNoScan),
               AsInt(service.FetchCandidateDetail("dev-x")));
    const CommitSelectionResult empty = service.CommitSelection({});
    CheckTrue(r, "service.empty_commit",
              !empty.scan_active && empty.added == 0 && empty.refreshed == 0 && empty.skipped == 0);
    std::vector<DiscoveryCommitEntry> entries;
    DiscoveryCommitEntry entry;
    entry.id = "dev-x";
    entries.push_back(entry);
    const CommitSelectionResult no_scan = service.CommitSelection(entries);
    CheckTrue(r, "service.no_scan_commit",
              !no_scan.scan_active && no_scan.added == 0 && no_scan.refreshed == 0 &&
                  no_scan.skipped == 0);
}

}  // namespace

bool RunDiscoverySelfTest() {
    TestResult r;
    TestDegradedPublish(r);
    TestSuccessfulPublish(r);
    TestCommitAdd(r);
    TestCommitRefresh(r);
    TestCommitNewCodeSelection(r);
    TestCommitSkipEqualAndRicher(r);
    TestCommitEqualCountRicherMaps(r);
    TestCommitNoShrinkGate(r);
    TestCommitDetailGate(r);
    TestCommitUnionMergeStrategy(r);
    TestCommitRefreshBackfillsHomeId(r);
    TestServiceNoScan(r);

    if (r.passed == r.total) {
        ESP_LOGI(TAG, "DISCOVERY SELFTEST PASS (%d checks)", r.total);
        return true;
    }
    ESP_LOGE(TAG, "DISCOVERY SELFTEST FAIL (%d/%d)", r.passed, r.total);
    return false;
}

}  // namespace hub_core
