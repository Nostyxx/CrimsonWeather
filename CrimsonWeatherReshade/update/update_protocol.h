#pragma once

#include "update_service.h"

#include <string>

namespace update_protocol {

// Parses a successful /api/v1/update response. On failure, info is unchanged.
bool ParseUpdateMetadata(const std::string& body, UpdateCheckInfo& info);

} // namespace update_protocol
