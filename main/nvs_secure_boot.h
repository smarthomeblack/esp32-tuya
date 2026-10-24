#pragma once

#include "esp_err.h"

namespace boot {

/// Encrypted-NVS bring-up for the default "nvs" partition. First boot: burn
/// the eFuse HMAC key, migrate the plaintext values, re-init encrypted.
/// Later boots go straight to the secure init. Never aborts: an unrecoverable
/// failure degrades (defaults + setup mode) — callers log and continue.
esp_err_t InitSecureNvs();

}  // namespace boot
