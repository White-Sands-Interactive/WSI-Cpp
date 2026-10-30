#pragma once

#include "mcp.hpp"
#include "wsi/testing.hpp"

#include <atomic>
#include <memory>
#include <thread>

namespace wsi {

AgentEvent loop_event_to_agent_event(const std::string& session_id, const Json& event);

class SessionLoop {
 public:
    SessionLoop(std::string session_id, std::shared_ptr<detail::MessageTransport> transport, std::string instructions,
                std::optional<std::string> goal, std::shared_ptr<testing::Model> model, Logger logger,
                std::function<void(const TurnReport&)> on_turn_result, std::function<void(const AgentEvent&)> on_event,
                std::function<void(int)> on_usage);
    ~SessionLoop();

    void start();
    void request_stop();
    void stop();

 private:
    void run();
    testing::ToolCall decide(const Json& tools, const Json& messages, TokenUsage& usage);
    detail::ToolPayload call_tool(const std::string& name, const Json& args);
    void emit(const Json& event);

    std::string session_id_;
    std::shared_ptr<detail::MessageTransport> transport_;
    std::string instructions_;
    std::optional<std::string> goal_;
    std::shared_ptr<testing::Model> model_;
    Logger logger_;
    std::function<void(const TurnReport&)> on_turn_result_;
    std::function<void(const AgentEvent&)> on_event_;
    std::function<void(int)> on_usage_;
    detail::McpClient client_;
    std::atomic<bool> stopped_{false};
    std::thread thread_;
};

}  // namespace wsi
