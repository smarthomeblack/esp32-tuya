#pragma once

namespace hub_core {

/// Discovery service self-test (degraded detail neither cached nor refreshed,
/// and commit refresh of a strictly-poorer detail-loaded device).
bool RunDiscoverySelfTest();

}  // namespace hub_core
