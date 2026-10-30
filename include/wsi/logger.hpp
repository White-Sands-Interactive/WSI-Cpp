#pragma once

#include <functional>
#include <string>

namespace wsi {

/// Optional logger. Leave the functions empty and the SDK writes nothing.
struct Logger {
    std::function<void(const std::string& message)> debug;
    std::function<void(const std::string& message)> info;
    std::function<void(const std::string& message)> warn;
    std::function<void(const std::string& message)> error;
};

}  // namespace wsi
