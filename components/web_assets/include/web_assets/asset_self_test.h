#pragma once

namespace web_assets {

/// WAS1 parse/validate over pure RAM images plus the FindAsset pre-init
/// contract (no flash access); true only when every check passes.
bool RunWebAssetSelfTest();

}  // namespace web_assets
