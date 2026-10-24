#pragma once

#include <string>
#include <string_view>

#include "hub_core/device.h"

namespace hub_core {

/// Raw-dp -> standard-code/value strategies (`default`, `enum`).
class Strategies {
public:
    Strategies() = delete;  // static-only

    /// Converts one raw dp report; false = DROP (unknown strategy, bad
    /// statusFormat, no fallback, failed Enum gate).
    static bool Convert(const StrategyConfig& cfg, int dp_id, const DpValue& raw,
                        std::string& out_code, DpValue& out_value,
                        const DeviceStatusRangeMap& status_range);

private:
    /// `default` strategy: raw passthrough, else the type's default value.
    static bool ConvertDefault(const StrategyConfig& cfg, const DpValue& raw, std::string& out_code,
                               DpValue& out_value);

    /// `enum` strategy: enum_mapping lookup, else the default fallback.
    static bool ConvertEnum(const StrategyConfig& cfg, const DpValue& raw, std::string& out_code,
                            DpValue& out_value);

    /// The shared type-default fallback of both strategies.
    static bool ConvertDefaultValue(const StrategyConfig& cfg, DpValue& out_value);

    /// out_code = the single key of the statusFormat JSON object.
    static bool CodeFromStatusFormat(const DeviceString& status_format_json, std::string& out_code);

    /// Enum range gate: drop when the Enum range excludes the value; unparsable keeps.
    static bool PassesEnumRangeGate(const std::string& code, const DpValue& value,
                                    const DeviceStatusRangeMap& status_range);
};

/// One raw dp report -> (code, value): strategy lookup + Convert + drop
/// logging; false = drop.
bool ConvertDpReport(std::string_view dev_id, const DeviceStrategyMap& local_strategy,
                     const DeviceStatusRangeMap& status_range, int dp_id, const DpValue& raw,
                     std::string& out_code, DpValue& out_value);

}  // namespace hub_core
