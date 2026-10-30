#pragma once

#include <functional>
#include <memory>
#include <optional>
#include <string>

namespace wsi::net {

class WsConnection {
 public:
    virtual ~WsConnection() = default;
    virtual void send_text(const std::string& text) = 0;
    virtual void abort() = 0;
    virtual void on_text(std::function<void(const std::string&)> handler) = 0;
};

class WsServer {
 public:
    explicit WsServer(std::function<void(const std::shared_ptr<WsConnection>&)> on_connection);
    ~WsServer();

    WsServer(const WsServer&) = delete;
    WsServer& operator=(const WsServer&) = delete;

    int port() const;
    void close();

 private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

class WsClient {
 public:
    WsClient();
    ~WsClient();

    WsClient(const WsClient&) = delete;
    WsClient& operator=(const WsClient&) = delete;

    void connect(const std::string& url);
    void send_text(const std::string& text);
    std::optional<std::string> read_text();
    void abort();
    bool is_open() const;

 private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace wsi::net
