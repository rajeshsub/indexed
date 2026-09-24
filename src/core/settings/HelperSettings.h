#pragma once

#include "platform/Elevation.h"
#include "settings/Settings.h"
#include <string>

namespace indexed {

// Reads settings on behalf of the root helper without opening indexed.conf
// by plain path (docs/adr/0008): a FIFO or device planted there would hang
// the helper or be opened by root. `out` is replaced only by a successful
// read, so on any refusal the caller keeps what it had (defaults at
// startup, the previous settings on a reload); the error is returned for
// the caller to report (kOpenFailed covers the ordinary missing-file case).
ElevationError LoadSettingsForRoot(const std::string& configPath, const TargetUser& user,
                                   Settings& out);

}  // namespace indexed
