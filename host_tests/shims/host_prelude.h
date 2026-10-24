#pragma once

// Force-included in every TU: the ESP32 target guarantees freestanding
// <stdint.h>/<stddef.h> (the IDF build pulls them via its generated config
// include), and some component headers rely on that.

#include <stddef.h>
#include <stdint.h>
