#include "session_loop.hpp"

#include "log.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

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

constexpr int kMaxActiveSubgoals = 8;
constexpr int kMaxSubgoalRunes = 240;
constexpr int kMaxProgressNotes = 5;
constexpr int kMaxSubgoalCallsPerTurn = 4;

constexpr const char* kSubgoalInstructions =
    "Subgoals last until you complete them, and only while this session is alive. The session goal stays for the "
    "whole session. Call add_subgoal to add one, update_subgoal to record progress, and complete_subgoal to retire "
    "it. Those calls are not a game move. After them, call exactly one game action.";

bool is_subgoal_tool(const std::string& name) {
    return name == "add_subgoal" || name == "update_subgoal" || name == "complete_subgoal";
}

bool is_state_tool(const std::string& name) { return name == "wait" || name == "see"; }

std::size_t utf8_length(const std::string& text) {
    std::size_t count = 0;
    for (unsigned char c : text) {
        if ((c & 0xC0) != 0x80) ++count;
    }
    return count;
}

std::string trim_copy(std::string text) {
    const auto not_space = [](unsigned char c) { return std::isspace(c) == 0; };
    text.erase(text.begin(), std::find_if(text.begin(), text.end(), not_space));
    text.erase(std::find_if(text.rbegin(), text.rend(), not_space).base(), text.end());
    return text;
}

std::string arg_string(const Json& args, const std::string& key) {
    if (!args.is_object() || !args.contains(key) || args.at(key).is_null()) return "";
    const Json& value = args.at(key);
    if (value.is_string()) return trim_copy(value.get<std::string>());
    if (value.is_number()) {
        const double number = value.get<double>();
        if (number >= 0 && number < 1e15 && number == std::trunc(number)) {
            return std::to_string(static_cast<long long>(number));
        }
    }
    return "";
}

struct Subgoal {
    std::string id;
    std::string objective;
    std::vector<std::string> notes;
};

struct SubgoalBoard {
    std::vector<Subgoal> items;
    int next = 0;
    int calls = 0;

    void begin_turn() { calls = 0; }
    bool open() const { return calls < kMaxSubgoalCallsPerTurn; }

    std::string list_text() const {
        if (items.empty()) return "Active subgoals: none.";
        std::string text = "Active subgoals:";
        for (const auto& item : items) {
            text += "\n" + item.id + ". " + item.objective;
            text += "\n   progress: ";
            if (item.notes.empty()) {
                text += "none";
            } else {
                for (std::size_t i = 0; i < item.notes.size(); ++i) {
                    if (i != 0) text += "; ";
                    text += item.notes[i];
                }
            }
        }
        return text;
    }

    /// A call past the per-turn cap is refused and does not change the board.
    /// Other rejections still count toward the cap. The string result is the
    /// model-facing text, the id, and the error. An empty error means success.
    std::tuple<std::string, std::string, std::string> apply(const std::string& name, const Json& args) {
        if (!open()) return {"", "", "subgoal tools are closed for this turn"};
        ++calls;
        if (name == "add_subgoal") return add(arg_string(args, "objective"));
        if (name == "update_subgoal") return update(arg_string(args, "id"), arg_string(args, "progress"));
        if (name == "complete_subgoal") return complete(arg_string(args, "id"));
        return {"", "", "unknown subgoal tool " + name};
    }

    std::tuple<std::string, std::string, std::string> add(const std::string& raw) {
        const std::string objective = trim_copy(raw);
        if (objective.empty()) return {"", "", "objective is required"};
        if (utf8_length(objective) > static_cast<std::size_t>(kMaxSubgoalRunes)) {
            return {"", "", "objective must be " + std::to_string(kMaxSubgoalRunes) + " characters or less"};
        }
        if (static_cast<int>(items.size()) >= kMaxActiveSubgoals) {
            return {"", "", std::to_string(kMaxActiveSubgoals) +
                                " active subgoals is the maximum. Complete one before adding another."};
        }
        ++next;
        const std::string id = std::to_string(next);
        items.push_back(Subgoal{id, objective, {}});
        return {"Added subgoal " + id + ": " + objective + ".", id, ""};
    }

