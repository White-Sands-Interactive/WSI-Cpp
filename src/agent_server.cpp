#include "wsi/agent_server.hpp"

#include "log.hpp"
#include "memory.hpp"
#include "platform_connection.hpp"
#include "turn_gate.hpp"
#include "uuid.hpp"

#include <cctype>
#include <cstdlib>
#include <mutex>
#include <unordered_map>
#include <utility>
#include <vector>

namespace wsi {
namespace {

constexpr int kDefaultTurnTimeoutMs = 60000;

std::string trim_copy(std::string value) {
    auto space = [](unsigned char c) { return std::isspace(c) != 0; };
    while (!value.empty() && space(static_cast<unsigned char>(value.front()))) value.erase(value.begin());
    while (!value.empty() && space(static_cast<unsigned char>(value.back()))) value.pop_back();
    return value;
}

bool is_hex_char(char c) {
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

bool is_character_id(const std::string& id) {
    constexpr int kGroups[] = {8, 4, 4, 4, 12};
    std::size_t index = 0;
    for (int group = 0; group < 5; ++group) {
        if (group > 0) {
            if (index >= id.size() || id[index] != '-') return false;
            ++index;
        }
        for (int count = 0; count < kGroups[group]; ++count) {
            if (index >= id.size() || !is_hex_char(id[index])) return false;
            ++index;
        }
    }
    return index == id.size();
}

std::string quoted(const std::string& value) {
    std::string out = "\"";
    for (const unsigned char c : value) {
        if (c == '\\' || c == '"') {
            out.push_back('\\');
            out.push_back(static_cast<char>(c));
        } else if (c == '\n') {
            out += "\\n";
        } else if (c == '\r') {
            out += "\\r";
        } else if (c == '\t') {
            out += "\\t";
        } else {
            out.push_back(static_cast<char>(c));
        }
    }
    out.push_back('"');
    return out;
}

std::string require_player_id(const std::optional<std::string>& raw, bool required) {
    const std::string id = trim_copy(raw.value_or(""));
    if (id.empty()) {
        if (required) {
            throw std::runtime_error("playerId is required. Copy the character ID from the web app");
        }
        return "";
    }
    if (!is_character_id(id)) {
        throw std::runtime_error("playerId " + quoted(id) +
                                 " is not a character ID. Copy the ID from the character in the web app");
    }
    return id;
}

std::string join_instructions(const std::optional<std::string>& server, const std::optional<std::string>& start) {
    const std::string base = server ? *server : default_agent_instructions();
    std::string joined;
    if (!base.empty()) joined = base;
    if (start && !start->empty()) {
        if (!joined.empty()) joined += "\n\n";
        joined += *start;
    }
    return joined;
}

TurnReport turn_report_from_wire(const Json& data) {
    TurnReport report;
    report.ok = data.value("ok", false);
    if (data.contains("error") && data.at("error").is_string()) report.error = data.at("error").get<std::string>();
    if (data.contains("action") && data.at("action").is_string()) report.action = data.at("action").get<std::string>();
    if (data.contains("args") && data.at("args").is_object()) report.args = data.at("args");
    if (data.contains("result")) report.result = data.at("result");
    report.attempts = data.value("attempts", 0);
    if (data.contains("attemptDetails") && data.at("attemptDetails").is_array()) {
        for (const auto& item : data.at("attemptDetails")) {
            if (!item.is_object()) continue;
            Attempt attempt;
            attempt.action = item.value("action", "");
            if (item.contains("args") && item.at("args").is_object()) attempt.args = item.at("args");
            attempt.error = item.value("error", "");
            report.attempt_details.push_back(std::move(attempt));
        }
    }
    if (data.contains("usage") && data.at("usage").is_object()) {
        TokenUsage usage;
        usage.prompt_tokens = data.at("usage").value("promptTokens", 0);
        usage.completion_tokens = data.at("usage").value("completionTokens", 0);
        report.usage = usage;
    }
    report.connection_lost = data.value("connectionLost", false);
    return report;
}

AgentEvent event_from_wire(const Json& envelope) {
    AgentEvent event;
    event.session_id = envelope.value("sessionId", "");
    const std::string name = envelope.value("event", "");
    Json args = Json::object();
    const bool has_args = envelope.contains("args") && envelope.at("args").is_object();
    if (has_args) args = envelope.at("args");
    if (name == "action") {
        event.type = "action";
        event.action = envelope.value("action", "unknown");
        event.args = std::move(args);
        return event;
    }
    if (name == "retry") {
        event.type = "retry";
        event.action = envelope.value("action", "unknown");
        event.args = std::move(args);
        event.error = envelope.value("error", "action failed");
        return event;
    }
    if (name == "subgoal") {
        event.type = "subgoal";
        event.action = envelope.value("action", "unknown");
        event.args = std::move(args);
        if (envelope.contains("error") && envelope.at("error").is_string() &&
            !envelope.at("error").get<std::string>().empty()) {
            event.error = envelope.at("error").get<std::string>();
        }
        return event;
    }
    event.type = "turnFailed";
    event.error = envelope.value("error", "turn failed");
    if (envelope.contains("action") && envelope.at("action").is_string() &&
        !envelope.at("action").get<std::string>().empty()) {
        event.action = envelope.at("action").get<std::string>();
    }
    if (has_args) event.args = std::move(args);
    return event;
}

}  // namespace

struct AbortSignal::State {
    std::mutex mu;
    bool aborted = false;
    std::string reason;
    int next_id = 1;
    std::vector<std::pair<int, std::function<void()>>> listeners;
};

AbortSignal::AbortSignal() : state_(std::make_shared<State>()) {}
AbortSignal::~AbortSignal() = default;

void AbortSignal::abort(const std::string& reason) {
    std::vector<std::function<void()>> listeners;
    {
        std::lock_guard lock(state_->mu);
        if (state_->aborted) return;
        state_->aborted = true;
        state_->reason = reason.empty() ? "turn aborted" : reason;
        for (const auto& [id, listener] : state_->listeners) listeners.push_back(listener);
    }
    for (const auto& listener : listeners) listener();
}

bool AbortSignal::aborted() const {
    std::lock_guard lock(state_->mu);
    return state_->aborted;
}

std::string AbortSignal::reason() const {
    std::lock_guard lock(state_->mu);
    return state_->aborted ? state_->reason : "";
}

Subscription AbortSignal::on_abort(std::function<void()> listener) {
    int id = 0;
    {
        std::lock_guard lock(state_->mu);
        id = state_->next_id++;
        state_->listeners.emplace_back(id, std::move(listener));
    }
    std::weak_ptr<State> weak = state_;
    return Subscription([weak, id] {
        auto state = weak.lock();
        if (!state) return;
        std::lock_guard lock(state->mu);
        std::erase_if(state->listeners, [&](const auto& item) { return item.first == id; });
    });
}

struct AgentServer::State : std::enable_shared_from_this<State> {
    struct Session {
        std::string id;
        std::string token;
        std::string name;
        std::shared_ptr<TurnGate> gate;
        std::shared_ptr<detail::SessionMcpServer> server;
        std::shared_ptr<detail::SessionMuxTransport> mux;
        std::mutex listener_mu;
        int next_listener = 1;
        std::vector<std::pair<int, EventListener>> listeners;
    };

