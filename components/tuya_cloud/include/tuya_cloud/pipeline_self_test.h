#pragma once

namespace tuya_cloud {

/// Golden-vector self-test for the sharing signing pipeline (src/signing.h)
/// against src/pipeline_vectors.h. Returns true only when every check passes.
bool RunPipelineSelfTest();

}  // namespace tuya_cloud