    std::tuple<std::string, std::string, std::string> update(const std::string& id, const std::string& raw) {
        if (id.empty()) return {"", "", "id is required"};
        const std::string progress = trim_copy(raw);
        if (progress.empty()) return {"", id, "progress is required"};
        if (utf8_length(progress) > static_cast<std::size_t>(kMaxSubgoalRunes)) {
            return {"", id, "progress must be " + std::to_string(kMaxSubgoalRunes) + " characters or less"};
        }
        Subgoal* item = find(id);
        if (item == nullptr) return {"", id, "no active subgoal with id " + id};
        item->notes.push_back(progress);
        if (static_cast<int>(item->notes.size()) > kMaxProgressNotes) {
            item->notes.erase(item->notes.begin(), item->notes.end() - kMaxProgressNotes);
        }
        return {"Updated subgoal " + id + ".", id, ""};
    }

    std::tuple<std::string, std::string, std::string> complete(const std::string& id) {
        if (id.empty()) return {"", "", "id is required"};
        for (auto it = items.begin(); it != items.end(); ++it) {
            if (it->id != id) continue;
            const std::string objective = it->objective;
            items.erase(it);
            return {"Completed subgoal " + id + " (" + objective + "). It is no longer active.", id, ""};
        }
        return {"", id, "no active subgoal with id " + id};
    }

    Subgoal* find(const std::string& id) {
        for (auto& item : items) {
            if (item.id == id) return &item;
        }
        return nullptr;
    }
};

Json subgoal_tools() {
    return Json::array({
        Json{{"name", "add_subgoal"},
             {"description", "Add a subgoal for this session. It stays until you complete it. This is not a game move."},
             {"inputSchema",
              Json{{"type", "object"},
                   {"properties", Json{{"objective", Json{{"type", "string"}, {"description", "What this subgoal is, in one sentence."}}}}},
                   {"required", Json::array({"objective"})},
                   {"additionalProperties", false}}}},
        Json{{"name", "update_subgoal"},
             {"description", "Record progress on an active subgoal. This is not a game move."},
             {"inputSchema",
              Json{{"type", "object"},
                   {"properties",
                    Json{{"id", Json{{"type", "string"}}},
                         {"progress", Json{{"type", "string"}, {"description", "What has happened toward this subgoal."}}}}},
                   {"required", Json::array({"id", "progress"})},
                   {"additionalProperties", false}}}},
        Json{{"name", "complete_subgoal"},
             {"description", "Complete an active subgoal and drop it. This is not a game move."},
             {"inputSchema",
              Json{{"type", "object"},
                   {"properties", Json{{"id", Json{{"type", "string"}}}}},
                   {"required", Json::array({"id"})},
                   {"additionalProperties", false}}}},
    });
}

Json decision_tools(const Json& actions, bool open) {
    Json tools = actions;
    if (!open) return tools;
    for (const auto& tool : subgoal_tools()) tools.push_back(tool);
    return tools;
}

