#pragma once

#include "memory.hpp"
#include "turn_gate.hpp"
#include "wsi/agent_server.hpp"
#include "wsi/player.hpp"

#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace wsi::detail {

class MessageTransport {
 public:
    virtual ~MessageTransport() = default;
    virtual void start() = 0;
    virtual void close() = 0;
    virtual void send(const Json& message) = 0;
    virtual void on_message(std::function<void(const Json&)> handler) = 0;
};

class InMemoryTransport : public MessageTransport, public std::enable_shared_from_this<InMemoryTransport> {
 public:
    static std::pair<std::shared_ptr<InMemoryTransport>, std::shared_ptr<InMemoryTransport>> create_linked_pair();

    void start() override;
    void close() override;
    void send(const Json& message) override;
    void on_message(std::function<void(const Json&)> handler) override;

 private:
    void deliver(const Json& message);
    void flush();

    std::mutex mu_;
    bool started_ = false;
    bool closed_ = false;
    std::function<void(const Json&)> handler_;
    std::vector<Json> pending_;
    std::weak_ptr<InMemoryTransport> peer_;
};

class SessionMuxTransport : public MessageTransport {
 public:
    SessionMuxTransport(std::string session_id, std::function<void(const Json& envelope)> send_envelope);

    void start() override;
    void close() override;
    void send(const Json& message) override;
    void on_message(std::function<void(const Json&)> handler) override;
    void receive(const Json& message);

 private:
    void deliver(const Json& message);
    void flush();

    std::string session_id_;
    std::function<void(const Json&)> send_envelope_;
    std::mutex mu_;
    bool started_ = false;
    bool closed_ = false;
    std::function<void(const Json&)> handler_;
    std::vector<Json> pending_;
};

struct ToolPayload {
    bool is_error = false;
    std::string text;
    Json json = nullptr;
};

class McpClient {
 public:
    void connect(const std::shared_ptr<MessageTransport>& transport);
    Json list_tools();
    ToolPayload call_tool(const std::string& name, const Json& arguments);
    void close();

 private:
    Json request(const std::string& method, const Json& params);
    void deliver(const Json& message);
    void fail_pending(const std::string& message);

    struct Pending {
        std::mutex mu;
        std::condition_variable cv;
        bool done = false;
        bool failed = false;
        std::string error;
        Json result = nullptr;
    };

    std::mutex mu_;
    std::shared_ptr<MessageTransport> transport_;
    std::unordered_map<int, std::shared_ptr<Pending>> pending_;
    int next_id_ = 0;
    bool closed_ = false;
};

class SessionActionLog {
 public:
    SessionActionLog(std::shared_ptr<SessionMemory> memory, std::string session_id, std::string character);
    void record(const Json& entry);
    Json done() const;

 private:
    std::shared_ptr<SessionMemory> memory_;
    std::string session_id_;
    std::string character_;
};

class SessionMcpServer {
 public:
    SessionMcpServer(ReflectedPlayer player, std::shared_ptr<TurnGate> gate, std::string name,
                     std::shared_ptr<SessionActionLog> actions);
    ~SessionMcpServer();

    void connect(const std::shared_ptr<MessageTransport>& transport);
    void close();

 private:
    void loop();
    void handle(const Json& message);
    Json initialize(const Json& params) const;
    Json call_tool(const Json& params);
    Json viewed();
    void reply(const Json& id, const Json& result);
    void send(const Json& message);

    ReflectedPlayer player_;
    std::shared_ptr<TurnGate> gate_;
    std::string name_;
    std::shared_ptr<SessionActionLog> actions_;
    std::shared_ptr<MessageTransport> transport_;
    std::mutex mu_;
    std::condition_variable cv_;
    std::vector<Json> queue_;
    bool closed_ = false;
    std::thread thread_;
};

ToolPayload parse_tool_payload(const Json& result);
Json json_result(const Json& payload, bool is_error = false);
Json tool_error(const std::string& message);

}  // namespace wsi::detail
