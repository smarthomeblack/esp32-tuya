// One-shot NVS at-rest encryption switch: plaintext read-out -> eFuse key
// burn -> partition wipe -> encrypted init -> value write-back.

#include "nvs_secure_boot.h"

#include <esp_log.h>
#include <nvs.h>
#include <nvs_flash.h>
#include <nvs_sec_provider.h>

#include <cstdint>
#include <string>
#include <vector>

static const char* TAG = "NvsBoot";

namespace boot {

namespace {

// Snapshot of one NVS entry, enumerated at runtime (no hardcoded key list).
// `integer` holds every integer type; `data` holds STR/BLOB bytes.
struct NvsEntry {
    std::string ns;
    std::string key;
    nvs_type_t type = NVS_TYPE_ANY;
    std::string data;
    int64_t integer = 0;
};

// Reads the value by its real nvs_type_t through a read-only handle.
bool ReadEntryValue(NvsEntry& entry) {
    nvs_handle_t handle = 0;
    if (nvs_open(entry.ns.c_str(), NVS_READONLY, &handle) != ESP_OK) {
        return false;
    }
    const char* key = entry.key.c_str();
    esp_err_t err = ESP_FAIL;
    switch (entry.type) {
        case NVS_TYPE_U8: {
            uint8_t v = 0;
            err = nvs_get_u8(handle, key, &v);
            entry.integer = v;
            break;
        }
        case NVS_TYPE_I8: {
            int8_t v = 0;
            err = nvs_get_i8(handle, key, &v);
            entry.integer = v;
            break;
        }
        case NVS_TYPE_U16: {
            uint16_t v = 0;
            err = nvs_get_u16(handle, key, &v);
            entry.integer = v;
            break;
        }
        case NVS_TYPE_I16: {
            int16_t v = 0;
            err = nvs_get_i16(handle, key, &v);
            entry.integer = v;
            break;
        }
        case NVS_TYPE_U32: {
            uint32_t v = 0;
            err = nvs_get_u32(handle, key, &v);
            entry.integer = v;
            break;
        }
        case NVS_TYPE_I32: {
            int32_t v = 0;
            err = nvs_get_i32(handle, key, &v);
            entry.integer = v;
            break;
        }
        case NVS_TYPE_U64: {
            uint64_t v = 0;
            err = nvs_get_u64(handle, key, &v);
            entry.integer = static_cast<int64_t>(v);
            break;
        }
        case NVS_TYPE_I64: {
            err = nvs_get_i64(handle, key, &entry.integer);
            break;
        }
        default: {
            // STR and every blob flavor: probe the length, read the bytes.
            const bool is_str = entry.type == NVS_TYPE_STR;
            size_t len = 0;
            esp_err_t probe = ESP_FAIL;
            if (is_str) {
                probe = nvs_get_str(handle, key, nullptr, &len);
            } else {
                probe = nvs_get_blob(handle, key, nullptr, &len);
            }
            if (probe != ESP_OK) {
                break;
            }
            entry.data.assign(len, '\0');  // len 0: an empty blob is valid
            char* buf = len == 0 ? entry.data.data() : &entry.data[0];
            if (is_str) {
                err = nvs_get_str(handle, key, buf, &len);
            } else {
                err = nvs_get_blob(handle, key, buf, &len);
            }
            if (err == ESP_OK) {
                entry.data.resize(len);
            }
            break;
        }
    }
    nvs_close(handle);
    return err == ESP_OK;
}

// Plain init on purpose: enumerates the partition BEFORE the key exists.
// Every entry of every namespace is captured (contributor-added keys too).
void ReadPlaintextSnapshot(std::vector<NvsEntry>& out) {
    esp_err_t err = nvs_flash_init_partition(NVS_DEFAULT_PART_NAME);
    if (err != ESP_OK) {
        // Unreadable either way (corrupt/truncated) — migrate what we can: nothing.
        ESP_LOGW(TAG, "Plaintext NVS read-out failed (%s) — migrating empty", esp_err_to_name(err));
        return;
    }
    nvs_iterator_t it = nullptr;
    esp_err_t next = nvs_entry_find(NVS_DEFAULT_PART_NAME, nullptr, NVS_TYPE_ANY, &it);
    while (next == ESP_OK) {
        // Zero-init: namespace_name is left unset when the ns index is stale.
        nvs_entry_info_t info = {};
        nvs_entry_info(it, &info);
        NvsEntry entry;
        entry.ns = info.namespace_name;
        entry.key = info.key;
        entry.type = info.type;
        if (ReadEntryValue(entry)) {
            out.push_back(std::move(entry));
        } else {
            ESP_LOGW(TAG, "Migration: unreadable entry '%s/%s' dropped", info.namespace_name,
                     info.key);
        }
        next = nvs_entry_next(&it);
    }
    nvs_release_iterator(it);  // NULL when the iterator exhausted itself
    ESP_LOGI(TAG, "Plaintext NVS snapshot: %u entry(s)", static_cast<unsigned>(out.size()));
}

// Writes every snapshot entry back with the matching typed setter.
size_t WriteSnapshotBack(const std::vector<NvsEntry>& snapshot) {
    size_t restored = 0;
    for (const NvsEntry& entry : snapshot) {
        nvs_handle_t handle = 0;
        if (nvs_open(entry.ns.c_str(), NVS_READWRITE, &handle) != ESP_OK) {
            ESP_LOGE(TAG, "write-back: nvs_open('%s') failed", entry.ns.c_str());
            continue;
        }
        const char* key = entry.key.c_str();
        esp_err_t err = ESP_FAIL;
        switch (entry.type) {
            case NVS_TYPE_U8:
                err = nvs_set_u8(handle, key, static_cast<uint8_t>(entry.integer));
                break;
            case NVS_TYPE_I8:
                err = nvs_set_i8(handle, key, static_cast<int8_t>(entry.integer));
                break;
            case NVS_TYPE_U16:
                err = nvs_set_u16(handle, key, static_cast<uint16_t>(entry.integer));
                break;
            case NVS_TYPE_I16:
                err = nvs_set_i16(handle, key, static_cast<int16_t>(entry.integer));
                break;
            case NVS_TYPE_U32:
                err = nvs_set_u32(handle, key, static_cast<uint32_t>(entry.integer));
                break;
            case NVS_TYPE_I32:
                err = nvs_set_i32(handle, key, static_cast<int32_t>(entry.integer));
                break;
            case NVS_TYPE_U64:
                err = nvs_set_u64(handle, key, static_cast<uint64_t>(entry.integer));
                break;
            case NVS_TYPE_I64: err = nvs_set_i64(handle, key, entry.integer); break;
            case NVS_TYPE_STR: err = nvs_set_str(handle, key, entry.data.c_str()); break;
            default: err = nvs_set_blob(handle, key, entry.data.data(), entry.data.size()); break;
        }
        if (err == ESP_OK) {
            err = nvs_commit(handle);
        }
        nvs_close(handle);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "write-back of '%s/%s' failed: %s", entry.ns.c_str(), entry.key.c_str(),
                     esp_err_to_name(err));
        } else {
            ++restored;
        }
    }
    return restored;
}

