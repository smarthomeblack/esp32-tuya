#include "hub_services/command_service_test.h"
#include "hub_services/command_service.h"

#include <cJSON.h>
#include <esp_log.h>

#include <cmath>
#include <cstdint>
#include <set>
#include <string>
#include <vector>

#include "dedup_filter.h"
#include "hub_core/device.h"
#include "hub_core/device_registry.h"
#include "hub_core/dp_value.h"
#include "openapi_command_internal.h"
#include "tuya_cloud/paths.h"

static const char* TAG = "CommandSelfTest";

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

void CheckEqStr(TestResult& r, const char* name, const std::string& expected,
                const std::string& got) {
    ++r.total;
    if (expected == got) {
        ++r.passed;
        return;
    }
    ESP_LOGE(TAG, "FAIL %s: expected=\"%s\" got=\"%s\"", name, expected.c_str(), got.c_str());
}

void CheckEqInt(TestResult& r, const char* name, long long expected, long long got) {
    ++r.total;
    if (expected == got) {
        ++r.passed;
        return;
    }
    ESP_LOGE(TAG, "FAIL %s: expected=%lld got=%lld", name, expected, got);
}

/// RAII cJSON tree (guarded free, no exceptions).
class JsonPtr {
public:
    explicit JsonPtr(const char* text) : root_(cJSON_Parse(text)) {}
    ~JsonPtr() { cJSON_Delete(root_); }
    JsonPtr(const JsonPtr&) = delete;
    JsonPtr& operator=(const JsonPtr&) = delete;

    cJSON* root() const { return root_; }

private:
    cJSON* root_;
};

// Command vector builders (factory form avoids ambiguous ctor overloads).

std::vector<DeviceCommand> SwitchLed(bool on) {
    return {DeviceCommand{"switch_led", DpValue::FromBool(on)}};
}

std::vector<DeviceCommand> WorkModeWhite() {
    return {DeviceCommand{"work_mode", DpValue::FromString("white")}};
}

// Golden fragments — shaped like the real SDK responses.

/// Scene-list golden: full, disabled, defaults-only, non-bool enabled,
/// and three unusable scene_id rows (skipped).
constexpr const char* kScenesJson =
    "["
    "{\"scene_id\": \"S1\", \"name\": \"Movie Night\", \"enabled\": true,"
    " \"actions\": [{\"entity\": \"light\", \"num\": 1}]},"
    "{\"scene_id\": \"S2\", \"name\": \"Morning\", \"enabled\": false, \"actions\": []},"
    "{\"scene_id\": \"S3\"},"
    "{\"scene_id\": \"S4\", \"name\": \"Enabled as string\", \"enabled\": \"yes\"},"
    "{\"scene_id\": 42, \"name\": \"Numeric scene id\"},"
    "{\"name\": \"Missing scene id\"},"
    "{\"scene_id\": \"\", \"name\": \"Empty scene id\"}"
    "]";

/// A second home's scene list (the accumulation check).
constexpr const char* kScenesMoreJson =
    "[{\"scene_id\": \"T1\", \"name\": \"Away\", \"enabled\": true, \"actions\": []}]";

// Test sections

void TestDpValueEquality(TestResult& r) {
    // Same-type equality.
    CheckTrue(r, "dp.eq.bool", DpValue::FromBool(true) == DpValue::FromBool(true));
    CheckTrue(r, "dp.eq.int", DpValue::FromInt(42) == DpValue::FromInt(42));
    CheckTrue(r, "dp.eq.double", DpValue::FromDouble(2.5) == DpValue::FromDouble(2.5));
    CheckTrue(r, "dp.eq.string", DpValue::FromString("white") == DpValue::FromString("white"));
    CheckTrue(r, "dp.eq.false.zero", DpValue::FromBool(false) == DpValue::FromBool(false));

    // Same-type inequality.
    CheckTrue(r, "dp.ne.bool", !(DpValue::FromBool(true) == DpValue::FromBool(false)));
    CheckTrue(r, "dp.ne.int", !(DpValue::FromInt(1) == DpValue::FromInt(2)));
    CheckTrue(r, "dp.ne.double", !(DpValue::FromDouble(2.5) == DpValue::FromDouble(2.6)));
    CheckTrue(r, "dp.ne.string", !(DpValue::FromString("white") == DpValue::FromString("colour")));

    // Cross-type: type-STRICT (Python would equate True == 1 and 1 == 1.0).
    CheckTrue(r, "dp.ne.bool.int", !(DpValue::FromBool(true) == DpValue::FromInt(1)));
    CheckTrue(r, "dp.ne.int.double", !(DpValue::FromInt(1) == DpValue::FromDouble(1.0)));
    // A string never equals a number.
    CheckTrue(r, "dp.ne.string.int", !(DpValue::FromString("1") == DpValue::FromInt(1)));

    // NaN != NaN (a NaN command is never deduplicated).
    CheckTrue(r, "dp.nan.ne",
              !(DpValue::FromDouble(std::nan("")) == DpValue::FromDouble(std::nan(""))));

    // IsEmpty: the null-ish form; false and 0 are REAL values.
    CheckTrue(r, "dp.isempty.default", DpValue().IsEmpty());
    CheckTrue(r, "dp.isempty.emptystring", DpValue::FromString("").IsEmpty());
    CheckTrue(r, "dp.notempty.zero",
              !DpValue::FromInt(0).IsEmpty() && !DpValue::FromBool(false).IsEmpty());

    // DeviceCommand deep equality (code + value).
    CheckTrue(r, "cmd.eq",
              DeviceCommand{"switch_led", DpValue::FromBool(true)} ==
                  DeviceCommand{"switch_led", DpValue::FromBool(true)});
    CheckTrue(r, "cmd.ne.value",
              !(DeviceCommand{"switch_led", DpValue::FromBool(true)} ==
                DeviceCommand{"switch_led", DpValue::FromBool(false)}));
    CheckTrue(r, "cmd.ne.code",
              !(DeviceCommand{"switch_led", DpValue::FromBool(true)} ==
                DeviceCommand{"work_mode", DpValue::FromBool(true)}));
}

