#include "hub_core/parse.h"

#include <cJSON.h>

#include "parse_helpers.h"

namespace hub_core {

namespace {

/// RAII cJSON tree for the parsed JSON text (guarded free).
class JsonTree {
public:
    explicit JsonTree(const std::string& json)
        : root_(cJSON_ParseWithLength(json.c_str(), json.size())) {}
    ~JsonTree() { cJSON_Delete(root_); }
    JsonTree(const JsonTree&) = delete;
    JsonTree& operator=(const JsonTree&) = delete;

    cJSON* root() const { return root_; }

private:
    cJSON* root_;
};

}  // namespace

bool ParseDeviceJson(const std::string& json, Device& out) {
    JsonTree tree(json);
    if (tree.root() == nullptr) {
        return false;
    }
    return ParseDevice(tree.root(), out);
}

bool ParseDeviceDetailResultJson(const std::string& json, Device& out) {
    JsonTree tree(json);
    if (tree.root() == nullptr || !cJSON_IsArray(tree.root())) {
        return false;
    }
    const cJSON* first = tree.root()->child;
    if (first == nullptr) {
        return false;  // empty detail array: the id is gone server-side
    }
    return ParseDevice(first, out);
}

void ParseSpecificationsJson(const std::string& json, Device& device) {
    JsonTree tree(json);
    if (tree.root() == nullptr) {
        return;
    }
    ParseSpecifications(tree.root(), device);
}

void ParseStatusStrategyJson(const std::string& json, Device& device) {
    JsonTree tree(json);
    if (tree.root() == nullptr) {
        return;
    }
    ParseStatusStrategy(tree.root(), device);
}

void ParseDpReportTypesJson(const std::string& json, Device& device) {
    JsonTree tree(json);
    if (tree.root() == nullptr) {
        return;
    }
    ParseDpReportTypes(tree.root(), device);
}

bool ParseRoomsJson(const std::string& json, std::vector<Room>& rooms) {
    JsonTree tree(json);
    if (tree.root() == nullptr) {
        return false;
    }
    return ParseRooms(tree.root(), rooms);
}

bool ParseRoomDevicesJson(const std::string& json, std::vector<std::string>& device_ids) {
    JsonTree tree(json);
    if (tree.root() == nullptr) {
        return false;
    }
    return ParseRoomDevices(tree.root(), device_ids);
}

}  // namespace hub_core
