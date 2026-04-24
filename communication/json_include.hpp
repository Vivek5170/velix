#pragma once

// Centralized inclusion point for nlohmann::json.
// We always use the vendored copy to ensure reproducible single-version builds.
// Define VELIX_JSON_INCLUDED so callers can guard JSON-only APIs if needed.

#include "../vendor/nlohmann/json.hpp"

#define VELIX_JSON_INCLUDED 1