void TestDedupFilter(TestResult& r) {
    // Fake wall-clock base in ms (epoch-ish, so the first call sweeps).
    constexpr int64_t t0 = 1000000;

    // --- one timeline: window boundary + update-on-allow ---
    {
        DedupFilter f;
        CheckTrue(r, "dedup.first.allows", f.Allows("dev1", SwitchLed(true), t0));
        CheckTrue(r, "dedup.same.within.blocks", !f.Allows("dev1", SwitchLed(true), t0 + 5000));
        CheckTrue(r, "dedup.same.9999.blocks", !f.Allows("dev1", SwitchLed(true), t0 + 9999));
        CheckTrue(r, "dedup.same.10000.allows", f.Allows("dev1", SwitchLed(true), t0 + 10000));
        // The allow at t0+10000 refreshed the entry.
        CheckTrue(r, "dedup.allow.updates.entry", !f.Allows("dev1", SwitchLed(true), t0 + 15000));
    }

    // --- a BLOCKED call must not refresh the timer ---
    {
        DedupFilter f;
        CheckTrue(r, "dedup.blocked.setup", f.Allows("d", SwitchLed(true), t0));
        CheckTrue(r, "dedup.blocked.block", !f.Allows("d", SwitchLed(true), t0 + 3000));
        // If the blocked call had refreshed last_ms, this would still block.
        CheckTrue(r, "dedup.blocked.no.refresh", f.Allows("d", SwitchLed(true), t0 + 10000));
    }

    // --- different commands inside the window -> allow ---
    {
        DedupFilter f;
        CheckTrue(r, "dedup.changed.setup", f.Allows("d", SwitchLed(true), t0));
        CheckTrue(r, "dedup.changed.value.allows", f.Allows("d", SwitchLed(false), t0 + 1000));
    }

    // --- type-strict deep equality (documented divergence) ---
    {
        DedupFilter f;
        CheckTrue(r, "dedup.bool.vs.int.setup",
                  f.Allows("d", {DeviceCommand{"switch_led", DpValue::FromBool(true)}}, t0));
        // Type-strict: allowed.
        CheckTrue(r, "dedup.bool.vs.int.allows",
                  f.Allows("d", {DeviceCommand{"switch_led", DpValue::FromInt(1)}}, t0 + 1000));
    }
    {
        DedupFilter f;
        CheckTrue(r, "dedup.int.vs.double.setup",
                  f.Allows("d", {DeviceCommand{"bright_value", DpValue::FromInt(1)}}, t0));
        // Type-strict: allowed.
        CheckTrue(
            r, "dedup.int.vs.double.allows",
            f.Allows("d", {DeviceCommand{"bright_value", DpValue::FromDouble(1.0)}}, t0 + 1000));
    }

    // --- list structure: element ORDER and LENGTH matter ---
    {
        DedupFilter f;
        const std::vector<DeviceCommand> ab = {
            DeviceCommand{"switch_led", DpValue::FromBool(true)},
            DeviceCommand{"bright_value", DpValue::FromInt(128)}};
        const std::vector<DeviceCommand> ba = {
            DeviceCommand{"bright_value", DpValue::FromInt(128)},
            DeviceCommand{"switch_led", DpValue::FromBool(true)}};
        CheckTrue(r, "dedup.order.setup", f.Allows("d", ab, t0));
        CheckTrue(r, "dedup.order.matters", f.Allows("d", ba, t0 + 1000));
    }
    {
        DedupFilter f;
        CheckTrue(r, "dedup.length.setup", f.Allows("d", SwitchLed(true), t0));
        CheckTrue(r, "dedup.length.matters",
                  f.Allows("d",
                           {DeviceCommand{"switch_led", DpValue::FromBool(true)},
                            DeviceCommand{"bright_value", DpValue::FromInt(128)}},
                           t0 + 1000));
    }

    // --- identical STRING commands within the window -> blocked ---
    {
        DedupFilter f;
        CheckTrue(r, "dedup.string.setup", f.Allows("d", WorkModeWhite(), t0));
        CheckTrue(r, "dedup.string.values.block", !f.Allows("d", WorkModeWhite(), t0 + 1000));
    }

    // --- per-device isolation ---
    {
        DedupFilter f;
        CheckTrue(r, "dedup.isolation.setup.a", f.Allows("devA", WorkModeWhite(), t0));
        CheckTrue(r, "dedup.isolation.block.a", !f.Allows("devA", WorkModeWhite(), t0 + 1000));
        CheckTrue(r, "dedup.device.isolation", f.Allows("devB", WorkModeWhite(), t0 + 2000));
    }

    // --- NaN commands are never deduplicated ---
    {
        DedupFilter f;
        CheckTrue(r, "dedup.nan.setup",
                  f.Allows("d", {DeviceCommand{"temp", DpValue::FromDouble(std::nan(""))}}, t0));
        CheckTrue(
            r, "dedup.nan.not.equal",
            f.Allows("d", {DeviceCommand{"temp", DpValue::FromDouble(std::nan(""))}}, t0 + 1000));
    }

    // --- Forget: a FAILED send re-arms an identical retry ---
    {
        DedupFilter f;
        CheckTrue(r, "dedup.forget.setup", f.Allows("d", SwitchLed(true), t0));
        f.Forget("d");
        CheckTrue(r, "dedup.forget.retry.allows", f.Allows("d", SwitchLed(true), t0 + 1000));
    }

    // --- no Forget (the send SUCCEEDED): the retry stays blocked ---
    {
        DedupFilter f;
        CheckTrue(r, "dedup.success.setup", f.Allows("d", SwitchLed(true), t0));
        CheckTrue(r, "dedup.success.retry.blocks", !f.Allows("d", SwitchLed(true), t0 + 1000));
    }

    // --- lazy sweep: entries >= 10 s old are dropped ---
    {
        DedupFilter f;
        CheckTrue(r, "dedup.sweep.setup.a", f.Allows("devA", SwitchLed(true), t0));
        CheckTrue(r, "dedup.sweep.setup.b", f.Allows("devB", SwitchLed(true), t0 + 20000));
        // The sweep inside the t0+20000 call removed devA (20 s old).
        CheckEqInt(r, "dedup.sweep.removes.expired", 1, static_cast<long long>(f.Size()));
    }

    // --- sweep cadence: at most once per 10 s ---
    {
        DedupFilter f;
        CheckTrue(r, "dedup.cadence.setup.a", f.Allows("devA", SwitchLed(true), t0));
        CheckTrue(r, "dedup.cadence.setup.b", f.Allows("devB", SwitchLed(true), t0 + 8000));
        CheckTrue(r, "dedup.cadence.setup.c", f.Allows("devC", SwitchLed(true), t0 + 17000));
        // No sweep at t0+8000; the t0+17000 sweep drops devA (17 s),
        // keeps devB, adds devC -> 2.
        CheckEqInt(r, "dedup.sweep.cadence", 2, static_cast<long long>(f.Size()));
    }
}

