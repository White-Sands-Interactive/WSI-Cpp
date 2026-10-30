#pragma once

#include "wsi/logger.hpp"

namespace wsi {

inline void log_line(const Logger& logger, const char* level, const std::string& message) {
    if (level == nullptr) return;
    const std::string name(level);
    if (name == "debug" && logger.debug) logger.debug(message);
    else if (name == "info" && logger.info) logger.info(message);
    else if (name == "warn" && logger.warn) logger.warn(message);
    else if (name == "error" && logger.error) logger.error(message);
}

}  // namespace wsi
