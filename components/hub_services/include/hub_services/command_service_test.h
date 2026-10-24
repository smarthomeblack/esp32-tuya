#pragma once

namespace hub_core {

/// Command service self-test (dedup table, byte-exact request bodies,
/// scenes parse, disabled-entity rejection); true only when all pass.
bool RunCommandSelfTest();

}  // namespace hub_core
