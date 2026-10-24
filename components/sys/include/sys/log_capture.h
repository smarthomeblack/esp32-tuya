#pragma once

#include "sys/log_buffer.h"

namespace sys {

namespace LogCapture {

/// Installs the log vprintf hook (chain to the previous sink so UART
/// output stays intact) and configures the process-wide ring. Idempotent.
void Start();

/// The process-wide ring (the REST layer reads it).
LogBuffer& Buffer();

}  // namespace LogCapture

}  // namespace sys