void TestBuildCommandsBody(TestResult& r) {
    std::string body;

    // Byte-exact compact JSON, insertion order code-then-value.
    CheckTrue(r, "body.bool.true",
              BuildCommandsBody({DeviceCommand{"switch_led", DpValue::FromBool(true)}}, body));
    CheckEqStr(r, "body.bool.true.json",
               "{\"commands\":[{\"code\":\"switch_led\",\"value\":true}]}", body);

    CheckTrue(r, "body.bool.false",
              BuildCommandsBody({DeviceCommand{"switch_led", DpValue::FromBool(false)}}, body));
    CheckEqStr(r, "body.bool.false.json",
               "{\"commands\":[{\"code\":\"switch_led\",\"value\":false}]}", body);

    CheckTrue(r, "body.int",
              BuildCommandsBody({DeviceCommand{"bright_value", DpValue::FromInt(128)}}, body));
    CheckEqStr(r, "body.int.json", "{\"commands\":[{\"code\":\"bright_value\",\"value\":128}]}",
               body);

    CheckTrue(r, "body.int.negative",
              BuildCommandsBody({DeviceCommand{"countdown", DpValue::FromInt(-5)}}, body));
    CheckEqStr(r, "body.int.negative.json",
               "{\"commands\":[{\"code\":\"countdown\",\"value\":-5}]}", body);

    CheckTrue(r, "body.int64",
              BuildCommandsBody({DeviceCommand{"big", DpValue::FromInt(4294967296LL)}}, body));
    CheckEqStr(r, "body.int64.json", "{\"commands\":[{\"code\":\"big\",\"value\":4294967296}]}",
               body);

    CheckTrue(r, "body.double",
              BuildCommandsBody({DeviceCommand{"temp_value", DpValue::FromDouble(2.5)}}, body));
    CheckEqStr(r, "body.double.json", "{\"commands\":[{\"code\":\"temp_value\",\"value\":2.5}]}",
               body);

    CheckTrue(r, "body.string",
              BuildCommandsBody({DeviceCommand{"work_mode", DpValue::FromString("white")}}, body));
    CheckEqStr(r, "body.string.json",
               "{\"commands\":[{\"code\":\"work_mode\",\"value\":\"white\"}]}", body);

    // String escaping mirrors json.dumps: a " b \ c -> "a\"b\\c".
    CheckTrue(r, "body.string.escape",
              BuildCommandsBody({DeviceCommand{"mode", DpValue::FromString("a\"b\\c")}}, body));
    CheckEqStr(r, "body.string.escape.json",
               "{\"commands\":[{\"code\":\"mode\",\"value\":\"a\\\"b\\\\c\"}]}", body);

    // Multiple commands keep their order.
    CheckTrue(r, "body.multi",
              BuildCommandsBody({DeviceCommand{"switch_led", DpValue::FromBool(true)},
                                 DeviceCommand{"bright_value", DpValue::FromInt(128)}},
                                body));
    CheckEqStr(r, "body.multi.json",
               "{\"commands\":[{\"code\":\"switch_led\",\"value\":true},"
               "{\"code\":\"bright_value\",\"value\":128}]}",
               body);

    // Rejections: invalid input is never serialized.
    CheckTrue(r, "body.reject.empty.list", !BuildCommandsBody({}, body));
    CheckTrue(r, "body.reject.empty.code",
              !BuildCommandsBody({DeviceCommand{"", DpValue::FromBool(true)}}, body));
    CheckTrue(r, "body.reject.null.value",
              !BuildCommandsBody({DeviceCommand{"switch_led", DpValue()}}, body));
    CheckTrue(r, "body.reject.empty.string.value",
              !BuildCommandsBody({DeviceCommand{"mode", DpValue::FromString("")}}, body));
}

