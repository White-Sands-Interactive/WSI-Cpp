#pragma once

#include "mcp.hpp"
#include "wsi/logger.hpp"

#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace wsi {

class PlatformConnection {
 public:
    PlatformConnection(std::string url, std::string platform_key, Logger logger,
                       std::function<void(const Json&)> on_envelope, std::function<void()> on_disconnect,
                       std::function<void()> on_reconnect, std::function<void(std::exception_ptr)> on_error,
                       std::function<std::vector<std::pair<std::string, std::string>>()> sessions);
    ~PlatformConnection();

    PlatformConnection(const PlatformConnection&) = delete;
    PlatformConnection& operator=(const PlatformConnection&) = delete;

    void connect();
    void send(const Json& envelope);
    void begin_wait(const std::string& session_id, const std::string& kind, int timeout_ms = 15000);
    void wait(const std::string& session_id, const std::string& kind);
    void cancel_wait(const std::string& session_id, const std::string& kind);
    std::shared_ptr<detail::SessionMuxTransport> create_transport(const std::string& session_id);
    void close();

 private:
    struct Waiter;
    void net_main();
    void hello();
    bool reconnect();
    void resume_sessions();
    void dispatch(const Json& envelope);
    void settle(const std::string& key, const Json* envelope, std::exception_ptr error);
    void fail_waiters(std::exception_ptr error);
    void flush_outbox();
    void start_heartbeat();
    void stop_heartbeat();
    std::string key_for(const std::string& session_id, const std::string& kind) const;

    std::string url_;
    std::string platform_key_;
    std::string client_id_;
    Logger logger_;
    std::function<void(const Json&)> on_envelope_;
    std::function<void()> on_disconnect_;
    std::function<void()> on_reconnect_;
    std::function<void(std::exception_ptr)> on_error_;
    std::function<std::vector<std::pair<std::string, std::string>>()> sessions_;

    struct State;
    std::unique_ptr<State> state_;
};

}  // namespace wsi