std::string turn_prompt(const std::string& view, const std::string& goals) {
    return "It is your turn. Current view:\n" + view + "\n\n" + goals +
           "\n\nYou may call add_subgoal, update_subgoal, or complete_subgoal. Those are not a move.\n"
           "Otherwise call exactly one action tool.";
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
    if (name == "subgoal") {
        out.type = "subgoal";
        out.action = event.value("action", "unknown");
        out.args = event.contains("args") && event.at("args").is_object() ? event.at("args") : Json::object();
        if (event.contains("error") && event.at("error").is_string() && !event.at("error").get<std::string>().empty()) {
            out.error = event.at("error").get<std::string>();
        }
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
            if (is_state_tool(name) || is_subgoal_tool(name)) continue;
            action_tools.push_back(tool);
        }
    }
    SubgoalBoard subgoals;
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
        subgoals.begin_turn();
        Json messages = Json::array(
            {Json{{"role", "user"}, {"content", turn_prompt(wait_result.text, subgoals.list_text())}}});

        for (;;) {
            testing::ToolCall call = decide(decision_tools(action_tools, subgoals.open()), messages, usage);
            if (is_subgoal_tool(call.name) && subgoals.open()) {
                const auto [text, id, call_error] = subgoals.apply(call.name, call.arguments);
                Json args = call.arguments.is_object() ? call.arguments : Json::object();
                if (!id.empty()) args["id"] = id;
                Json event = Json{{"event", "subgoal"}, {"action", call.name}, {"args", args}};
                std::string content = text;
                if (!call_error.empty()) {
                    event["error"] = call_error;
                    content = call_error;
                    log_line(logger_, "info", "subgoal rejected " + call.name + ": " + call_error);
                } else {
                    log_line(logger_, "info", "subgoal updated " + call.name + " id " + id);
                }
                emit(event);
                content += "\n\n" + subgoals.list_text();
                messages.push_back(Json{{"role", "assistant"},
                                        {"content", "Called " + call.name + "(" + call.arguments.dump() + ")"}});
                messages.push_back(Json{{"role", "tool"},
                                        {"name", call.name},
                                        {"content", content},
                                        {"isError", !call_error.empty()}});
                if (!subgoals.open()) {
                    messages.push_back(Json{{"role", "user"},
                                            {"content",
                                             "Subgoal tools are closed for this turn. Call exactly one game action. "
                                             "Do not call wait or see."}});
                }
                continue;
            }

            std::string action_error;
            detail::ToolPayload result;
            bool failed = false;
            if (is_subgoal_tool(call.name)) {
                action_error = "subgoal tools are closed for this turn";
                failed = true;
            } else if (is_state_tool(call.name)) {
                action_error = call.name + " is not an action";
                failed = true;
            } else {
                emit(Json{{"event", "action"}, {"action", call.name}, {"args", call.arguments}});
                result = call_tool(call.name, call.arguments);
                if (result.is_error) {
                    action_error = tool_error_message(result);
                    failed = true;
                }
            }
            if (!failed) {
                TurnReport report;
                report.ok = true;
                report.action = call.name;
                report.args = call.arguments;
                report.result = result.json;
                report.attempts = static_cast<int>(attempt_details.size()) + 1;
                report.usage = usage;
                if (on_turn_result_) on_turn_result_(report);
                break;
            }
            if (!attempt_details.empty() && (is_state_tool(call.name) || is_subgoal_tool(call.name))) {
                const std::string message = "Retry returned " + call.name + " instead of an action (after: " +
                                            attempt_details.back().error + ")";
                emit(Json{{"event", "turnFailed"}, {"error", message}});
                TurnReport report;
                report.ok = false;
                report.error = message;
                report.attempts = static_cast<int>(attempt_details.size());
                report.attempt_details = attempt_details;
                report.usage = usage;
                if (on_turn_result_) on_turn_result_(report);
                break;
            }

            attempt_details.push_back(Attempt{call.name, call.arguments, action_error});
            if (attempt_details.size() >= 2) {
                log_line(logger_, "warn", "session " + session_id_ + " retry " + call.name + " failed: " + action_error);
                emit(Json{{"event", "turnFailed"},
                          {"action", call.name},
                          {"args", call.arguments},
                          {"error", action_error}});
                TurnReport report;
                report.ok = false;
                report.error = action_error;
                report.attempts = 2;
                report.attempt_details = std::move(attempt_details);
                report.usage = usage;
                if (on_turn_result_) on_turn_result_(report);
                break;
            }

            log_line(logger_, "warn",
                     "session " + session_id_ + " action " + call.name + " failed: " + action_error + "; retrying once");
            emit(Json{{"event", "retry"}, {"action", call.name}, {"args", call.arguments}, {"error", action_error}});
            const std::string tool_text = result.text.empty() ? action_error : result.text;
            messages.push_back(
                Json{{"role", "assistant"}, {"content", "Called " + call.name + "(" + call.arguments.dump() + ")"}});
            messages.push_back(
                Json{{"role", "tool"}, {"name", call.name}, {"content", tool_text}, {"isError", true}});
            messages.push_back(Json{{"role", "user"},
                                    {"content", "Action " + call.name + " failed: " + action_error +
                                                    "\nRetry once with a different legal action (or different arguments)."
                                                    "\nDo not repeat the same failing call. Do not call wait or see."
                                                    "\nCurrent view was:\n" +
                                                    wait_result.text}});
        }
    }
}

testing::ToolCall SessionLoop::decide(const Json& tools, const Json& messages, TokenUsage& usage) {
    std::string instructions = compose_instructions(instructions_, goal_);
    instructions += std::string(instructions.empty() ? "" : "\n\n") + kSubgoalInstructions;
    auto decision = model_->decide(instructions, tools, messages);
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