void TestSceneRequests(TestResult& r) {
    // Query / trigger bodies.
    CheckEqStr(r, "scene.query.body", "{\"homeId\":\"H1\"}", tuya_cloud::HomeIdParams("H1"));
    CheckEqStr(r, "scene.trigger.body", "{\"homeId\":\"123\",\"sceneId\":\"456\"}",
               BuildSceneTriggerBody("123", "456"));

    // Scenes parse: golden fragment through the FetchScenes helper.
    std::vector<Scene> scenes;
    {
        const JsonPtr root(kScenesJson);
        CheckEqInt(r, "scenes.count", 4,
                   static_cast<long long>(ParseScenes(root.root(), "H1", scenes)));
    }
    if (scenes.size() == 4) {
        CheckEqStr(r, "scenes.0.scene_id", "S1", scenes[0].scene_id);
        CheckEqStr(r, "scenes.0.name", "Movie Night", scenes[0].name);
        CheckTrue(r, "scenes.0.enabled", scenes[0].enabled);
        CheckEqStr(r, "scenes.0.home_id", "H1", scenes[0].home_id);

        CheckEqStr(r, "scenes.1.scene_id", "S2", scenes[1].scene_id);
        CheckEqStr(r, "scenes.1.name", "Morning", scenes[1].name);
        CheckTrue(r, "scenes.1.disabled", !scenes[1].enabled);
        CheckEqStr(r, "scenes.1.home_id", "H1", scenes[1].home_id);

        // Missing name/enabled keep the struct defaults.
        CheckEqStr(r, "scenes.2.scene_id", "S3", scenes[2].scene_id);
        CheckEqStr(r, "scenes.2.name", "", scenes[2].name);
        CheckTrue(r, "scenes.2.enabled_default", !scenes[2].enabled);

        // Non-bool enabled ("yes") keeps false.
        CheckEqStr(r, "scenes.3.scene_id", "S4", scenes[3].scene_id);
        CheckTrue(r, "scenes.3.enabled_nonbool", !scenes[3].enabled);
    }

    // Per-home accumulation: the second home's list is APPENDED.
    {
        const JsonPtr root(kScenesMoreJson);
        CheckEqInt(r, "scenes.more.count", 1,
                   static_cast<long long>(ParseScenes(root.root(), "H2", scenes)));
    }
    if (scenes.size() == 5) {
        CheckEqStr(r, "scenes.4.scene_id", "T1", scenes[4].scene_id);
        CheckEqStr(r, "scenes.4.name", "Away", scenes[4].name);
        CheckTrue(r, "scenes.4.enabled", scenes[4].enabled);
        CheckEqStr(r, "scenes.4.home_id", "H2", scenes[4].home_id);
    }

    // Non-array / null / empty results append nothing.
    std::vector<Scene> junk;
    {
        const JsonPtr not_array("{\"scene_id\": \"X\"}");
        CheckEqInt(r, "scenes.nonarray", 0,
                   static_cast<long long>(ParseScenes(not_array.root(), "H1", junk)));
    }
    CheckEqInt(r, "scenes.null", 0, static_cast<long long>(ParseScenes(nullptr, "H1", junk)));
    {
        const JsonPtr empty("[]");
        CheckEqInt(r, "scenes.empty.array", 0,
                   static_cast<long long>(ParseScenes(empty.root(), "H1", junk)));
    }
    CheckEqInt(r, "scenes.junk.untouched", 0, static_cast<long long>(junk.size()));
}

// Disabled-entity rejection — drives the LIVE service (safe: the guard
// fires before the network; CustomerApi is unconfigured at self-test time).

void TestDisabledEntityRejection(TestResult& r) {
    DeviceRegistry& registry = DeviceRegistry::GetInstance();

    Device device;
    device.id = "selftest-disabled";
    device.name = "Selftest Disabled Entity Device";
    device.disabled_entities.insert("switch_1");
    registry.UpsertDevice(device);

    CommandService& service = CommandService::GetInstance();

    // The DISABLED code: kError twice — NO dedup entry was written
    // (a written entry would dedup the second call).
    CheckTrue(r, "disabled.entity.first.rejected",
              service.SendDeviceCommands(std::string(device.id),
                                         {DeviceCommand{"switch_1", DpValue::FromBool(true)}}) ==
                  CommandResult::kError);
    CheckTrue(r, "disabled.entity.second.rejected",
              service.SendDeviceCommands(std::string(device.id),
                                         {DeviceCommand{"switch_1", DpValue::FromBool(true)}}) ==
                  CommandResult::kError);

    // A failed send (sharing leg unconfigured -> kError) forgets the
    // dedup entry: the identical retry re-runs and fails again.
    CheckTrue(r, "disabled.entity.other.normal.kerror",
              service.SendDeviceCommands(std::string(device.id),
                                         {DeviceCommand{"switch_2", DpValue::FromBool(true)}}) ==
                  CommandResult::kError);
    CheckTrue(r, "disabled.entity.other.normal.retry.allowed",
              service.SendDeviceCommands(std::string(device.id),
                                         {DeviceCommand{"switch_2", DpValue::FromBool(true)}}) ==
                  CommandResult::kError);

    // The self-test device leaves the registry (its dedup entry expires).
    CheckTrue(r, "disabled.entity.cleanup", registry.RemoveDevice(device.id));
}

// OpenAPI command routing + quota (pure helpers; no network / clock / NVS).

/// Mixed local_strategy: dp1 open+issue, dp2 open+commands, dp3+dp4 sharing;
/// dp5's status_code collides with dp1 (first dp_id wins).
Device MixedStrategyDevice() {
    Device device;
    device.id = "dev-mixed";

    StrategyConfig open_issue;
    open_issue.dp_id = 1;
    open_issue.status_code = "switch_led";
    open_issue.use_open_api = true;
    open_issue.property_update = true;
    device.local_strategy.emplace(1, open_issue);

    StrategyConfig open_commands;
    open_commands.dp_id = 2;
    open_commands.status_code = "bright_value";
    open_commands.use_open_api = true;
    open_commands.property_update = false;
    device.local_strategy.emplace(2, open_commands);

    StrategyConfig sharing;
    sharing.dp_id = 3;
    sharing.status_code = "work_mode";
    sharing.use_open_api = false;
    sharing.property_update = false;
    device.local_strategy.emplace(3, sharing);

    StrategyConfig sharing_default;
    sharing_default.dp_id = 4;
    sharing_default.status_code = "countdown";
    device.local_strategy.emplace(4, sharing_default);

    // Same status_code as dp 1 — the FIRST dp_id wins the scan.
    StrategyConfig shadow_dup;
    shadow_dup.dp_id = 5;
    shadow_dup.status_code = "switch_led";
    shadow_dup.use_open_api = false;
    device.local_strategy.emplace(5, shadow_dup);

    return device;
}

