#include "tuya_cloud/pipeline_self_test.h"

#include <esp_log.h>

#include <string>

#include "pipeline_vectors.h"
#include "signing.h"

static const char* TAG = "PipelineSelfTest";

namespace tuya_cloud {

namespace {

struct TestResult {
    int total = 0;
    int passed = 0;
};

// Keep logged strings readable; URL/encdata are long base64 blobs.
std::string TruncateForLog(const std::string& s, size_t max_chars = 100) {
    if (s.size() <= max_chars) {
        return s;
    }
    return s.substr(0, max_chars) + "...(" + std::to_string(s.size()) + "B)";
}

void CheckEqStr(TestResult& r, const char* case_name, const char* field,
                const std::string& expected, const std::string& got) {
    ++r.total;
    if (expected == got) {
        ++r.passed;
        return;
    }
    ESP_LOGE(TAG, "FAIL %s.%s: expected=\"%s\" got=\"%s\"", case_name, field,
             TruncateForLog(expected).c_str(), TruncateForLog(got).c_str());
}

void TestPipelineVectors(TestResult& r) {
    // Checks every BuildSignedRequest output field (X-sid is "" and still checked).
    for (const pipeline_vectors::PipelineVector& v : pipeline_vectors::kPipeline) {
        const BuiltRequest got =
            BuildSignedRequest(v.endpoint, v.path, v.rid, v.access_token, v.refresh_token,
                               v.time_ms, v.nonce, v.params_json, v.body_json);
        const char* name = v.name;
        CheckEqStr(r, name, "url", v.url, got.url);
        CheckEqStr(r, name, "body", v.body, got.body);
        CheckEqStr(r, name, "app_key", v.app_key, got.app_key);
        CheckEqStr(r, name, "request_id", v.request_id, got.request_id);
        CheckEqStr(r, name, "sid", v.sid, got.sid);
        CheckEqStr(r, name, "time", v.time, got.time);
        CheckEqStr(r, name, "token", v.token, got.token);
        CheckEqStr(r, name, "sign", v.sign, got.sign);
    }
}

void TestPercentEncode(TestResult& r) {
    for (const pipeline_vectors::PercentEncodeVector& v : pipeline_vectors::kPercentEncode) {
        CheckEqStr(r, "percent_encode", "encode", v.expected, PercentEncode(v.input));
    }
}

}  // namespace

bool RunPipelineSelfTest() {
    TestResult r;
    TestPipelineVectors(r);
    TestPercentEncode(r);

    if (r.passed == r.total) {
        ESP_LOGI(TAG, "PIPELINE SELFTEST PASS (%d checks)", r.total);
        return true;
    }
    ESP_LOGE(TAG, "PIPELINE SELFTEST FAIL (%d/%d)", r.passed, r.total);
    return false;
}

}  // namespace tuya_cloud
