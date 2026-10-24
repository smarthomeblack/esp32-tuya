// OpenAPI command routing, body building and quota helpers (openapi_command_internal.h).

#include <cerrno>
#include <cstdlib>
#include <ctime>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include "tuya_cloud/time_sync.h"
#include "tuya_crypto/json_writer.h"

#include "dedup_filter.h"
#include "openapi_command_internal.h"

namespace hub_core {

namespace {

/// First local_strategy entry with status_code == code, in dp_id order.
const StrategyConfig* FindStrategyForCode(const Device& device, const std::string& code) {
    for (const auto& entry : device.local_strategy) {
        if (std::string_view(entry.second.status_code) == code) {
            return &entry.second;
        }
    }
    return nullptr;
}

}  // namespace

std::string OpenApiDevicePath(std::string_view path_template, const std::string& dev_id) {
    std::string path(path_template);
    constexpr std::string_view kId = "{id}";
    const size_t pos = path.find(kId);
    if (pos != std::string::npos) {
        path.replace(pos, kId.size(), dev_id);
    }
    return path;
}

int CurrentMonth() {
    const time_t seconds = static_cast<time_t>(tuya_cloud::TimeSync::NowMs() / 1000);
    struct tm utc = {};
    gmtime_r(&seconds, &utc);
    const int month = (utc.tm_year + 1900) * 100 + (utc.tm_mon + 1);
    return month >= kQuotaMinValidMonth ? month : 0;
}

bool BuildShadowIssueBody(const std::vector<DeviceCommand>& commands, std::string& body) {
    // {"properties":"<inner JSON as a STRING>"} — double-encoded, all commands in one doc.
    if (commands.empty()) {
        return false;
    }
    tuya_crypto::JsonWriter inner;
    inner.BeginObject();
    for (const DeviceCommand& command : commands) {
        if (command.code.empty() || command.value.IsEmpty()) {
            return false;
        }
        inner.Key(command.code);
        switch (command.value.type) {
            case DpValue::Type::kBool:
                // json.dumps lowercase literals (NOT Python str(True)).
                inner.Bool(command.value.b);
                break;
            case DpValue::Type::kInt: inner.Int(command.value.i); break;
            case DpValue::Type::kDouble: inner.Double(command.value.d); break;
            case DpValue::Type::kString: inner.String(command.value.s); break;
        }
    }
    inner.EndObject();
    if (inner.HasError()) {
        return false;
    }

    // The wrapper embeds the inner document as one escaped JSON string.
    tuya_crypto::JsonWriter outer;
    outer.BeginObject();
    outer.Key("properties");
    outer.String(inner.Str());
    outer.EndObject();
    if (outer.HasError()) {
        return false;
    }
    body = outer.Str();
    return true;
}

bool BuildOpenApiCommandsBody(const std::vector<DeviceCommand>& commands, std::string& body) {
    // Same wire shape as the sharing commands body.
    return BuildCommandsBody(commands, body);
}

void RouteCommands(const Device& device, const std::vector<DeviceCommand>& commands,
                   std::vector<DeviceCommand>& openapi_cmds,
                   std::vector<DeviceCommand>& sharing_cmds) {
    openapi_cmds.clear();
    sharing_cmds.clear();
    for (const DeviceCommand& command : commands) {
        // No entry or use_open_api=false -> sharing.
        const StrategyConfig* cfg = FindStrategyForCode(device, command.code);
        if (cfg != nullptr && cfg->use_open_api) {
            openapi_cmds.push_back(command);
        } else {
            sharing_cmds.push_back(command);
        }
    }
}

void SplitByPropertyUpdate(const Device& device, const std::vector<DeviceCommand>& commands,
                           std::vector<DeviceCommand>& issue_cmds,
                           std::vector<DeviceCommand>& regular_cmds) {
    // property_update=true -> shadow/properties/issue; false -> /commands.
    issue_cmds.clear();
    regular_cmds.clear();
    for (const DeviceCommand& command : commands) {
        const StrategyConfig* cfg = FindStrategyForCode(device, command.code);
        const bool property_update = cfg != nullptr && cfg->property_update;
        if (property_update) {
            issue_cmds.push_back(command);
        } else {
            regular_cmds.push_back(command);
        }
    }
}

std::string QuotaEncode(int month, const std::set<std::string>& devices) {
    // NVS form "<YYYYMM>:dev1,dev2,..." (std::set iterates sorted).
    std::string out = std::to_string(month);
    out.push_back(':');
    bool first = true;
    for (const std::string& dev : devices) {
        if (!first) {
            out.push_back(',');
        }
        first = false;
        out += dev;
    }
    return out;
}

bool QuotaDecode(const std::string& text, int& month, std::set<std::string>& devices) {
    devices.clear();  // garbage never poisons the live set
    const size_t sep = text.find(':');
    if (sep == std::string::npos || sep == 0) {
        return false;
    }
    errno = 0;
    char* end = nullptr;
    const long long value = std::strtoll(text.c_str(), &end, 10);
    if (errno != 0 || end != text.c_str() + sep) {
        return false;
    }
    month = static_cast<int>(value);
    // Comma-split; empty tokens ignored.
    size_t pos = sep + 1;
    while (pos <= text.size()) {
        const size_t comma = text.find(',', pos);
        const size_t stop = comma == std::string::npos ? text.size() : comma;
        if (stop > pos) {
            devices.insert(text.substr(pos, stop - pos));
        }
        if (comma == std::string::npos) {
            break;
        }
        pos = comma + 1;
    }
    return true;
}

bool QuotaRecord(int month, const std::string& dev_id, int& tracked_month,
                 std::set<std::string>& devices) {
    // An unsynced clock (month <= 0) is a NO-OP.
    if (month <= 0) {
        return false;
    }
    bool rolled = false;
    if (month != tracked_month) {
        tracked_month = month;
        devices.clear();
        rolled = true;
    }
    const bool is_new = devices.insert(dev_id).second;
    return is_new || rolled;
}

}  // namespace hub_core
