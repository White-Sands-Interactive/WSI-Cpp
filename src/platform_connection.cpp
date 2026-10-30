#include "platform_connection.hpp"

#include "log.hpp"
#include "uuid.hpp"
#include "websocket.hpp"
#include "wsi/error.hpp"
#include "wsi/protocol.hpp"

#include <chrono>
#include <condition_variable>
#include <exception>
#include <mutex>
#include <thread>
#include <unordered_map>

namespace wsi {
namespace {
constexpr int kHeartbeatMs = 15000;
constexpr int kPongTimeoutMs = 10000;
constexpr int kMaxReconnectAttempts = 8;
}  // namespace

struct PlatformConnection::Waiter {
    std::mutex mu;
    std::condition_variable cv;
    bool done = false;
    std::exception_ptr error;
    Json envelope = Json::object();
    int timeout_ms = 15000;
};

struct PlatformConnection::State {
    net::WsClient ws;
    std::mutex mu;
    std::condition_variable cv;
    std::thread net;
    std::thread heartbeat;
    std::mutex heartbeat_mu;
    std::condition_variable heartbeat_cv;
    bool heartbeat_stop = false;
    std::string pending_pong;
    std::mutex send_mu;
    std::vector<Json> outbox;
    std::mutex pending_mu;
    std::unordered_map<std::string, std::shared_ptr<Waiter>> pending;
    std::mutex ready_mu;
    std::condition_variable ready_cv;
    bool ready = false;
    std::exception_ptr ready_error;
    bool closed = false;
    bool ever_connected = false;
    bool net_started = false;
    int reconnect_attempt = 0;
};

PlatformConnection::PlatformConnection(std::string url, std::string platform_key, Logger logger,
                                       std::function<void(const Json&)> on_envelope, std::function<void()> on_disconnect,
                                       std::function<void()> on_reconnect, std::function<void(std::exception_ptr)> on_error,
                                       std::function<std::vector<std::pair<std::string, std::string>>()> sessions)
    : url_(std::move(url)),
      platform_key_(std::move(platform_key)),
      client_id_(uuid_v4()),
      logger_(std::move(logger)),
      on_envelope_(std::move(on_envelope)),
      on_disconnect_(std::move(on_disconnect)),
      on_reconnect_(std::move(on_reconnect)),
      on_error_(std::move(on_error)),
      sessions_(std::move(sessions)),
      state_(std::make_unique<State>()) {}

PlatformConnection::~PlatformConnection() { close(); }

void PlatformConnection::connect() {
    if (state_->closed) throw std::runtime_error("Platform connection is closed");
    if (!state_->net_started) {
        state_->net_started = true;
        state_->net = std::thread([this] { net_main(); });
    }
    std::unique_lock lock(state_->ready_mu);
    state_->ready_cv.wait(lock, [&] { return state_->ready; });
    if (state_->ready_error) std::rethrow_exception(state_->ready_error);
}

void PlatformConnection::send(const Json& envelope) {
    std::lock_guard lock(state_->send_mu);
    if (state_->ws.is_open()) {
        try {
            state_->ws.send_text(envelope.dump());
            return;
        } catch (...) {
            const std::string type = envelope.value("type", "");
            if (type == "mcp" || type == "ping") state_->outbox.push_back(envelope);
            return;
        }
    }
    const std::string type = envelope.value("type", "");
    if (type == "mcp" || type == "ping") {
        state_->outbox.push_back(envelope);
        return;
    }
    throw std::runtime_error("Not connected to platform");
}

void PlatformConnection::begin_wait(const std::string& session_id, const std::string& kind, int timeout_ms) {
    auto waiter = std::make_shared<Waiter>();
    waiter->timeout_ms = timeout_ms;
    std::lock_guard lock(state_->pending_mu);
    state_->pending[key_for(session_id, kind)] = std::move(waiter);
}

void PlatformConnection::wait(const std::string& session_id, const std::string& kind) {
    const std::string key = key_for(session_id, kind);
    std::shared_ptr<Waiter> waiter;
    {
        std::lock_guard lock(state_->pending_mu);
        auto found = state_->pending.find(key);
        if (found == state_->pending.end()) throw std::runtime_error("Timed out waiting for session " + kind);
        waiter = found->second;
    }
    {
        std::unique_lock lock(waiter->mu);
        if (!waiter->cv.wait_for(lock, std::chrono::milliseconds(waiter->timeout_ms), [&] { return waiter->done; })) {
            lock.unlock();
            std::lock_guard pending_lock(state_->pending_mu);
            auto found = state_->pending.find(key);
            if (found != state_->pending.end() && found->second == waiter) state_->pending.erase(found);
            std::lock_guard again(waiter->mu);
            if (!waiter->done) throw std::runtime_error("Timed out waiting for session " + kind);
        }
        if (waiter->error) std::rethrow_exception(waiter->error);
    }
}

void PlatformConnection::cancel_wait(const std::string& session_id, const std::string& kind) {
    settle(key_for(session_id, kind), nullptr, std::make_exception_ptr(std::runtime_error("platform connection closed")));
}

std::shared_ptr<detail::SessionMuxTransport> PlatformConnection::create_transport(const std::string& session_id) {
    return std::make_shared<detail::SessionMuxTransport>(session_id, [this](const Json& envelope) { send(envelope); });
}

void PlatformConnection::close() {
    if (!state_) return;
    {
        std::lock_guard lock(state_->mu);
        state_->closed = true;
        state_->cv.notify_all();
    }
    stop_heartbeat();
    state_->ws.abort();
    if (state_->net.joinable()) state_->net.join();
    fail_waiters(std::make_exception_ptr(std::runtime_error("platform connection closed")));
}

void PlatformConnection::net_main() {
    try {
        hello();
        state_->ever_connected = true;
        state_->reconnect_attempt = 0;
        {
            std::lock_guard lock(state_->ready_mu);
            state_->ready = true;
        }
        state_->ready_cv.notify_all();
    } catch (...) {
        {
            std::lock_guard lock(state_->ready_mu);
            state_->ready = true;
            state_->ready_error = std::current_exception();
        }
        state_->ready_cv.notify_all();
        return;
    }

    start_heartbeat();
    while (!state_->closed) {
        std::optional<std::string> text;
        try {
            text = state_->ws.read_text();
        } catch (...) {
            text.reset();
        }
        if (!text) {
            stop_heartbeat();
            if (state_->closed || !state_->ever_connected) {
                fail_waiters(std::make_exception_ptr(std::runtime_error("platform connection closed")));
                return;
            }
            if (!reconnect()) return;
            continue;
        }
        try {
            dispatch(Json::parse(*text));
        } catch (const std::exception& ex) {
            if (on_error_) on_error_(std::make_exception_ptr(std::runtime_error(ex.what())));
        }
    }
}

void PlatformConnection::hello() {
    state_->ws.connect(url_);
    state_->ws.send_text(Json{{"type", "hello"},
                              {"platformKey", platform_key_},
                              {"clientId", client_id_},
                              {"protocolVersion", kProtocolVersion},
                              {"sdkVersion", kSdkVersion}}
                             .dump());
    auto text = state_->ws.read_text();
    if (!text) throw std::runtime_error("platform connection closed before hello");
    const Json envelope = Json::parse(*text);
    const std::string type = envelope.value("type", "");
    if (type == "hello_err") {
        state_->closed = true;
        const std::string message = envelope.value("message", "hello failed");
        if (envelope.value("code", "") == "PROTOCOL") throw ProtocolVersionError(message);
        throw PlatformAuthError(message);
    }
    if (type != "hello_ok") throw std::runtime_error("platform connection closed before hello");
    flush_outbox();
}

bool PlatformConnection::reconnect() {
    if (on_disconnect_) on_disconnect_();
    while (!state_->closed) {
        if (state_->reconnect_attempt >= kMaxReconnectAttempts) {
            state_->closed = true;
            auto error = std::make_exception_ptr(ConnectionLostError("Could not reconnect to the platform"));
            if (on_error_) on_error_(error);
            fail_waiters(error);
            return false;
        }
        int delay = 250;
        for (int i = 0; i < state_->reconnect_attempt; ++i) delay = std::min(delay * 2, kSessionResumeGraceMs);
        delay = std::min(delay, kSessionResumeGraceMs);
        state_->reconnect_attempt += 1;
        log_line(logger_, "warn", "platform connection dropped; retrying in " + std::to_string(delay) + "ms");
        {
            std::unique_lock lock(state_->mu);
            if (state_->cv.wait_for(lock, std::chrono::milliseconds(delay), [&] { return state_->closed; })) return false;
        }
        try {
            hello();
            state_->ever_connected = true;
            state_->reconnect_attempt = 0;
            resume_sessions();
            start_heartbeat();
            if (on_reconnect_) on_reconnect_();
            return true;
        } catch (const PlatformAuthError&) {
            state_->closed = true;
            if (on_error_) on_error_(std::current_exception());
            fail_waiters(std::current_exception());
            return false;
        } catch (const ProtocolVersionError&) {
            state_->closed = true;
            if (on_error_) on_error_(std::current_exception());
            fail_waiters(std::current_exception());
            return false;
        } catch (...) {
            if (state_->closed) return false;
        }
    }
    return false;
}

void PlatformConnection::resume_sessions() {
    const auto sessions = sessions_ ? sessions_() : std::vector<std::pair<std::string, std::string>>{};
    for (const auto& [session_id, session_token] : sessions) {
        begin_wait(session_id, "resume");
        const std::string key = key_for(session_id, "resume");
        std::shared_ptr<Waiter> waiter;
        {
            std::lock_guard lock(state_->pending_mu);
            waiter = state_->pending.at(key);
        }
        send(Json{{"type", "resume_session"}, {"sessionId", session_id}, {"sessionToken", session_token}});
        while (!state_->closed) {
            {
                std::lock_guard lock(waiter->mu);
                if (waiter->done) {
                    if (waiter->error) std::rethrow_exception(waiter->error);
                    break;
                }
            }
            auto text = state_->ws.read_text();
            if (!text) throw std::runtime_error("platform connection closed");
            dispatch(Json::parse(*text));
        }
    }
}

void PlatformConnection::dispatch(const Json& envelope) {
    const std::string type = envelope.value("type", "");
    if (type == "pong") {
        std::lock_guard lock(state_->heartbeat_mu);
        if (state_->pending_pong == envelope.value("id", "")) {
            state_->pending_pong.clear();
            state_->heartbeat_cv.notify_all();
        }
        return;
    }
    const std::string session_id = envelope.value("sessionId", "");
    if (!session_id.empty()) {
        if (type == "session_started" || type == "session_stopped") {
            settle(key_for(session_id, type == "session_started" ? "start" : "stop"), &envelope, nullptr);
        } else if (type == "session_resumed") {
            settle(key_for(session_id, "resume"), &envelope, nullptr);
        } else if (type == "resume_err") {
            settle(key_for(session_id, "resume"), nullptr,
                   std::make_exception_ptr(ConnectionLostError(envelope.value("message", "resume failed"))));
        }
    }
    if (on_envelope_) on_envelope_(envelope);
}

void PlatformConnection::settle(const std::string& key, const Json* envelope, std::exception_ptr error) {
    std::shared_ptr<Waiter> waiter;
    {
        std::lock_guard lock(state_->pending_mu);
        auto found = state_->pending.find(key);
        if (found == state_->pending.end()) return;
        waiter = found->second;
        state_->pending.erase(found);
    }
    std::lock_guard lock(waiter->mu);
    if (waiter->done) return;
    waiter->done = true;
    waiter->error = std::move(error);
    if (envelope != nullptr) waiter->envelope = *envelope;
    waiter->cv.notify_all();
}

void PlatformConnection::fail_waiters(std::exception_ptr error) {
    std::vector<std::shared_ptr<Waiter>> waiters;
    {
        std::lock_guard lock(state_->pending_mu);
        for (auto& [key, waiter] : state_->pending) waiters.push_back(waiter);
        state_->pending.clear();
    }
    for (const auto& waiter : waiters) {
        std::lock_guard lock(waiter->mu);
        if (waiter->done) continue;
        waiter->done = true;
        waiter->error = error;
        waiter->cv.notify_all();
    }
}

void PlatformConnection::flush_outbox() {
    std::vector<Json> pending;
    {
        std::lock_guard lock(state_->send_mu);
        pending.swap(state_->outbox);
    }
    for (const auto& envelope : pending) send(envelope);
}

void PlatformConnection::start_heartbeat() {
    stop_heartbeat();
    std::lock_guard lock(state_->heartbeat_mu);
    if (state_->closed) return;
    state_->heartbeat_stop = false;
    state_->heartbeat = std::thread([this] {
        std::unique_lock lock(state_->heartbeat_mu);
        while (!state_->heartbeat_stop && !state_->closed) {
            if (state_->heartbeat_cv.wait_for(lock, std::chrono::milliseconds(kHeartbeatMs),
                                              [&] { return state_->heartbeat_stop || state_->closed; })) {
                return;
            }
            if (!state_->ws.is_open()) continue;
            const std::string id = uuid_v4();
            state_->pending_pong = id;
            lock.unlock();
            try {
                send(Json{{"type", "ping"}, {"id", id}});
            } catch (...) {
                lock.lock();
                continue;
            }
            lock.lock();
            if (!state_->heartbeat_cv.wait_for(lock, std::chrono::milliseconds(kPongTimeoutMs), [&] {
                    return state_->heartbeat_stop || state_->closed || state_->pending_pong != id;
                })) {
                if (state_->pending_pong == id) {
                    log_line(logger_, "warn", "platform heartbeat timed out");
                    state_->ws.abort();
                }
            }
        }
    });
}

void PlatformConnection::stop_heartbeat() {
    std::thread worker;
    {
        std::lock_guard lock(state_->heartbeat_mu);
        state_->heartbeat_stop = true;
        state_->pending_pong.clear();
        if (state_->heartbeat.joinable()) worker = std::move(state_->heartbeat);
    }
    state_->heartbeat_cv.notify_all();
    if (worker.joinable()) worker.join();
}

std::string PlatformConnection::key_for(const std::string& session_id, const std::string& kind) const {
    return "session:" + session_id + ":" + kind;
}

}  // namespace wsi