void TestRouteCommands(TestResult& r) {
    const Device device = MixedStrategyDevice();

    // Mixed list: use_open_api DPs -> openapi, the rest -> sharing.
    {
        std::vector<DeviceCommand> openapi_cmds;
        std::vector<DeviceCommand> sharing_cmds;
        RouteCommands(device,
                      {DeviceCommand{"switch_led", DpValue::FromBool(true)},
                       DeviceCommand{"bright_value", DpValue::FromInt(128)},
                       DeviceCommand{"work_mode", DpValue::FromString("white")},
                       DeviceCommand{"countdown", DpValue::FromInt(0)}},
                      openapi_cmds, sharing_cmds);
        CheckEqInt(r, "route.mixed.openapi.count", 2, static_cast<long long>(openapi_cmds.size()));
        CheckEqInt(r, "route.mixed.sharing.count", 2, static_cast<long long>(sharing_cmds.size()));
        if (openapi_cmds.size() == 2) {
            CheckEqStr(r, "route.mixed.openapi.0.code", "switch_led", openapi_cmds[0].code);
            CheckEqStr(r, "route.mixed.openapi.1.code", "bright_value", openapi_cmds[1].code);
            CheckTrue(r, "route.mixed.openapi.0.value",
                      openapi_cmds[0].value == DpValue::FromBool(true));
            CheckTrue(r, "route.mixed.openapi.1.value",
                      openapi_cmds[1].value == DpValue::FromInt(128));
        }
        if (sharing_cmds.size() == 2) {
            CheckEqStr(r, "route.mixed.sharing.0.code", "work_mode", sharing_cmds[0].code);
            CheckEqStr(r, "route.mixed.sharing.1.code", "countdown", sharing_cmds[1].code);
        }
    }

    // ALL sharing — the openapi group is empty, order preserved.
    {
        std::vector<DeviceCommand> openapi_cmds;
        std::vector<DeviceCommand> sharing_cmds;
        RouteCommands(device,
                      {DeviceCommand{"work_mode", DpValue::FromString("white")},
                       DeviceCommand{"countdown", DpValue::FromInt(10)}},
                      openapi_cmds, sharing_cmds);
        CheckEqInt(r, "route.all.sharing.openapi", 0, static_cast<long long>(openapi_cmds.size()));
        CheckEqInt(r, "route.all.sharing.count", 2, static_cast<long long>(sharing_cmds.size()));
    }

    // ALL OpenAPI (a fully enriched device).
    {
        std::vector<DeviceCommand> openapi_cmds;
        std::vector<DeviceCommand> sharing_cmds;
        RouteCommands(device,
                      {DeviceCommand{"switch_led", DpValue::FromBool(false)},
                       DeviceCommand{"bright_value", DpValue::FromInt(42)}},
                      openapi_cmds, sharing_cmds);
        CheckEqInt(r, "route.all.openapi.count", 2, static_cast<long long>(openapi_cmds.size()));
        CheckEqInt(r, "route.all.openapi.sharing", 0, static_cast<long long>(sharing_cmds.size()));
    }

    // A code with NO local_strategy entry routes to sharing.
    {
        std::vector<DeviceCommand> openapi_cmds;
        std::vector<DeviceCommand> sharing_cmds;
        RouteCommands(device, {DeviceCommand{"unknown_code", DpValue::FromBool(true)}},
                      openapi_cmds, sharing_cmds);
        CheckEqInt(r, "route.unknown.code.openapi", 0, static_cast<long long>(openapi_cmds.size()));
        CheckEqInt(r, "route.unknown.code.sharing", 1, static_cast<long long>(sharing_cmds.size()));
    }

    // An EMPTY local_strategy: everything sharing.
    {
        Device bare;
        bare.id = "dev-bare";
        std::vector<DeviceCommand> openapi_cmds;
        std::vector<DeviceCommand> sharing_cmds;
        RouteCommands(bare, {DeviceCommand{"switch_led", DpValue::FromBool(true)}}, openapi_cmds,
                      sharing_cmds);
        CheckEqInt(r, "route.bare.openapi", 0, static_cast<long long>(openapi_cmds.size()));
        CheckEqInt(r, "route.bare.sharing", 1, static_cast<long long>(sharing_cmds.size()));
    }

    // Duplicate status_code: the FIRST dp_id's entry wins — dp 1 shadows dp 5.
    {
        std::vector<DeviceCommand> openapi_cmds;
        std::vector<DeviceCommand> sharing_cmds;
        RouteCommands(device, {DeviceCommand{"switch_led", DpValue::FromBool(true)}}, openapi_cmds,
                      sharing_cmds);
        CheckEqInt(r, "route.dup.code.first.wins", 1, static_cast<long long>(openapi_cmds.size()));
        CheckEqInt(r, "route.dup.code.sharing", 0, static_cast<long long>(sharing_cmds.size()));
    }
}