// Recovery ladder, never aborts: init -> on ANY error erase + retry -> a
// second failure degrades (defaults + setup mode; Settings fail soft).
esp_err_t InitEncryptedWithRecovery() {
    esp_err_t err = nvs_flash_init();
    if (err == ESP_OK) {
        return ESP_OK;
    }
    ESP_LOGW(TAG, "NVS init failed (%s) — erasing and retrying", esp_err_to_name(err));
    esp_err_t erase_err = nvs_flash_erase();
    if (erase_err == ESP_OK) {
        err = nvs_flash_init();
        if (err == ESP_OK) {
            return ESP_OK;
        }
        ESP_LOGE(TAG, "NVS init failed after erase (%s)", esp_err_to_name(err));
    } else {
        ESP_LOGE(TAG, "nvs_flash_erase failed (%s)", esp_err_to_name(erase_err));
        err = erase_err;
    }
    ESP_LOGE(TAG, "NVS unavailable — continuing with defaults; hub re-enters setup mode");
    return err;
}

}  // namespace

esp_err_t InitSecureNvs() {
    // Registered by nvs_sec_provider's system init (CONFIG_NVS_SEC_KEY_PROTECT_USING_HMAC).
    nvs_sec_scheme_t* scheme = nvs_flash_get_default_security_scheme();
    if (scheme == nullptr || scheme->nvs_flash_read_cfg == nullptr) {
        ESP_LOGE(TAG, "NVS security scheme not registered (check sdkconfig NVS_SEC_*)");
        return ESP_ERR_INVALID_STATE;
    }

    nvs_sec_cfg_t cfg = {};
    esp_err_t err = nvs_flash_read_security_cfg_v2(scheme, &cfg);
    if (err == ESP_OK) {
        // Key already in eFuse: normal encrypted boot.
        return InitEncryptedWithRecovery();
    }
    if (err != ESP_ERR_NVS_SEC_HMAC_KEY_NOT_FOUND) {
        ESP_LOGE(TAG, "NVS security cfg read failed: %s", esp_err_to_name(err));
        return InitEncryptedWithRecovery();
    }

    // First encrypted boot (runs once): snapshot plaintext values, burn the
    // eFuse key, wipe the plaintext partition, re-init encrypted, write back.
    ESP_LOGW(TAG, "eFuse HMAC key absent — one-shot plaintext NVS migration");
    std::vector<NvsEntry> snapshot;
    ReadPlaintextSnapshot(snapshot);

    err = nvs_flash_generate_keys_v2(scheme, &cfg);  // burns the key (dev mode)
    if (err != ESP_OK) {
        // Flash content stays intact so a later boot can retry; the RAM handle
        // is closed so this boot's NVS writes fail closed (Settings fail soft).
        ESP_LOGE(TAG, "NVS key generation (eFuse burn) failed: %s", esp_err_to_name(err));
        nvs_flash_deinit_partition(NVS_DEFAULT_PART_NAME);
        return err;
    }
    // Key burned: power loss before the write-back loses the values
    // (next boot re-enters setup mode).
    err = nvs_flash_erase();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_flash_erase failed: %s", esp_err_to_name(err));
        return err;
    }
    err = InitEncryptedWithRecovery();
    if (err != ESP_OK) {
        return err;
    }
    const size_t restored = WriteSnapshotBack(snapshot);
    ESP_LOGI(TAG, "NVS migration complete (%u value(s) re-stored encrypted)",
             static_cast<unsigned>(restored));
    return ESP_OK;
}

}  // namespace boot
