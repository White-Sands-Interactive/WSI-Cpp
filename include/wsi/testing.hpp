#pragma once

#include "wsi/agent_server.hpp"

#include <memory>
#include <string>
#include <vector>

namespace wsi::testing {

struct ToolCall {
    std::string name;
    Json arguments = Json::object();
};

struct ModelDecision {
    ToolCall call;
    TokenUsage usage{1, 1};
};

/// Ask the model to choose exactly one tool call. Used by the offline loop.
class Model {
 public:
    virtual ~Model() = default;
    virtual ModelDecision decide(const std::string& instructions, const Json& tools, const Json& messages) = 0;
};

inline ToolCall call(std::string name, Json arguments = Json::object()) {
    return ToolCall{std::move(name), std::move(arguments)};
}

/// Inline platform. Every session shares the script, in order.
/// When the script runs out, the stand-in model calls wait.
std::shared_ptr<InlinePlatform> create_test_platform(std::vector<ToolCall> script);

/// Same loop, with a caller-supplied model. Sessions share that model.
std::shared_ptr<InlinePlatform> create_test_platform(std::shared_ptr<Model> model);

}  // namespace wsi::testing