void TestBuildShadowIssueBody(TestResult& r) {
    std::string body;

    // Byte-exact goldens; "properties" is a double-encoded JSON STRING.
    CheckTrue(r, "shadow.body.bool",
              BuildShadowIssueBody({DeviceCommand{"switch_led", DpValue::FromBool(true)}}, body));
    CheckEqStr(r, "shadow.body.bool.json", "{\"properties\":\"{\\\"switch_led\\\":true}\"}", body);

    CheckTrue(r, "shadow.body.bool.false",
              BuildShadowIssueBody({DeviceCommand{"switch_led", DpValue::FromBool(false)}}, body));
    CheckEqStr(r, "shadow.body.bool.false.json", "{\"properties\":\"{\\\"switch_led\\\":false}\"}",
               body);

    CheckTrue(r, "shadow.body.int",
              BuildShadowIssueBody({DeviceCommand{"bright_value", DpValue::FromInt(128)}}, body));
    CheckEqStr(r, "shadow.body.int.json", "{\"properties\":\"{\\\"bright_value\\\":128}\"}", body);

    CheckTrue(r, "shadow.body.int.negative",
              BuildShadowIssueBody({DeviceCommand{"countdown", DpValue::FromInt(-5)}}, body));
    CheckEqStr(r, "shadow.body.int.negative.json", "{\"properties\":\"{\\\"countdown\\\":-5}\"}",
               body);

    CheckTrue(
        r, "shadow.body.string",
        BuildShadowIssueBody({DeviceCommand{"work_mode", DpValue::FromString("white")}}, body));
    CheckEqStr(r, "shadow.body.string.json",
               "{\"properties\":\"{\\\"work_mode\\\":\\\"white\\\"}\"}", body);

    CheckTrue(r, "shadow.body.double",
              BuildShadowIssueBody({DeviceCommand{"temp", DpValue::FromDouble(2.5)}}, body));
    CheckEqStr(r, "shadow.body.double.json", "{\"properties\":\"{\\\"temp\\\":2.5}\"}", body);

    // ALL commands in ONE inner document, insertion order kept.
    CheckTrue(r, "shadow.body.multi",
              BuildShadowIssueBody({DeviceCommand{"switch_led", DpValue::FromBool(true)},
                                    DeviceCommand{"bright_value", DpValue::FromInt(128)},
                                    DeviceCommand{"work_mode", DpValue::FromString("white")}},
                                   body));
    CheckEqStr(r, "shadow.body.multi.json",
               "{\"properties\":\"{\\\"switch_led\\\":true,\\\"bright_value\\\":128,"
               "\\\"work_mode\\\":\\\"white\\\"}\"}",
               body);

    // Escaping: the inner value's escapes are re-escaped by the outer embedding.
    CheckTrue(r, "shadow.body.escape",
              BuildShadowIssueBody({DeviceCommand{"mode", DpValue::FromString("a\"b\\c")}}, body));
    CheckEqStr(r, "shadow.body.escape.json",
               "{\"properties\":\"{\\\"mode\\\":\\\"a\\\\\\\"b\\\\\\\\c\\\"}\"}", body);

    // Round-trip: "properties" is a STRING; the inner text parses.
    {
        CheckTrue(r, "shadow.body.roundtrip.build",
                  BuildShadowIssueBody({DeviceCommand{"switch_led", DpValue::FromBool(true)},
                                        DeviceCommand{"bright_value", DpValue::FromInt(7)}},
                                       body));
        const JsonPtr root(body.c_str());
        CheckTrue(r, "shadow.body.roundtrip.parse", root.root() != nullptr);
        const cJSON* props = root.root() != nullptr
                                 ? cJSON_GetObjectItemCaseSensitive(root.root(), "properties")
                                 : nullptr;
        CheckTrue(r, "shadow.body.roundtrip.isstring", cJSON_IsString(props));
        if (cJSON_IsString(props) && props->valuestring != nullptr) {
            const JsonPtr inner(props->valuestring);
            CheckTrue(r, "shadow.body.roundtrip.inner.parse", inner.root() != nullptr);
            const cJSON* led = inner.root() != nullptr
                                   ? cJSON_GetObjectItemCaseSensitive(inner.root(), "switch_led")
                                   : nullptr;
            const cJSON* bright =
                inner.root() != nullptr
                    ? cJSON_GetObjectItemCaseSensitive(inner.root(), "bright_value")
                    : nullptr;
            CheckTrue(r, "shadow.body.roundtrip.inner.bool", cJSON_IsTrue(led));
            CheckTrue(r, "shadow.body.roundtrip.inner.int",
                      cJSON_IsNumber(bright) && bright->valueint == 7);
        }
    }

    // Rejections mirror BuildCommandsBody.
    CheckTrue(r, "shadow.body.reject.empty.list", !BuildShadowIssueBody({}, body));
    CheckTrue(r, "shadow.body.reject.empty.code",
              !BuildShadowIssueBody({DeviceCommand{"", DpValue::FromBool(true)}}, body));
    CheckTrue(r, "shadow.body.reject.null.value",
              !BuildShadowIssueBody({DeviceCommand{"switch_led", DpValue()}}, body));
    CheckTrue(r, "shadow.body.reject.empty.string.value",
              !BuildShadowIssueBody({DeviceCommand{"mode", DpValue::FromString("")}}, body));
}

