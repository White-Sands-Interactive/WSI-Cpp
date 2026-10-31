#pragma once

#include "wsi/event.hpp"
#include "wsi/logger.hpp"
#include "wsi/player.hpp"

#include <functional>
#include <memory>
#include <optional>
#include <string>

namespace wsi {

namespace detail {
class MessageTransport;
}

class AbortSignal;

struct TurnOptions {
    /// Overrides the server default. turn() throws TurnTimeoutError when exceeded.
    std::optional<int> timeout_ms;
    /// Abort the wait for the platform's turn result. nullptr means no signal.
    AbortSignal* signal = nullptr;
};

struct StartOptions {
    /// Label stored with the session. Defaults to the player type name.
    std::optional<std::string> name;
    /// Added to the model prompt for this player only.
    std::optional<std::string> goal;
    /// Extra instructions appended to the default agent prompt.
    std::optional<std::string> instructions;
};

/// What an inline platform receives for one player.
struct StartSession {
    std::string session_id;
    std::string session_token;
    std::string name;
    Json tools = Json::array();
    std::string instructions;
    std::optional<std::string> goal;
    std::function<void(const TurnReport&)> on_turn_result;
    std::function<void(const AgentEvent&)> on_event;
    std::function<void(int tokens)> on_usage;
    Logger logger;
    /// Linked to this session's tool server. The testing loop reads it. Game code leaves it alone.
    std::shared_ptr<detail::MessageTransport> transport;
};

/// In-process platform used by testing::create_test_platform and local runs.
class InlinePlatform {
 public:
    virtual ~InlinePlatform() = default;
    virtual void start_session(const StartSession& session) = 0;
    /// Mark the session stopped before its wait is released, so the loop does not take another action.
    virtual void prepare_stop(const std::string& session_id) { (void)session_id; }
    virtual void stop_session(const std::string& session_id) = 0;
    virtual void close() {}
};

struct AgentServerOptions {
    /// Platform key naming the billing account.
    /// Defaults to the WSI_PLATFORM_KEY environment variable.
    /// Not required when inline_platform is set.
    std::optional<std::string> platform_key;
    /// Skip the outbound WebSocket and run against this in-process platform.
    std::shared_ptr<InlinePlatform> inline_platform;
    /// Replaces the default agent instructions for every character on this server.
    std::optional<std::string> instructions;
    /// Default timeout for turn(). Default: 60 seconds.
    std::optional<int> turn_timeout_ms;
    /// Receives connection and session events for every player on this server.
    EventListener on_event;
    /// Where the SDK writes warnings. Nothing is written when the functions are empty.
    Logger logger;
    /// Turn the action log off. The view then has no done field.
    bool memory = true;
    /// SQLite file of actions each character has already called.
    /// Defaults to .wsi/agent-memory.sqlite in the current directory.
    std::optional<std::string> memory_path;
    /// Test hook. Game code leaves this empty and connects to kPlatformUrl.
    std::optional<std::string> platform_url;
};

/// Cancel a turn() wait. abort() unblocks the wait with reason.
class AbortSignal {
 public:
    AbortSignal();
    ~AbortSignal();

    AbortSignal(const AbortSignal&) = delete;
    AbortSignal& operator=(const AbortSignal&) = delete;

    void abort(const std::string& reason = {});
    bool aborted() const;
    /// Message thrown from turn() after abort(). Empty when not aborted.
    std::string reason() const;
    /// Fires when abort() is called. The subscription removes the listener.
    Subscription on_abort(std::function<void()> listener);

 private:
    struct State;
    std::shared_ptr<State> state_;
};

/// One started player. turn() grants that character one chance to act.
class AgentHandle {
 public:
    AgentHandle() = default;

    const std::string& session_id() const;

    /// Grant this session one turn.
    /// Returns what the agent did, or throws after a second failed action, a timeout, or a lost connection.
    TurnResult turn(const TurnOptions& options = {});

    /// Stop the remote agent for this session. The AgentServer stays up.
    void stop();

    /// Subscribe to this session's action, retry, and failure events.
    Subscription on(EventListener listener);

    explicit operator bool() const { return static_cast<bool>(state_); }

 private:
    struct Access;
    friend class AgentServer;
    std::shared_ptr<Access> state_;
    std::string session_id_;
};

/// One in-process tool server and one outbound connection to the platform.
/// Every start() is its own session. The game calls turn() to decide whose turn it is.
class AgentServer {
    friend class AgentHandle;

 public:
    explicit AgentServer(AgentServerOptions options = {});
    ~AgentServer();

    AgentServer(const AgentServer&) = delete;
    AgentServer& operator=(const AgentServer&) = delete;
    AgentServer(AgentServer&&) noexcept;
    AgentServer& operator=(AgentServer&&) noexcept;

    /// Reflect the player into tools, register a session, and return a handle.
    /// The player must outlive the session.
    AgentHandle start(Player& player, const StartOptions& options = {});

    /// Subscribe to every session, plus connection events.
    Subscription on(EventListener listener);

    /// Stop every session and close the platform connection.
    void close();

    /// Close the socket so a blocked start() or turn() returns. Local sessions
    /// are left in place until close().
    void interrupt();

 private:
    struct State;
    std::shared_ptr<State> state_;
};

}  // namespace wsi
