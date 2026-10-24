#pragma once

#include <string>
#include <string_view>

#include "tuya_crypto/json_writer.h"

namespace tuya_cloud {

/// Substitutes "{devId}"; device ids are [A-Za-z0-9-] — no URL escaping.
inline std::string DevicePath(std::string_view path, std::string_view dev_id) {
    std::string out(path);
    constexpr std::string_view kPlaceholder = "{devId}";
    const size_t pos = out.find(kPlaceholder);
    if (pos != std::string::npos) {
        out.replace(pos, kPlaceholder.size(), dev_id);
    }
    return out;
}

/// Query plaintext {"homeId":"<id>"} — compact, insertion order.
inline std::string HomeIdParams(std::string_view home_id) {
    tuya_crypto::JsonWriter writer;
    writer.BeginObject();
    writer.Key("homeId");
    writer.String(home_id);
    writer.EndObject();
    return writer.Str();
}

}  // namespace tuya_cloud