void TestBuildOpenApiCommandsBody(TestResult& r) {
    std::string body;

    // The SAME shape as the sharing body.
    CheckTrue(
        r, "oa.body.bool",
        BuildOpenApiCommandsBody({DeviceCommand{"switch_led", DpValue::FromBool(true)}}, body));
    CheckEqStr(r, "oa.body.bool.json", "{\"commands\":[{\"code\":\"switch_led\",\"value\":true}]}",
               body);

    CheckTrue(r, "oa.body.multi",
              BuildOpenApiCommandsBody({DeviceCommand{"switch_led", DpValue::FromBool(false)},
                                        DeviceCommand{"bright_value", DpValue::FromInt(128)}},
                                       body));
    CheckEqStr(r, "oa.body.multi.json",
               "{\"commands\":[{\"code\":\"switch_led\",\"value\":false},"
               "{\"code\":\"bright_value\",\"value\":128}]}",
               body);

    CheckTrue(
        r, "oa.body.string",
        BuildOpenApiCommandsBody({DeviceCommand{"work_mode", DpValue::FromString("white")}}, body));
    CheckEqStr(r, "oa.body.string.json",
               "{\"commands\":[{\"code\":\"work_mode\",\"value\":\"white\"}]}", body);

    // Identical to BuildCommandsBody output (the shared builder).
    std::string sharing_body;
    CheckTrue(
        r, "oa.body.sharing.equal.build",
        BuildCommandsBody({DeviceCommand{"switch_led", DpValue::FromBool(true)}}, sharing_body));
    std::string openapi_body;
    CheckTrue(r, "oa.body.sharing.equal.oa.build",
              BuildOpenApiCommandsBody({DeviceCommand{"switch_led", DpValue::FromBool(true)}},
                                       openapi_body));
    CheckEqStr(r, "oa.body.sharing.equal", sharing_body, openapi_body);

    // Rejections: the shared policy.
    CheckTrue(r, "oa.body.reject.empty.list", !BuildOpenApiCommandsBody({}, body));
    CheckTrue(r, "oa.body.reject.empty.code",
              !BuildOpenApiCommandsBody({DeviceCommand{"", DpValue::FromInt(1)}}, body));
    CheckTrue(r, "oa.body.reject.null.value",
              !BuildOpenApiCommandsBody({DeviceCommand{"switch_led", DpValue()}}, body));
}

void TestSplitByPropertyUpdate(TestResult& r) {
    const Device device = MixedStrategyDevice();

    // property_update decides each group's PRIMARY endpoint.
    {
        std::vector<DeviceCommand> issue_cmds;
        std::vector<DeviceCommand> regular_cmds;
        SplitByPropertyUpdate(device,
                              {DeviceCommand{"switch_led", DpValue::FromBool(true)},
                               DeviceCommand{"bright_value", DpValue::FromInt(128)}},
                              issue_cmds, regular_cmds);
        CheckEqInt(r, "split.issue.count", 1, static_cast<long long>(issue_cmds.size()));
        CheckEqInt(r, "split.regular.count", 1, static_cast<long long>(regular_cmds.size()));
        if (issue_cmds.size() == 1) {
            CheckEqStr(r, "split.issue.0.code", "switch_led", issue_cmds[0].code);
            CheckTrue(r, "split.issue.0.value", issue_cmds[0].value == DpValue::FromBool(true));
        }
        if (regular_cmds.size() == 1) {
            CheckEqStr(r, "split.regular.0.code", "bright_value", regular_cmds[0].code);
            CheckTrue(r, "split.regular.0.value", regular_cmds[0].value == DpValue::FromInt(128));
        }
    }

    // The fallback matrix: primary + fallback bodies.
    {
        std::string issue_primary;
        CheckTrue(r, "split.issue.primary.body",
                  BuildShadowIssueBody({DeviceCommand{"switch_led", DpValue::FromBool(true)}},
                                       issue_primary));
        std::string issue_fallback;
        CheckTrue(r, "split.issue.fallback.body",
                  BuildOpenApiCommandsBody({DeviceCommand{"switch_led", DpValue::FromBool(true)}},
                                           issue_fallback));
        CheckEqStr(r, "split.issue.primary.json", "{\"properties\":\"{\\\"switch_led\\\":true}\"}",
                   issue_primary);
        CheckEqStr(r, "split.issue.fallback.json",
                   "{\"commands\":[{\"code\":\"switch_led\",\"value\":true}]}", issue_fallback);

        std::string regular_primary;
        CheckTrue(r, "split.regular.primary.body",
                  BuildOpenApiCommandsBody({DeviceCommand{"bright_value", DpValue::FromInt(128)}},
                                           regular_primary));
        std::string regular_fallback;
        CheckTrue(r, "split.regular.fallback.body",
                  BuildShadowIssueBody({DeviceCommand{"bright_value", DpValue::FromInt(128)}},
                                       regular_fallback));
        CheckEqStr(r, "split.regular.primary.json",
                   "{\"commands\":[{\"code\":\"bright_value\",\"value\":128}]}", regular_primary);
        CheckEqStr(r, "split.regular.fallback.json",
                   "{\"properties\":\"{\\\"bright_value\\\":128}\"}", regular_fallback);
    }

    // No property_update anywhere: everything lands in the regular group.
    {
        Device bare;
        bare.id = "dev-bare";
        std::vector<DeviceCommand> issue_cmds;
        std::vector<DeviceCommand> regular_cmds;
        SplitByPropertyUpdate(bare, {DeviceCommand{"any", DpValue::FromBool(true)}}, issue_cmds,
                              regular_cmds);
        CheckEqInt(r, "split.bare.issue", 0, static_cast<long long>(issue_cmds.size()));
        CheckEqInt(r, "split.bare.regular", 1, static_cast<long long>(regular_cmds.size()));
    }
}

