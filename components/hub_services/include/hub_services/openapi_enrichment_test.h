#pragma once

namespace hub_core {

/// Golden-sample self-test for the OpenAPI enrichment parse helpers
/// (Thing Model + Shadow). Pure CPU; true only when every check passes.
bool RunOpenApiEnrichmentSelfTest();

}  // namespace hub_core
