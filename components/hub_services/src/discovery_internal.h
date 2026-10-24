#pragma once

/// INTERNAL (src-only) — do not include from public headers.
/// Discovery detail-publish and commit-apply decision helpers (no locks).

#include "hub_core/device.h"
#include "hub_core/device_registry.h"
#include "hub_services/discovery.h"

namespace hub_core {

/// Publishes a fetched detail model into `staging` (caller holds the
/// staging lock); only a fully successful fetch is cached in detail_loaded.
DetailFetchStatus PublishDetailResult(DiscoveryStaging& staging, Device& device,
                                      unsigned expected_generation, int failed_calls,
                                      bool auth_dead);

/// One commit entry applied to the registry.
enum class CommitApplyResult {
    kAdded,            ///< the id was new — the device was inserted
    kRefreshed,        ///< richer, non-shrinking staged model replaced the maps
    kSkippedExisting,  ///< equal, poorer, or richer-but-shrinking model — registry untouched
};

/// Applies one commit entry; a staged model that is detail-loaded, strictly
/// richer AND no-shrink refreshes an existing device (maps only — identity
/// and runtime preserved); a shrinking map is skipped, as is !detail_loaded.
CommitApplyResult ApplyCommitEntry(DeviceRegistry& registry, const Device& candidate,
                                   const DiscoveryCommitEntry& entry, bool detail_loaded);

}  // namespace hub_core