void TestQuotaHelpers(TestResult& r) {
    // Encode form: "<YYYYMM>:dev1,dev2,..." (sorted set order).
    {
        const std::set<std::string> devices{"devB", "devA", "devC"};
        CheckEqStr(r, "quota.encode", "202610:devA,devB,devC", QuotaEncode(202610, devices));
        CheckEqStr(r, "quota.encode.empty", "202610:", QuotaEncode(202610, {}));
    }

    // Decode round-trip.
    {
        int month = 0;
        std::set<std::string> devices;
        CheckTrue(r, "quota.decode", QuotaDecode("202610:devA,devB", month, devices));
        CheckEqInt(r, "quota.decode.month", 202610, month);
        CheckEqInt(r, "quota.decode.count", 2, static_cast<long long>(devices.size()));
        CheckTrue(r, "quota.decode.has.a", devices.count("devA") == 1);
        CheckTrue(r, "quota.decode.has.b", devices.count("devB") == 1);

        // Tolerant forms: trailing/duplicate commas, empty device list.
        CheckTrue(r, "quota.decode.trailing.comma", QuotaDecode("202610:devA,", month, devices));
        CheckEqInt(r, "quota.decode.trailing.count", 1, static_cast<long long>(devices.size()));
        CheckTrue(r, "quota.decode.empty.devices", QuotaDecode("202611:", month, devices));
        CheckEqInt(r, "quota.decode.empty.month", 202611, month);
        CheckEqInt(r, "quota.decode.empty.count", 0, static_cast<long long>(devices.size()));

        // Malformed: devices CLEARED first, month untouched, false.
        month = 202612;
        devices.insert("devX");
        CheckTrue(r, "quota.decode.reject.no.colon", !QuotaDecode("202610", month, devices));
        CheckTrue(r, "quota.decode.reject.bad.month", !QuotaDecode("abc:devA", month, devices));
        CheckTrue(r, "quota.decode.reject.leading.colon", !QuotaDecode(":devA", month, devices));
        CheckEqInt(r, "quota.decode.reject.month.untouched", 202612, month);
        CheckEqInt(r, "quota.decode.reject.devices.cleared", 0,
                   static_cast<long long>(devices.size()));
    }

    // QuotaRecord: a new month CLEARS the set, then inserts.
    {
        int month = 202610;  // last month
        std::set<std::string> devices{"dev1", "dev2"};
        // Same month: a new device is added, the state changes.
        CheckTrue(r, "quota.record.same.month.new.dev",
                  QuotaRecord(202610, "dev3", month, devices));
        CheckEqInt(r, "quota.record.same.month.count", 3, static_cast<long long>(devices.size()));
        // Same month + known device: nothing changes (no persist).
        CheckTrue(r, "quota.record.same.month.known.dev",
                  !QuotaRecord(202610, "dev3", month, devices));
        CheckEqInt(r, "quota.record.known.dev.count", 3, static_cast<long long>(devices.size()));
        // NEW month: the set resets, the device is tracked alone.
        CheckTrue(r, "quota.record.rollover", QuotaRecord(202611, "dev9", month, devices));
        CheckEqInt(r, "quota.record.rollover.month", 202611, month);
        CheckEqInt(r, "quota.record.rollover.count", 1, static_cast<long long>(devices.size()));
        CheckTrue(r, "quota.record.rollover.has.new", devices.count("dev9") == 1);
        CheckTrue(r, "quota.record.rollover.old.gone", devices.count("dev1") == 0);
        // Back to a PREVIOUS month: also a roll.
        CheckTrue(r, "quota.record.backward.roll", QuotaRecord(202610, "dev1", month, devices));
        CheckEqInt(r, "quota.record.backward.month", 202610, month);
        CheckEqInt(r, "quota.record.backward.count", 1, static_cast<long long>(devices.size()));
        // Never tracked (month 0): the first record rolls and tracks.
        int fresh_month = 0;
        std::set<std::string> fresh_devices;
        CheckTrue(r, "quota.record.first", QuotaRecord(202610, "dev1", fresh_month, fresh_devices));
        CheckEqInt(r, "quota.record.first.month", 202610, fresh_month);
        CheckEqInt(r, "quota.record.first.count", 1, static_cast<long long>(fresh_devices.size()));
        // Unsynced clock (month <= 0): strict NO-OP.
        CheckTrue(r, "quota.record.unsynced.noop",
                  !QuotaRecord(0, "devX", fresh_month, fresh_devices));
        CheckEqInt(r, "quota.record.unsynced.month", 202610, fresh_month);
        CheckEqInt(r, "quota.record.unsynced.count", 1,
                   static_cast<long long>(fresh_devices.size()));
    }
}

/// The command-sent hook must stay silent unless BOTH legs succeeded. A
/// successful send needs the cloud, so only the negative side is driven here.
void TestCommandSentHook(TestResult& r) {
    CommandService& service = CommandService::GetInstance();
    int fired = 0;
    service.SetCommandSentHook([&fired] { ++fired; });

    // Sharing leg unconfigured -> kError, so the notifier must not run.
    CheckTrue(r, "hook.send_failed",
              service.SendDeviceCommands("selftest-hook",
                                         {DeviceCommand{"switch_1", DpValue::FromBool(true)}}) ==
                  CommandResult::kError);
    CheckEqInt(r, "hook.not_fired_on_error", 0, fired);
    // Rejected before any leg (empty dev id).
    CheckTrue(
        r, "hook.rejected_before_send",
        service.SendDeviceCommands("", {DeviceCommand{"switch_1", DpValue::FromBool(true)}}) ==
            CommandResult::kError);
    CheckEqInt(r, "hook.not_fired_on_reject", 0, fired);

    // The hook is replaceable/clearable (a stale capture must be releasable).
    service.SetCommandSentHook(nullptr);
    CheckTrue(r, "hook.cleared_service_still_errors",
              service.SendDeviceCommands("selftest-hook",
                                         {DeviceCommand{"switch_1", DpValue::FromBool(true)}}) ==
                  CommandResult::kError);
}

}  // namespace

bool RunCommandSelfTest() {
    TestResult r;
    TestDpValueEquality(r);
    TestDedupFilter(r);
    TestBuildCommandsBody(r);
    TestSceneRequests(r);
    // The disabled-entity guard inside the LIVE SendDeviceCommands.
    TestDisabledEntityRejection(r);
    TestRouteCommands(r);
    TestBuildShadowIssueBody(r);
    TestBuildOpenApiCommandsBody(r);
    TestSplitByPropertyUpdate(r);
    TestQuotaHelpers(r);
    TestCommandSentHook(r);

    if (r.passed == r.total) {
        ESP_LOGI(TAG, "COMMAND SELFTEST PASS (%d checks)", r.total);
        return true;
    }
    ESP_LOGE(TAG, "COMMAND SELFTEST FAIL (%d/%d)", r.passed, r.total);
    return false;
}

}  // namespace hub_core
