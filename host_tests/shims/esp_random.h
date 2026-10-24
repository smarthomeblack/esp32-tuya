#pragma once

// Host shim shadowing <esp_random.h>. Deterministic on purpose: nonces only
// need validity, and reproducible runs make failures debuggable.

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

void esp_fill_random(void* buf, size_t len);

#ifdef __cplusplus
}
#endif
