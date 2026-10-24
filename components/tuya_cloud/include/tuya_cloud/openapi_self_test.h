#pragma once

namespace tuya_cloud {

/// Golden-vector self-test for the OpenAPI signing/login/URL helpers (vectors in
/// openapi_self_test.cc). Returns true only when every check passes.
bool RunOpenApiSelfTest();

}  // namespace tuya_cloud
