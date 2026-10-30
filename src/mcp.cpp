#include "mcp.hpp"

#include "wsi/protocol.hpp"

#include <utility>

namespace wsi::detail {
namespace {

Json rpc_response(const Json& id, const Json& result) {
    return Json{{"jsonrpc", "2.0"}, {"id", id}, {"result", result}};
}

Json rpc_error(const Json& id, int code, const std::string& message) {
    Json body = Json{{"jsonrpc", "2.0"}, {"error", Json{{"code", code}, {"message", message}}}};
    if (!id.is_null()) body["id"] = id;
    return body;
}

Json rpc_request(int id, const std::string& method, const Json& params) {
    Json message = Json{{"jsonrpc", "2.0"}, {"id", id}, {"method", method}};
    if (!params.is_null()) message["params"] = params;
    return message;
}

Json rpc_notify(const std::string& method) {
    return Json{{"jsonrpc", "2.0"}, {"method", method}};
}

std::string error_text(const std::exception& ex) {
    const char* text = ex.what();
    if (text == nullptr || text[0] == '\0') return "exception";
    return text;
}

}  // namespace

Json json_result(const Json& payload, bool is_error) {
    const Json body = payload.is_null() ? Json{{"ok", true}} : payload;
    return Json{{"isError", is_error},
                {"content", Json::array({Json{{"type", "text"}, {"text", body.dump()}}})}};
}

Json tool_error(const std::string& message) {
    return json_result(Json{{"error", message}}, true);
}

ToolPayload parse_tool_payload(const Json& result) {
    ToolPayload payload;
    if (!result.is_object()) {
        payload.text = result.is_null() ? "" : result.dump();
        payload.json = payload.text;
        return payload;
    }
    std::string text;
    if (result.contains("content") && result.at("content").is_array()) {
        for (const auto& entry : result.at("content")) {
            if (entry.is_object() && entry.value("type", "") == "text" && entry.contains("text") &&
                entry.at("text").is_string()) {
                if (!text.empty()) text.push_back('\n');
                text += entry.at("text").get<std::string>();
            }
        }
    }
    payload.is_error = result.value("isError", false);
    payload.text = text;
    try {
        payload.json = Json::parse(text);
    } catch (const Json::parse_error&) {
        payload.json = text;
    }
    return payload;
}

std::pair<std::shared_ptr<InMemoryTransport>, std::shared_ptr<InMemoryTransport>> InMemoryTransport::create_linked_pair() {
    auto server = std::shared_ptr<InMemoryTransport>(new InMemoryTransport());
    auto client = std::shared_ptr<InMemoryTransport>(new InMemoryTransport());
    server->peer_ = client;
    client->peer_ = server;
    return {server, client};
}

void InMemoryTransport::start() {
    std::vector<Json> pending;
    std::function<void(const Json&)> handler;
    {
        std::lock_guard lock(mu_);
        if (closed_) throw std::runtime_error("Transport already closed");
        started_ = true;
        if (handler_) {
            pending.swap(pending_);
            handler = handler_;
        }
    }
    for (const auto& message : pending) {
        if (handler) handler(message);
    }
}

void InMemoryTransport::close() {
    std::lock_guard lock(mu_);
    closed_ = true;
    handler_ = nullptr;
}

void InMemoryTransport::send(const Json& message) {
    std::shared_ptr<InMemoryTransport> peer;
    {
        std::lock_guard lock(mu_);
        if (closed_) throw std::runtime_error("Transport closed");
        peer = peer_.lock();
    }
    if (!peer) throw std::runtime_error("Transport is not linked");
    peer->deliver(message);
}

void InMemoryTransport::on_message(std::function<void(const Json&)> handler) {
    std::vector<Json> pending;
    {
        std::lock_guard lock(mu_);
        handler_ = std::move(handler);
        if (started_) pending.swap(pending_);
    }
    for (const auto& message : pending) {
        std::function<void(const Json&)> current;
        {
            std::lock_guard lock(mu_);
            current = handler_;
        }
        if (current) current(message);
    }
}

void InMemoryTransport::deliver(const Json& message) {
    std::function<void(const Json&)> handler;
    {
        std::lock_guard lock(mu_);
        if (closed_) return;
        if (!started_ || !handler_) {
            pending_.push_back(message);
            return;
        }
        handler = handler_;
    }
    try {
        if (handler) handler(message);
    } catch (...) {
    }
}

SessionMuxTransport::SessionMuxTransport(std::string session_id, std::function<void(const Json& envelope)> send_envelope)
    : session_id_(std::move(session_id)), send_envelope_(std::move(send_envelope)) {}

void SessionMuxTransport::start() {
    std::vector<Json> pending;
    std::function<void(const Json&)> handler;
    {
        std::lock_guard lock(mu_);
        if (closed_) throw std::runtime_error("Transport already closed");
        started_ = true;
        if (handler_) {
            pending.swap(pending_);
            handler = handler_;
        }
    }
    for (const auto& message : pending) {
        if (handler) handler(message);
    }
}

void SessionMuxTransport::close() {
    std::lock_guard lock(mu_);
    closed_ = true;
    handler_ = nullptr;
}

void SessionMuxTransport::send(const Json& message) {
    std::function<void(const Json&)> send_envelope;
    {
        std::lock_guard lock(mu_);
        if (closed_) throw std::runtime_error("Transport closed");
        send_envelope = send_envelope_;
    }
    if (!send_envelope) return;
    send_envelope(Json{{"type", "mcp"}, {"sessionId", session_id_}, {"message", message}});
}

void SessionMuxTransport::on_message(std::function<void(const Json&)> handler) {
    std::vector<Json> pending;
    {
        std::lock_guard lock(mu_);
        handler_ = std::move(handler);
        if (started_) pending.swap(pending_);
    }
    for (const auto& message : pending) {
        std::function<void(const Json&)> current;
        {
            std::lock_guard lock(mu_);
            current = handler_;
        }
        if (current) current(message);
    }
}

void SessionMuxTransport::receive(const Json& message) { deliver(message); }

void SessionMuxTransport::deliver(const Json& message) {
    std::function<void(const Json&)> handler;
    {
        std::lock_guard lock(mu_);
        if (closed_) return;
        if (!started_ || !handler_) {
            pending_.push_back(message);
            return;
        }
        handler = handler_;
    }
    try {
        if (handler) handler(message);
    } catch (...) {
    }
}

void McpClient::connect(const std::shared_ptr<MessageTransport>& transport) {
    transport_ = transport;
    transport_->on_message([this](const Json& message) { deliver(message); });
    transport_->start();
    request("initialize", Json{{"protocolVersion", kMcpProtocolVersion},
                               {"capabilities", Json::object()},
                               {"clientInfo", Json{{"name", "agent-player"}, {"version", kSdkVersion}}}});
    transport_->send(rpc_notify("notifications/initialized"));
}

Json McpClient::list_tools() {
    const Json result = request("tools/list", Json::object());
    Json tools = Json::array();
    if (result.is_object() && result.contains("tools") && result.at("tools").is_array()) {
        for (const auto& tool : result.at("tools")) {
            if (!tool.is_object()) continue;
            const std::string name = tool.value("name", "");
            Json input = Json{{"type", "object"}, {"properties", Json::object()}};
            if (tool.contains("inputSchema")) input = tool.at("inputSchema");
            tools.push_back(Json{{"name", name},
                                 {"description", tool.value("description", name)},
                                 {"inputSchema", std::move(input)}});
        }
    }
    return tools;
}

ToolPayload McpClient::call_tool(const std::string& name, const Json& arguments) {
    Json args = arguments.is_object() ? arguments : Json::object();
    return parse_tool_payload(request("tools/call", Json{{"name", name}, {"arguments", std::move(args)}}));
}

void McpClient::close() {
    fail_pending("mcp client closed");
    auto transport = transport_;
    if (transport) {
        try {
            transport->close();
        } catch (...) {
        }
    }
}

Json McpClient::request(const std::string& method, const Json& params) {
    std::shared_ptr<Pending> pending;
    int id = 0;
    std::shared_ptr<MessageTransport> transport;
    {
        std::lock_guard lock(mu_);
        if (closed_) throw std::runtime_error("mcp client closed");
        if (!transport_) throw std::runtime_error("Client not connected");
        id = ++next_id_;
        pending = std::make_shared<Pending>();
        pending_[id] = pending;
        transport = transport_;
    }
    try {
        transport->send(rpc_request(id, method, params));
        std::unique_lock lock(pending->mu);
        pending->cv.wait(lock, [&] { return pending->done; });
        if (pending->failed) throw std::runtime_error(pending->error.empty() ? "mcp error" : pending->error);
        return pending->result;
    } catch (...) {
        std::lock_guard lock(mu_);
        pending_.erase(id);
        throw;
    }
}

void McpClient::deliver(const Json& message) {
    if (!message.is_object()) return;
    const bool has_method = message.contains("method") && message.at("method").is_string();
    const bool has_id = message.contains("id") && !message.at("id").is_null();
    if (has_method) {
        if (!has_id) return;
        std::shared_ptr<MessageTransport> transport;
        {
            std::lock_guard lock(mu_);
            transport = transport_;
            if (closed_ || !transport) return;
        }
        const std::string method = message.at("method").get<std::string>();
        Json reply = method == "ping" ? rpc_response(message.at("id"), Json::object())
                                       : rpc_error(message.at("id"), -32601, "method not found: " + method);
        try {
            transport->send(reply);
        } catch (...) {
        }
        return;
    }
    if (!has_id) return;
    int id = 0;
    if (message.at("id").is_number_integer()) id = message.at("id").get<int>();
    else return;
    std::shared_ptr<Pending> pending;
    {
        std::lock_guard lock(mu_);
        auto found = pending_.find(id);
        if (found == pending_.end()) return;
        pending = found->second;
        pending_.erase(found);
    }
    std::lock_guard lock(pending->mu);
    if (message.contains("error") && message.at("error").is_object()) {
        pending->failed = true;
        pending->error = message.at("error").value("message", "mcp error");
    } else if (message.contains("result")) {
        pending->result = message.at("result");
    } else {
        pending->failed = true;
        pending->error = "mcp response missing result";
    }
    pending->done = true;
    pending->cv.notify_all();
}

void McpClient::fail_pending(const std::string& message) {
    std::vector<std::shared_ptr<Pending>> waiters;
    {
        std::lock_guard lock(mu_);
        closed_ = true;
        for (auto& [id, pending] : pending_) waiters.push_back(pending);
        pending_.clear();
    }
    for (const auto& pending : waiters) {
        std::lock_guard lock(pending->mu);
        if (pending->done) continue;
        pending->failed = true;
        pending->error = message;
        pending->done = true;
        pending->cv.notify_all();
    }
}

SessionActionLog::SessionActionLog(std::shared_ptr<SessionMemory> memory, std::string session_id, std::string character)
    : memory_(std::move(memory)), session_id_(std::move(session_id)), character_(std::move(character)) {}

void SessionActionLog::record(const Json& entry) { memory_->record(session_id_, character_, entry); }

Json SessionActionLog::done() const { return memory_->done(session_id_); }

SessionMcpServer::SessionMcpServer(ReflectedPlayer player, std::shared_ptr<TurnGate> gate, std::string name,
                                   std::shared_ptr<SessionActionLog> actions)
    : player_(std::move(player)), gate_(std::move(gate)), name_(std::move(name)), actions_(std::move(actions)) {}

SessionMcpServer::~SessionMcpServer() { close(); }

void SessionMcpServer::connect(const std::shared_ptr<MessageTransport>& transport) {
    transport_ = transport;
    transport_->on_message([this](const Json& message) {
        std::lock_guard lock(mu_);
        if (closed_) return;
        queue_.push_back(message);
        cv_.notify_one();
    });
    transport_->start();
    thread_ = std::thread([this] { loop(); });
}

void SessionMcpServer::close() {
    {
        std::lock_guard lock(mu_);
        closed_ = true;
        cv_.notify_all();
    }
    if (thread_.joinable()) thread_.join();
    if (transport_) {
        try {
            transport_->close();
        } catch (...) {
        }
    }
}

void SessionMcpServer::loop() {
    while (true) {
        Json message;
        {
            std::unique_lock lock(mu_);
            cv_.wait(lock, [&] { return closed_ || !queue_.empty(); });
            if (closed_ && queue_.empty()) return;
            message = std::move(queue_.front());
            queue_.erase(queue_.begin());
        }
        try {
            handle(message);
        } catch (...) {
        }
    }
}

void SessionMcpServer::handle(const Json& message) {
    if (!message.is_object()) return;
    if (!message.contains("method") || !message.at("method").is_string()) return;
    const bool has_id = message.contains("id") && !message.at("id").is_null();
    const std::string method = message.at("method").get<std::string>();
    if (!has_id || method.rfind("notifications/", 0) == 0) return;
    Json params = Json::object();
    if (message.contains("params") && message.at("params").is_object()) params = message.at("params");
    const Json id = message.at("id");
    try {
        if (method == "initialize") reply(id, initialize(params));
        else if (method == "ping") reply(id, Json::object());
        else if (method == "tools/list") reply(id, Json{{"tools", player_.tools}});
        else if (method == "tools/call") reply(id, call_tool(params));
        else send(rpc_error(id, -32601, "method not found: " + method));
    } catch (const std::exception& ex) {
        send(rpc_error(id, -32603, error_text(ex)));
    }
}

Json SessionMcpServer::initialize(const Json& params) const {
    std::string version = kMcpProtocolVersion;
    if (params.contains("protocolVersion") && params.at("protocolVersion").is_string() &&
        !params.at("protocolVersion").get<std::string>().empty()) {
        version = params.at("protocolVersion").get<std::string>();
    }
    return Json{{"protocolVersion", version},
                {"capabilities", Json{{"tools", Json::object()}}},
                {"serverInfo", Json{{"name", name_}, {"version", kSdkVersion}}}};
}

Json SessionMcpServer::call_tool(const Json& params) {
    if (!params.contains("name") || !params.at("name").is_string() || params.at("name").get<std::string>().empty()) {
        return tool_error("missing tool name");
    }
    const std::string name = params.at("name").get<std::string>();
    Json args = Json::object();
    if (params.contains("arguments") && params.at("arguments").is_object()) args = params.at("arguments");
    try {
        if (name == "see") return json_result(viewed());
        if (name == "wait") {
            gate_->wait_for_turn();
            return json_result(viewed());
        }
        const ReflectedAction* action = nullptr;
        for (const auto& candidate : player_.actions) {
            if (candidate.name == name) {
                action = &candidate;
                break;
            }
        }
        if (action == nullptr) return tool_error("Unknown tool: " + name);
        try {
            Json result = action->invoke(args);
            if (actions_) {
                actions_->record(Json{{"action", name}, {"args", args}, {"ok", true}});
            }
            return json_result(result);
        } catch (const std::exception& ex) {
            const std::string message = error_text(ex);
            if (actions_) {
                actions_->record(Json{{"action", name}, {"args", args}, {"ok", false}, {"error", message}});
            }
            return tool_error(message);
        }
    } catch (const std::exception& ex) {
        return tool_error(error_text(ex));
    }
}

Json SessionMcpServer::viewed() {
    Json view = player_.see.invoke(Json::object());
    if (!actions_) return view;
    return view_with_done(view, actions_->done());
}

void SessionMcpServer::reply(const Json& id, const Json& result) { send(rpc_response(id, result)); }

void SessionMcpServer::send(const Json& message) {
    std::shared_ptr<MessageTransport> transport;
    {
        std::lock_guard lock(mu_);
        if (closed_) return;
        transport = transport_;
    }
    if (!transport) return;
    try {
        transport->send(message);
    } catch (...) {
    }
}

}  // namespace wsi::detail
