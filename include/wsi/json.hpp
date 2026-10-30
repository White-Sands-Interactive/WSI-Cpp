#pragma once

#include <nlohmann/json.hpp>

namespace wsi {

/// JSON values on the player boundary and on the wire. Key order follows insertion.
using Json = nlohmann::ordered_json;

}  // namespace wsi
