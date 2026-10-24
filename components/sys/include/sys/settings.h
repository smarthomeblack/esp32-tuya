#pragma once

#include <nvs.h>

#include <string>

namespace sys {

/// RAII NVS namespace wrapper (commits + closes on destruction); a full
/// NVS logs instead of aborting. Not thread-safe (stack-scoped per call).
class Settings {
public:
    Settings(const std::string& ns, bool read_write = false);
    ~Settings();
    Settings(const Settings&) = delete;
    Settings& operator=(const Settings&) = delete;

    std::string GetString(const std::string& key, const std::string& default_value = "");
    void SetString(const std::string& key, const std::string& value);

    bool GetBool(const std::string& key, bool default_value = false);
    void SetBool(const std::string& key, bool value);

    void EraseKey(const std::string& key);
    void EraseAll();

private:
    std::string ns_;
    nvs_handle_t nvs_handle_ = 0;
    bool read_write_ = false;
    bool dirty_ = false;
};

}  // namespace sys
