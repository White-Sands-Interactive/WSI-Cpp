#pragma once

#include "wsi/json.hpp"

#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace wsi {

class Error : public std::runtime_error {
 public:
    explicit Error(const std::string& message) : std::runtime_error(message) {}
};

/// The platform rejected the key.
class PlatformAuthError : public Error {
 public:
    explicit PlatformAuthError(const std::string& message) : Error(message) {}
};

/// This SDK and the platform disagree on the protocol.
class ProtocolVersionError : public Error {
 public:
    explicit ProtocolVersionError(const std::string& message) : Error(message) {}
};

struct Attempt {
    std::string action;
    Json args = Json::object();
    std::string error;
};

/// The action and the retry both failed. attempts() lists both calls.
class TurnFailedError : public Error {
 public:
    TurnFailedError(const std::string& message, std::vector<Attempt> attempts = {})
        : Error(message), attempts_(std::move(attempts)) {}

    const std::vector<Attempt>& attempts() const { return attempts_; }

 private:
    std::vector<Attempt> attempts_;
};

/// The platform did not finish within the turn timeout.
class TurnTimeoutError : public Error {
 public:
    explicit TurnTimeoutError(const std::string& message) : Error(message) {}
};

/// The socket dropped and the session could not be resumed.
class ConnectionLostError : public Error {
 public:
    explicit ConnectionLostError(const std::string& message = "Connection to the platform was lost")
        : Error(message) {}
};

}  // namespace wsi
