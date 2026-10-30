#include "session_loop.hpp"

#include "log.hpp"

#include <utility>

namespace wsi {
namespace {

std::string compose_instructions(const std::string& instructions, const std::optional<std::string>& goal) {
    if (!goal || goal->empty()) return instructions;
    return instructions + "\n\nYour goal: " + *goal;
}

std::string tool_error_message(const detail::ToolPayload& result) {
    if (result.json.is_object() && result.json.contains("error")) {
        const auto& error = result.json.at("error");
        if (error.is_string()) return error.get<std::string>();
        return error.dump();
    }
    return result.text;
}

}  // namespace

AgentEvent loop_event_to_agent_event(const std::string& session_id, const Json& event) {
    const std::string name = event.value("event", "");
    AgentEvent out;
    out.session_id = session_id;
    if (name == "action") {
        out.type = "action";
        out.action = event.value("action", "unknown");
        out.args = event.contains("args") && event.at("args").is_object() ? event.at("args") : Json::object();
        return out;
    }
    if (name == "retry") {
        out.type = "retry";
        out.action = event.value("action", "unknown");
        out.args = event.contains("args") && event.at("args").is_object() ? event.at("args") : Json::object();
        out.error = event.value("error", "action failed");
        return out;
    }
    out.type = "turnFailed";
    out.error = event.value("error", "turn failed");
    if (event.contains("action") && event.at("action").is_string() && !event.at("action").get<std::string>().empty()) {
        out.action = event.at("action").get<std::string>();
    }
    if (event.contains("args") && event.at("args").is_object()) out.args = event.at("args");
    return out;
}

SessionLoop::SessionLoop(std::string session_id, std::shared_ptr<detail::MessageTransport> transport,
                         std::string instructions, std::optional<std::string> goal, std::shared_ptr<testing::Model> model,
                         Logger logger, std::function<void(const TurnReport&)> on_turn_result,
                         std::function<void(const AgentEvent&)> on_event, std::function<void(int)> on_usage)
    : session_id_(std::move(session_id)),
      transport_(std::move(transport)),
      instructions_(std::move(instructions)),
      goal_(std::move(goal)),
      model_(std::move(model)),
      logger_(std::move(logger)),
      on_turn_result_(std::move(on_turn_result)),
      on_event_(std::move(on_event)),
      on_usage_(std::move(on_usage)) {}

SessionLoop::~SessionLoop() { stop(); }

void SessionLoop::start() {
    if (thread_.joinable()) return;
    client_.connect(transport_);
    thread_ = std::thread([this] {
        try {
            run();
        } catch (const std::exception& ex) {
            if (stopped_) return;
            log_line(logger_, "error", "session " + session_id_ + " crashed: " + ex.what());
            TurnReport report;
            report.ok = false;
            report.error = ex.what();
            report.attempts = 0;
            if (on_turn_result_) on_turn_result_(report);
        } catch (...) {
            if (stopped_) return;
        }
    });
}

void SessionLoop::request_stop() { stopped_ = true; }

void SessionLoop::stop() {
    stopped_ = true;
    try {
        client_.close();
    } catch (...) {
    }
    if (thread_.joinable()) thread_.join();
}

void SessionLoop::run() {
    const Json listed = client_.list_tools();
    Json action_tools = Json::array();
    if (listed.is_array()) {
        for (const auto& tool : listed) {
            const std::string name = tool.value("name", "");
            if (name != "wait" && name != "see") action_tools.push_back(tool);
        }
    }
    while (!stopped_) {
        detail::ToolPayload wait_result;
        try {
            wait_result = call_tool("wait", Json::object());
        } catch (const std::exception&) {
            if (stopped_) return;
            throw;
        }
        if (stopped_) return;
        if (wait_result.is_error) {
            TurnReport report;
            report.ok = false;
            report.error = wait_result.text;
            report.attempts = 0;
            if (on_turn_result_) on_turn_result_(report);
            continue;
        }

        TokenUsage usage;
        std::vector<Attempt> attempt_details;
        Json messages = Json::array({Json{{"role", "user"},
                                           {"content", "It is your turn. Current view:\n" + wait_result.text +
                                                           "\nCall exactly one action tool."}}});
        testing::ToolCall action = decide(action_tools, messages, usage);
        detail::ToolPayload result;
        bool have_result = false;
        std::string first_error;
        if (action.name == "wait" || action.name == "see") {
            first_error = action.name + " is not an action";
        } else {
            emit(Json{{"event", "action"}, {"action", action.name}, {"args", action.arguments}});
            result = call_tool(action.name, action.arguments);
            have_result = true;
            if (!result.is_error) {
                TurnReport report;
                report.ok = true;
                report.action = action.name;
                report.args = action.arguments;
                report.result = result.json;
                report.attempts = 1;
                report.usage = usage;
                if (on_turn_result_) on_turn_result_(report);
                continue;
            }
            first_error = tool_error_message(result);
        }

        attempt_details.push_back(Attempt{action.name, action.arguments, first_error});
        log_line(logger_, "warn",
                 "session " + session_id_ + " action " + action.name + " failed: " + first_error + "; retrying once");
        emit(Json{{"event", "retry"}, {"action", action.name}, {"args", action.arguments}, {"error", first_error}});
        messages.push_back(Json{{"role", "assistant"},
                                {"content", "Called " + action.name + "(" + action.arguments.dump() + ")"}});
        messages.push_back(Json{{"role", "tool"},
                                {"name", action.name},
                                {"content", have_result ? result.text : first_error},
                                {"isError", true}});
        messages.push_back(Json{{"role", "user"},
                                {"content", "Action " + action.name + " failed: " + first_error +
                                                "\nRetry once with a different legal action (or different arguments)."
                                                "\nDo not repeat the same failing call. Do not call wait or see."
                                                "\nCurrent view was:\n" +
                                                wait_result.text}});

        testing::ToolCall retry = decide(action_tools, messages, usage);
        if (retry.name == "wait" || retry.name == "see") {
            const std::string error = "Retry returned " + retry.name + " instead of an action (after: " + first_error + ")";
            emit(Json{{"event", "turnFailed"}, {"error", error}});
            TurnReport report;
            report.ok = false;
            report.error = error;
            report.attempts = 1;
            report.attempt_details = attempt_details;
            report.usage = usage;
            if (on_turn_result_) on_turn_result_(report);
            continue;
        }

        emit(Json{{"event", "action"}, {"action", retry.name}, {"args", retry.arguments}});
        result = call_tool(retry.name, retry.arguments);
        if (!result.is_error) {
            TurnReport report;
            report.ok = true;
            report.action = retry.name;
            report.args = retry.arguments;
            report.result = result.json;
            report.attempts = 2;
            report.usage = usage;
            if (on_turn_result_) on_turn_result_(report);
            continue;
        }

        const std::string second_error = tool_error_message(result);
        attempt_details.push_back(Attempt{retry.name, retry.arguments, second_error});
        log_line(logger_, "warn", "session " + session_id_ + " retry " + retry.name + " failed: " + second_error);
        emit(Json{{"event", "turnFailed"},
                  {"action", retry.name},
                  {"args", retry.arguments},
                  {"error", second_error}});
        TurnReport report;
        report.ok = false;
        report.error = second_error;
        report.attempts = 2;
        report.attempt_details = std::move(attempt_details);
        report.usage = usage;
        if (on_turn_result_) on_turn_result_(report);
    }
}

testing::ToolCall SessionLoop::decide(const Json& tools, const Json& messages, TokenUsage& usage) {
    auto decision = model_->decide(compose_instructions(instructions_, goal_), tools, messages);
    usage.prompt_tokens += decision.usage.prompt_tokens;
    usage.completion_tokens += decision.usage.completion_tokens;
    if (on_usage_) on_usage_(decision.usage.prompt_tokens + decision.usage.completion_tokens);
    if (!decision.call.arguments.is_object()) decision.call.arguments = Json::object();
    return decision.call;
}

detail::ToolPayload SessionLoop::call_tool(const std::string& name, const Json& args) {
    return client_.call_tool(name, args);
}

void SessionLoop::emit(const Json& event) {
    if (!on_event_) return;
    on_event_(loop_event_to_agent_event(session_id_, event));
}

}  // namespace wsi
