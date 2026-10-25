// Host runner for the hub's pure-logic self-test suites.
//
// Compiles the REAL component self-test sources (no ESP-IDF) against
// include-path shims, runs every suite in the on-device boot-gate order and
// asserts the exact "TAG: ... SELFTEST PASS (N checks)" log line captured
// from the shimmed ESP_LOG — so both failures AND silent check-count drift
// fail the run.

#include <cstdio>
#include <cstring>

#include "esp_log.h"  // host shim

#include "hub_core/device_store_test.h"
#include "hub_core/model_self_test.h"
#include "hub_services/command_service_test.h"
#include "hub_services/discovery_test.h"
#include "hub_services/openapi_enrichment_test.h"
#include "tuya_cloud/openapi_self_test.h"
#include "tuya_cloud/pipeline_self_test.h"
#include "tuya_mqtt/mqtt_message_test.h"

namespace {

// Pinned per-suite check counts — the single source of truth.
// These mirror the on-device boot gate lines; a diff in either direction
// (check added or dropped) must be a deliberate, reviewed change.
struct Suite {
    const char* name;
    const char* pass_line;
    bool (*run)();
};

constexpr Suite kSuites[] = {
    {"pipeline", "PipelineSelfTest: PIPELINE SELFTEST PASS (26 checks)",
     tuya_cloud::RunPipelineSelfTest},
    {"openapi", "TuyaIoTTest: TUYA IOT SELFTEST PASS (28 checks)", tuya_cloud::RunOpenApiSelfTest},
    {"model", "ModelSelfTest: MODEL SELFTEST PASS (172 checks)", hub_core::RunModelSelfTest},
    {"store", "DeviceStoreTest: DEVICE STORE SELFTEST PASS (64 checks)",
     hub_core::RunDeviceStoreSelfTest},
    {"enrich", "EnrichSelfTest: TUYA IOT ENRICH SELFTEST PASS (87 checks)",
     hub_core::RunOpenApiEnrichmentSelfTest},
    {"mqtt", "MqttSelfTest: MQTT SELFTEST PASS (110 checks)", tuya_mqtt::RunMqttMessageSelfTest},
    {"command", "CommandSelfTest: COMMAND SELFTEST PASS (219 checks)",
     hub_core::RunCommandSelfTest},
    {"discovery", "DiscoverySelfTest: DISCOVERY SELFTEST PASS (90 checks)",
     hub_core::RunDiscoverySelfTest},
};

// Suites that stay hardware-only; printed in every run so the coverage gap
// is explicit. Total on-device gate: 949 checks.
constexpr const char* kExcluded[] = {
    "crypto (75): at-rest key material comes from eFuse (esp_hmac/esp_efuse)",
    "sys (56): settings roundtrip + log buffer need NVS/heap introspection",
    "assets (22): asset_store links ESP-IDF flash/mmap; the vectors are RAM-only",
};

constexpr int kPinnedTotal = 26 + 28 + 172 + 64 + 87 + 110 + 219 + 90;
constexpr int kDeviceTotal = kPinnedTotal + 75 + 56 + 22;

bool LineContains(const char* haystack, const char* needle) {
    return std::strstr(haystack, needle) != nullptr;
}

}  // namespace

int main() {
    printf("Host self-test runner: %zu suites, %d/%d on-device checks covered\n\n",
           sizeof(kSuites) / sizeof(kSuites[0]), kPinnedTotal, kDeviceTotal);

    int failed = 0;
    for (const Suite& suite : kSuites) {
        host_log_reset();
        const bool ok = suite.run();

        const char* found = nullptr;
        for (size_t i = 0; i < host_log_count(); ++i) {
            const char* line = host_log_line(i);
            if (std::strcmp(line, suite.pass_line) == 0) {
                found = line;
            }
        }
        bool saw_fail = false;
        for (size_t i = 0; i < host_log_count(); ++i) {
            if (LineContains(host_log_line(i), "SELFTEST FAIL")) {
                saw_fail = true;
            }
        }

        if (ok && found != nullptr && !saw_fail) {
            printf("[PASS] %-8s %s\n\n", suite.name, suite.pass_line);
        } else {
            ++failed;
            printf("[FAIL] %-8s return=%s pass_line=%s fail_line_logged=%s\n", suite.name,
                   ok ? "true" : "false", found != nullptr ? "matched" : "MISSING",
                   saw_fail ? "yes" : "no");
            printf("  expected: %s\n", suite.pass_line);
            printf("  captured %zu log line(s) above; mismatching lines:\n", host_log_count());
            for (size_t i = 0; i < host_log_count(); ++i) {
                if (LineContains(host_log_line(i), "FAIL")) {
                    printf("    %s\n", host_log_line(i));
                }
            }
            printf("\n");
        }
    }

    printf("Hardware-only suites excluded (%d checks): ", 75 + 56 + 22);
    for (const char* reason : kExcluded) {
        printf("\n  - %s", reason);
    }
    printf("\n\n");

    if (failed != 0) {
        printf("SELFTEST RUNNER FAIL (%d/%zu suites)\n", failed,
               sizeof(kSuites) / sizeof(kSuites[0]));
        return 1;
    }
    printf("SELFTEST RUNNER PASS (%zu suites, %d checks)\n", sizeof(kSuites) / sizeof(kSuites[0]),
           kPinnedTotal);
    return 0;
}
