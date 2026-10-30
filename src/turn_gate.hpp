#pragma once

#include "wsi/agent_server.hpp"

#include <atomic>
#include <condition_variable>
#include <exception>
#include <memory>
#include <mutex>
#include <optional>

namespace wsi {

class TurnGate {
 public:
    TurnGate();
    void wait_for_turn();
    TurnReport run_turn(std::optional<int> timeout_ms, AbortSignal* signal);
    void complete_turn(const TurnReport& report);
    void fail_turn(std::exception_ptr error);
    void reset();
    bool stopped() const;

 private:
    struct Impl;
    std::shared_ptr<Impl> impl_;
};

}  // namespace wsi
