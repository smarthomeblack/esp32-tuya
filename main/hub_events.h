#pragma once

#include <cstdint>

/// Event bits used to wake the main event loop.
constexpr uint32_t MAIN_EVENT_SCHEDULE = 1u << 0;
constexpr uint32_t MAIN_EVENT_CLOCK_TICK = 1u << 2;  ///< 1Hz housekeeping tick