    std::string key;
    std::string url;
    std::optional<std::string> instructions;
    std::optional<int> turn_timeout_ms;
    Logger logger;
    bool memory_enabled = true;
    std::optional<std::string> memory_path;
    std::shared_ptr<InlinePlatform> inline_platform;
    std::shared_ptr<PlatformConnection> connection;
    std::shared_ptr<SessionMemory> memory;
    std::mutex mu;
    std::unordered_map<std::string, std::shared_ptr<Session>> sessions;
    int next_listener = 1;
    std::vector<std::pair<int, EventListener>> listeners;
    bool started = false;
    bool closing = false;

    void ensure_connected();
    void emit(const AgentEvent& event);
    void stop_session(const std::string& session_id);
    std::shared_ptr<SessionMemory> action_memory();
    void on_envelope(const Json& envelope);
    void on_connection_error(std::exception_ptr error);
    std::vector<std::pair<std::string, std::string>> resumable();
};

struct AgentHandle::Access {
    std::weak_ptr<AgentServer::State> server;
    std::string session_id;
};

void AgentServer::State::ensure_connected() {
    if (inline_platform || started) {
        started = true;
        return;
    }
    auto self = shared_from_this();
    auto next = std::make_shared<PlatformConnection>(
        url, key, logger, [self](const Json& envelope) { self->on_envelope(envelope); },
        [self] { self->emit(AgentEvent{"disconnected", std::nullopt, std::nullopt, std::nullopt, std::nullopt}); },
        [self] { self->emit(AgentEvent{"reconnected", std::nullopt, std::nullopt, std::nullopt, std::nullopt}); },
        [self](std::exception_ptr error) { self->on_connection_error(std::move(error)); },
        [self] { return self->resumable(); });
    {
        std::lock_guard lock(mu);
        connection = next;
    }
    next->connect();
    started = true;
}

void AgentServer::State::emit(const AgentEvent& event) {
    std::vector<EventListener> server_listeners;
    std::shared_ptr<Session> session;
    {
        std::lock_guard lock(mu);
        for (const auto& [id, listener] : listeners) server_listeners.push_back(listener);
        if (event.session_id) {
            auto found = sessions.find(*event.session_id);
            if (found != sessions.end()) session = found->second;
        }
    }
    for (const auto& listener : server_listeners) listener(event);
    if (event.type == "disconnected" || event.type == "reconnected" || !event.session_id || !session) return;
    std::vector<EventListener> session_listeners;
    {
        std::lock_guard lock(session->listener_mu);
        for (const auto& [id, listener] : session->listeners) session_listeners.push_back(listener);
    }
    for (const auto& listener : session_listeners) listener(event);
}

void AgentServer::State::stop_session(const std::string& session_id) {
    std::shared_ptr<Session> session;
    {
        std::lock_guard lock(mu);
        auto found = sessions.find(session_id);
        if (found == sessions.end()) return;
        session = found->second;
        sessions.erase(found);
    }
    if (inline_platform) inline_platform->prepare_stop(session_id);
    session->gate->reset();
    if (inline_platform) {
        inline_platform->stop_session(session_id);
    } else if (connection) {
        try {
            connection->begin_wait(session_id, "stop");
            connection->send(Json{{"type", "stop_session"}, {"sessionId", session_id}});
            connection->wait(session_id, "stop");
        } catch (...) {
            connection->cancel_wait(session_id, "stop");
        }
    }
    session->server->close();
    std::shared_ptr<SessionMemory> store;
    {
        std::lock_guard lock(mu);
        store = memory;
    }
    if (store) store->forget(session_id);
}

std::shared_ptr<SessionMemory> AgentServer::State::action_memory() {
    if (!memory_enabled) return nullptr;
    std::lock_guard lock(mu);
    if (!memory) {
        const std::string path = memory_path ? *memory_path : default_memory_path();
        memory = std::make_shared<SessionMemory>(path);
    }
    return memory;
}

void AgentServer::State::on_envelope(const Json& envelope) {
    const std::string type = envelope.value("type", "");
    const std::string session_id = envelope.value("sessionId", "");
    std::shared_ptr<Session> session;
    if (!session_id.empty()) {
        std::lock_guard lock(mu);
        auto found = sessions.find(session_id);
        if (found != sessions.end()) session = found->second;
    }
    if (type == "mcp") {
        if (session && session->mux && envelope.contains("message")) session->mux->receive(envelope.at("message"));
        return;
    }
    if (type == "turn_result") {
        if (session) session->gate->complete_turn(turn_report_from_wire(envelope));
        return;
    }
    if (type == "session_event") {
        emit(event_from_wire(envelope));
        return;
    }
    if (type == "error") log_line(logger, "error", envelope.value("message", ""));
}

void AgentServer::State::on_connection_error(std::exception_ptr error) {
    try {
        std::rethrow_exception(error);
    } catch (const std::exception& ex) {
        log_line(logger, "error", ex.what());
    } catch (...) {
    }
    try {
        std::rethrow_exception(error);
    } catch (const ConnectionLostError& ex) {
        std::vector<std::shared_ptr<Session>> open;
        {
            std::lock_guard lock(mu);
            for (const auto& [id, session] : sessions) open.push_back(session);
        }
        auto lost = std::make_exception_ptr(ex);
        for (const auto& session : open) session->gate->fail_turn(lost);
    } catch (...) {
    }
}

std::vector<std::pair<std::string, std::string>> AgentServer::State::resumable() {
    std::lock_guard lock(mu);
    std::vector<std::pair<std::string, std::string>> open;
    open.reserve(sessions.size());
    for (const auto& [id, session] : sessions) open.emplace_back(session->id, session->token);
    return open;
}

AgentServer::AgentServer(AgentServerOptions options) {
    std::optional<std::string> key = options.platform_key;
    if (!key) {
        if (const char* env = std::getenv("WSI_PLATFORM_KEY")) key = std::string(env);
    }
    if (key && key->empty()) key.reset();
    if (!options.inline_platform && !key) {
        throw std::runtime_error("Set WSI_PLATFORM_KEY or pass platform_key, unless an inline platform is provided");
    }
    state_ = std::make_shared<State>();
    state_->key = key.value_or("inline");
    state_->url = options.platform_url && !options.platform_url->empty() ? *options.platform_url : std::string(kPlatformUrl);
    state_->instructions = std::move(options.instructions);
    state_->turn_timeout_ms = options.turn_timeout_ms;
    state_->logger = std::move(options.logger);
    state_->memory_enabled = options.memory;
    state_->memory_path = std::move(options.memory_path);
    state_->inline_platform = std::move(options.inline_platform);
    if (options.on_event) state_->listeners.emplace_back(state_->next_listener++, std::move(options.on_event));
}

AgentServer::~AgentServer() {
    try {
        close();
    } catch (...) {
    }
}

AgentServer::AgentServer(AgentServer&& other) noexcept : state_(std::move(other.state_)) {}

AgentServer& AgentServer::operator=(AgentServer&& other) noexcept {
    if (this != &other) {
        close();
        state_ = std::move(other.state_);
    }
    return *this;
}

AgentHandle AgentServer::start(Player& player, const StartOptions& options) {
    if (!state_) throw std::runtime_error("AgentServer is closed");
    state_->ensure_connected();
    const std::string player_id = require_player_id(options.player_id, state_->inline_platform == nullptr);
    ReflectedPlayer reflected = player.reflect();
    const std::string session_id = uuid_v4();
    const std::string session_token = uuid_v4();
    auto gate = std::make_shared<TurnGate>();
    std::string player_name = options.name.value_or(player.type_name());
    if (player_name.empty()) player_name = "player";
    std::shared_ptr<detail::SessionActionLog> actions;
    if (auto memory = state_->action_memory()) {
        actions = std::make_shared<detail::SessionActionLog>(std::move(memory), session_id, player_name);
    }
    const Json tools = reflected.tools;
    auto server = std::make_shared<detail::SessionMcpServer>(std::move(reflected), gate, "agent-player:" + player_name,
                                                            std::move(actions));
    auto session = std::make_shared<State::Session>();
    session->id = session_id;
    session->token = session_token;
    session->name = player_name;
    session->gate = gate;
    session->server = server;
    const std::string prompt = join_instructions(state_->instructions, options.instructions);

    if (state_->inline_platform) {
        auto [server_transport, client_transport] = detail::InMemoryTransport::create_linked_pair();
        server->connect(server_transport);
        {
            std::lock_guard lock(state_->mu);
            state_->sessions.emplace(session_id, session);
        }
        StartSession request;
        request.session_id = session_id;
        request.session_token = session_token;
        request.name = player_name;
        request.tools = tools;
        request.instructions = prompt;
        if (options.goal && !options.goal->empty()) request.goal = options.goal;
        request.player_id = player_id;
        request.on_turn_result = [gate](const TurnReport& report) { gate->complete_turn(report); };
        request.on_event = [state = state_](const AgentEvent& event) { state->emit(event); };
        request.on_usage = {};
        request.logger = state_->logger;
        request.transport = client_transport;
        try {
            state_->inline_platform->start_session(request);
        } catch (...) {
            state_->stop_session(session_id);
            throw;
        }
    } else {
        if (!state_->connection) throw std::runtime_error("Not connected");
        auto mux = state_->connection->create_transport(session_id);
        server->connect(mux);
        session->mux = mux;
        {
            std::lock_guard lock(state_->mu);
            state_->sessions.emplace(session_id, session);
        }
        try {
            state_->connection->begin_wait(session_id, "start");
            Json body = Json{{"type", "start_session"}, {"sessionId", session_id}, {"sessionToken", session_token},
                             {"name", player_name},     {"tools", tools},           {"instructions", prompt},
                             {"playerId", player_id}};
            if (options.goal && !options.goal->empty()) body["goal"] = *options.goal;
            state_->connection->send(body);
            state_->connection->wait(session_id, "start");
        } catch (...) {
            state_->stop_session(session_id);
            throw;
        }
    }

    AgentHandle handle;
    handle.session_id_ = session_id;
    handle.state_ = std::make_shared<AgentHandle::Access>();
    handle.state_->server = state_;
    handle.state_->session_id = session_id;
    return handle;
}

Subscription AgentServer::on(EventListener listener) {
    if (!state_) return Subscription{};
    int id = 0;
    {
        std::lock_guard lock(state_->mu);
        id = state_->next_listener++;
        state_->listeners.emplace_back(id, std::move(listener));
    }
    std::weak_ptr<State> weak = state_;
    return Subscription([weak, id] {
        auto state = weak.lock();
        if (!state) return;
        std::lock_guard lock(state->mu);
        std::erase_if(state->listeners, [&](const auto& item) { return item.first == id; });
    });
}

void AgentServer::interrupt() {
    if (!state_) return;
    std::shared_ptr<PlatformConnection> connection;
    {
        std::lock_guard lock(state_->mu);
        connection = state_->connection;
    }
    if (connection) connection->close();
}

void AgentServer::close() {
    auto state = state_;
    if (!state) return;
    {
        std::lock_guard lock(state->mu);
        if (state->closing) return;
        state->closing = true;
    }
    try {
        std::vector<std::string> ids;
        {
            std::lock_guard lock(state->mu);
            for (const auto& [id, session] : state->sessions) ids.push_back(id);
        }
        for (const auto& id : ids) state->stop_session(id);
        if (state->inline_platform) {
            try {
                state->inline_platform->close();
            } catch (...) {
            }
        }
        if (state->connection) {
            state->connection->close();
            state->connection.reset();
        }
        if (state->memory) {
            state->memory->close();
            state->memory.reset();
        }
        state->started = false;
    } catch (...) {
        std::lock_guard lock(state->mu);
        state->closing = false;
        throw;
    }
    std::lock_guard lock(state->mu);
    state->closing = false;
}

const std::string& AgentHandle::session_id() const { return session_id_; }

TurnResult AgentHandle::turn(const TurnOptions& options) {
    if (!state_) throw std::runtime_error("Unknown session " + session_id_);
    auto server = state_->server.lock();
    if (!server) throw std::runtime_error("Unknown session " + session_id_);
    std::shared_ptr<AgentServer::State::Session> session;
    {
        std::lock_guard lock(server->mu);
        auto found = server->sessions.find(session_id_);
        if (found == server->sessions.end()) throw std::runtime_error("Unknown session " + session_id_);
        session = found->second;
    }
    const int timeout = options.timeout_ms.value_or(server->turn_timeout_ms.value_or(kDefaultTurnTimeoutMs));
    const TurnReport report = session->gate->run_turn(timeout, options.signal);
    if (!report.ok) {
        if (report.connection_lost) {
            throw ConnectionLostError(report.error.value_or("Connection to the platform was lost"));
        }
        throw TurnFailedError(report.error.value_or("turn failed"), report.attempt_details);
    }
    TurnResult result;
    result.action = report.action.value_or("unknown");
    result.args = report.args.value_or(Json::object());
    result.result = report.result.value_or(Json(nullptr));
    result.attempts = report.attempts == 0 ? 1 : report.attempts;
    result.usage = report.usage.value_or(TokenUsage{});
    return result;
}

void AgentHandle::stop() {
    if (!state_) return;
    auto server = state_->server.lock();
    if (!server) return;
    server->stop_session(session_id_);
}

Subscription AgentHandle::on(EventListener listener) {
    if (!state_) return Subscription{};
    auto server = state_->server.lock();
    if (!server) return Subscription{};
    std::shared_ptr<AgentServer::State::Session> session;
    {
        std::lock_guard lock(server->mu);
        auto found = server->sessions.find(session_id_);
        if (found == server->sessions.end()) return Subscription{};
        session = found->second;
    }
    int id = 0;
    {
        std::lock_guard lock(session->listener_mu);
        id = session->next_listener++;
        session->listeners.emplace_back(id, std::move(listener));
    }
    std::weak_ptr<AgentServer::State::Session> weak = session;
    return Subscription([weak, id] {
        auto current = weak.lock();
        if (!current) return;
        std::lock_guard lock(current->listener_mu);
        std::erase_if(current->listeners, [&](const auto& item) { return item.first == id; });
    });
}

}  // namespace wsi
