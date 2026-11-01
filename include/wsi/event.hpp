#pragma once

#include "wsi/error.hpp"
#include "wsi/json.hpp"

#include <functional>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace wsi {

/// One action, subgoal change, retry, failure, or connection change.
/// `disconnected` and `reconnected` have no session id.
/// `type` is `action`, `subgoal`, `retry`, `turnFailed`, `disconnected`, or `reconnected`.
/// A `subgoal` event's `args` carries `id` when the platform assigned or received one.
struct AgentEvent {
    std::string type;
    std::optional<std::string> session_id;
    std::optional<std::string> action;
    std::optional<Json> args;
    std::optional<std::string> error;
};

using EventListener = std::function<void(const AgentEvent&)>;

/// Unsubscribes when destroyed.
class Subscription {
 public:
    Subscription() = default;
    explicit Subscription(std::function<void()> unsubscribe) : unsubscribe_(std::move(unsubscribe)) {}

    ~Subscription() { reset(); }

    Subscription(Subscription&& other) noexcept : unsubscribe_(std::move(other.unsubscribe_)) {}

    Subscription& operator=(Subscription&& other) noexcept {
        if (this != &other) {
            reset();
            unsubscribe_ = std::move(other.unsubscribe_);
        }
        return *this;
    }

    Subscription(const Subscription&) = delete;
    Subscription& operator=(const Subscription&) = delete;

    void reset() {
        if (unsubscribe_) {
            auto fn = std::move(unsubscribe_);
            unsubscribe_ = nullptr;
            fn();
        }
    }

 private:
    std::function<void()> unsubscribe_;
};

struct TokenUsage {
    int prompt_tokens = 0;
    int completion_tokens = 0;
};

/// What the agent did on one granted turn.
struct TurnResult {
    std::string action;
    Json args = Json::object();
    Json result = nullptr;
    int attempts = 1;
    TokenUsage usage;
};

/// Outcome of one turn. The platform sends this as `turn_result`.
struct TurnReport {
    bool ok = false;
    std::optional<std::string> error;
    std::optional<std::string> action;
    std::optional<Json> args;
    std::optional<Json> result;
    int attempts = 0;
    std::vector<Attempt> attempt_details;
    std::optional<TokenUsage> usage;
    bool connection_lost = false;
};

}  // namespace wsi
