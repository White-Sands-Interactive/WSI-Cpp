#include "websocket.hpp"

#include <openssl/sha.h>
#include <openssl/ssl.h>

#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstring>
#include <mutex>
#include <random>
#include <sstream>
#include <stdexcept>
#include <thread>
#include <vector>

namespace wsi::net {
namespace {

constexpr std::uint64_t kMaxPayload = 16ull * 1024ull * 1024ull;
constexpr const char* kWsGuid = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";

std::string base64_encode(const unsigned char* data, std::size_t length) {
    static constexpr char kTable[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve(((length + 2) / 3) * 4);
    for (std::size_t i = 0; i < length; i += 3) {
        const unsigned int n = static_cast<unsigned int>(data[i]) << 16 |
                               (i + 1 < length ? static_cast<unsigned int>(data[i + 1]) << 8 : 0) |
                               (i + 2 < length ? static_cast<unsigned int>(data[i + 2]) : 0);
        out.push_back(kTable[(n >> 18) & 63]);
        out.push_back(kTable[(n >> 12) & 63]);
        out.push_back(i + 1 < length ? kTable[(n >> 6) & 63] : '=');
        out.push_back(i + 2 < length ? kTable[n & 63] : '=');
    }
    return out;
}

std::string sha1_base64(const std::string& input) {
    unsigned char hash[SHA_DIGEST_LENGTH];
    SHA1(reinterpret_cast<const unsigned char*>(input.data()), input.size(), hash);
    return base64_encode(hash, SHA_DIGEST_LENGTH);
}

std::string random_key() {
    unsigned char bytes[16];
    std::random_device device;
    std::uniform_int_distribution<int> dist(0, 255);
    for (auto& byte : bytes) byte = static_cast<unsigned char>(dist(device));
    return base64_encode(bytes, sizeof(bytes));
}

std::string lower_copy(std::string text) {
    for (char& ch : text) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    return text;
}

void set_socket_options(int fd) {
    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
#ifdef SO_NOSIGPIPE
    setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
#endif
}

int fcntl_nonblock(int fd);
void fcntl_restore(int fd, int flags);

struct ParsedUrl {
    bool tls = false;
    std::string host;
    int port = 80;
    std::string path = "/";
};

ParsedUrl parse_url(const std::string& url) {
    const auto scheme_end = url.find("://");
    if (scheme_end == std::string::npos) throw std::runtime_error("invalid websocket url");
    const std::string scheme = url.substr(0, scheme_end);
    ParsedUrl parsed;
    if (scheme == "wss") {
        parsed.tls = true;
        parsed.port = 443;
    } else if (scheme == "ws") {
        parsed.tls = false;
        parsed.port = 80;
    } else {
        throw std::runtime_error("websocket url must be ws:// or wss://");
    }
    const std::string rest = url.substr(scheme_end + 3);
    const auto slash = rest.find('/');
    const std::string hostport = slash == std::string::npos ? rest : rest.substr(0, slash);
    parsed.path = slash == std::string::npos ? "/" : rest.substr(slash);
    if (hostport.empty()) throw std::runtime_error("websocket url is missing a host");
    const auto colon = hostport.rfind(':');
    if (colon != std::string::npos && hostport.find(']') == std::string::npos) {
        parsed.host = hostport.substr(0, colon);
        parsed.port = std::stoi(hostport.substr(colon + 1));
    } else {
        parsed.host = hostport;
    }
    return parsed;
}

class Stream {
 public:
    int fd = -1;
    SSL* ssl = nullptr;
    SSL_CTX* ctx = nullptr;
    std::mutex write_mu;
    std::atomic<bool> open{false};
    std::string buffer;
    std::size_t offset = 0;

    ~Stream() { destroy(); }

    void destroy() {
        open = false;
        if (ssl != nullptr) {
            SSL_free(ssl);
            ssl = nullptr;
        }
        if (ctx != nullptr) {
            SSL_CTX_free(ctx);
            ctx = nullptr;
        }
        if (fd >= 0) {
            ::close(fd);
            fd = -1;
        }
        buffer.clear();
        offset = 0;
    }

    void abort() {
        open = false;
        const int current = fd;
        if (current >= 0) ::shutdown(current, SHUT_RDWR);
    }

    bool fill() {
        if (fd < 0 && ssl == nullptr) return false;
        char temp[8192];
        int n = 0;
        if (ssl != nullptr) {
            n = SSL_read(ssl, temp, sizeof(temp));
            if (n <= 0) return false;
        } else {
#ifdef MSG_NOSIGNAL
            n = static_cast<int>(::recv(fd, temp, sizeof(temp), 0));
#else
            n = static_cast<int>(::recv(fd, temp, sizeof(temp), 0));
#endif
            if (n <= 0) return false;
        }
        buffer.append(temp, static_cast<std::size_t>(n));
        return true;
    }

    bool take(void* dst, std::size_t n) {
        auto* out = static_cast<char*>(dst);
        while (buffer.size() - offset < n) {
            if (offset > 0 && offset == buffer.size()) {
                buffer.clear();
                offset = 0;
            }
            if (!fill()) return false;
        }
        std::memcpy(out, buffer.data() + offset, n);
        offset += n;
        if (offset > 65536 && offset == buffer.size()) {
            buffer.clear();
            offset = 0;
        }
        return true;
    }

    std::string read_http_headers() {
        while (buffer.find("\r\n\r\n", offset) == std::string::npos) {
            if (!fill()) throw std::runtime_error("websocket handshake failed");
        }
        const auto end = buffer.find("\r\n\r\n", offset);
        std::string headers = buffer.substr(offset, end - offset);
        offset = end + 4;
        return headers;
    }

    void write_all(const char* data, std::size_t length) {
        std::size_t sent = 0;
        while (sent < length) {
            int n = 0;
            if (ssl != nullptr) {
                n = SSL_write(ssl, data + sent, static_cast<int>(length - sent));
                if (n <= 0) throw std::runtime_error("websocket write failed");
            } else {
#ifdef MSG_NOSIGNAL
                n = static_cast<int>(::send(fd, data + sent, length - sent, MSG_NOSIGNAL));
#else
                n = static_cast<int>(::send(fd, data + sent, length - sent, 0));
#endif
                if (n <= 0) throw std::runtime_error("websocket write failed");
            }
            sent += static_cast<std::size_t>(n);
        }
    }

    struct Frame {
        bool fin = false;
        int opcode = 0;
        std::string payload;
    };

    bool read_frame(Frame& frame) {
        unsigned char header[2];
        if (!take(header, 2)) return false;
        frame.fin = (header[0] & 0x80) != 0;
        frame.opcode = header[0] & 0x0f;
        const bool masked = (header[1] & 0x80) != 0;
        std::uint64_t length = header[1] & 0x7f;
        if (length == 126) {
            unsigned char ext[2];
            if (!take(ext, 2)) return false;
            length = (static_cast<std::uint64_t>(ext[0]) << 8) | ext[1];
        } else if (length == 127) {
            unsigned char ext[8];
            if (!take(ext, 8)) return false;
            length = 0;
            for (unsigned char byte : ext) length = (length << 8) | byte;
        }
        if (length > kMaxPayload) return false;
        unsigned char mask[4] = {};
        if (masked && !take(mask, 4)) return false;
        frame.payload.assign(static_cast<std::size_t>(length), '\0');
        if (length > 0 && !take(frame.payload.data(), static_cast<std::size_t>(length))) return false;
        if (masked) {
            for (std::uint64_t i = 0; i < length; ++i) {
                frame.payload[static_cast<std::size_t>(i)] ^= static_cast<char>(mask[i % 4]);
            }
        }
        return true;
    }

    void send_frame(int opcode, const std::string& payload, bool mask) {
        if (payload.size() > kMaxPayload) throw std::runtime_error("websocket payload is too large");
        std::string frame;
        frame.push_back(static_cast<char>(0x80 | (opcode & 0x0f)));
        const auto length = payload.size();
        if (length < 126) {
            frame.push_back(static_cast<char>((mask ? 0x80 : 0) | static_cast<int>(length)));
        } else if (length <= 0xffff) {
            frame.push_back(static_cast<char>((mask ? 0x80 : 0) | 126));
            frame.push_back(static_cast<char>((length >> 8) & 0xff));
            frame.push_back(static_cast<char>(length & 0xff));
        } else {
            frame.push_back(static_cast<char>((mask ? 0x80 : 0) | 127));
            for (int shift = 56; shift >= 0; shift -= 8) {
                frame.push_back(static_cast<char>((static_cast<std::uint64_t>(length) >> shift) & 0xff));
            }
        }
        std::string masked = payload;
        if (mask) {
            unsigned char key[4];
            std::random_device device;
            std::uniform_int_distribution<int> dist(0, 255);
            for (auto& byte : key) byte = static_cast<unsigned char>(dist(device));
            frame.append(reinterpret_cast<char*>(key), 4);
            for (std::size_t i = 0; i < masked.size(); ++i) {
                masked[i] ^= static_cast<char>(key[i % 4]);
            }
        }
        frame += masked;
        std::lock_guard lock(write_mu);
        if (!open || fd < 0) throw std::runtime_error("websocket closed");
        write_all(frame.data(), frame.size());
    }

    std::optional<std::string> read_text(bool mask_outgoing) {
        std::string message;
        int message_opcode = -1;
        while (open) {
            Frame frame;
            if (!read_frame(frame)) return std::nullopt;
            if (frame.opcode == 0x9) {
                try {
                    send_frame(0xA, frame.payload, mask_outgoing);
                } catch (...) {
                }
                continue;
            }
            if (frame.opcode == 0xA) continue;
            if (frame.opcode == 0x8) {
                try {
                    send_frame(0x8, frame.payload, mask_outgoing);
                } catch (...) {
                }
                return std::nullopt;
            }
            if (frame.opcode == 0x1 || frame.opcode == 0x0) {
                if (frame.opcode != 0) message_opcode = frame.opcode;
                if (message.size() + frame.payload.size() > kMaxPayload) return std::nullopt;
                message += frame.payload;
                if (frame.fin) {
                    if (message_opcode == 0x1) return message;
                    message.clear();
                    message_opcode = -1;
                }
            }
        }
        return std::nullopt;
    }
};

std::string header_value(const std::string& headers, const std::string& name) {
    std::istringstream lines(headers);
    std::string line;
    const std::string prefix = lower_copy(name) + ":";
    while (std::getline(lines, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        const auto colon = line.find(':');
        if (colon == std::string::npos) continue;
        if (lower_copy(line.substr(0, colon + 1)) == prefix) {
            std::string value = line.substr(colon + 1);
            const auto start = value.find_first_not_of(" \t");
            return start == std::string::npos ? "" : value.substr(start);
        }
    }
    return {};
}

int tcp_connect(const std::string& host, int port) {
    addrinfo hints{};
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_family = AF_UNSPEC;
    addrinfo* result = nullptr;
    const std::string service = std::to_string(port);
    if (getaddrinfo(host.c_str(), service.c_str(), &hints, &result) != 0 || result == nullptr) {
        throw std::runtime_error("could not resolve " + host);
    }
    int fd = -1;
    for (auto* info = result; info != nullptr; info = info->ai_next) {
        fd = ::socket(info->ai_family, info->ai_socktype, info->ai_protocol);
        if (fd < 0) continue;
        set_socket_options(fd);
        const int flags = fcntl_nonblock(fd);
        const int rc = ::connect(fd, info->ai_addr, info->ai_addrlen);
        if (rc == 0 || errno == EINPROGRESS) {
            pollfd pfd{};
            pfd.fd = fd;
            pfd.events = POLLOUT;
            const int waited = ::poll(&pfd, 1, 10000);
            int error = 0;
            socklen_t len = sizeof(error);
            getsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &len);
            fcntl_restore(fd, flags);
            if (waited > 0 && error == 0) break;
        }
        ::close(fd);
        fd = -1;
    }
    freeaddrinfo(result);
    if (fd < 0) throw std::runtime_error("could not connect to " + host);
    return fd;
}

int fcntl_nonblock(int fd) {
    const int flags = ::fcntl(fd, F_GETFL, 0);
    ::fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    return flags;
}

void fcntl_restore(int fd, int flags) { ::fcntl(fd, F_SETFL, flags); }

void tls_connect(Stream& stream, const std::string& host) {
    OPENSSL_init_ssl(0, nullptr);
    stream.ctx = SSL_CTX_new(TLS_client_method());
    if (stream.ctx == nullptr) throw std::runtime_error("could not create a TLS context");
    SSL_CTX_set_default_verify_paths(stream.ctx);
    SSL_CTX_set_verify(stream.ctx, SSL_VERIFY_PEER, nullptr);
    stream.ssl = SSL_new(stream.ctx);
    if (stream.ssl == nullptr) throw std::runtime_error("could not create a TLS session");
    SSL_set_fd(stream.ssl, stream.fd);
    SSL_set_tlsext_host_name(stream.ssl, host.c_str());
    if (SSL_connect(stream.ssl) != 1) throw std::runtime_error("TLS handshake failed");
    if (SSL_get_verify_result(stream.ssl) != X509_V_OK) throw std::runtime_error("TLS certificate verification failed");
}

class ServerConnection : public WsConnection {
 public:
    explicit ServerConnection(int fd) {
        stream_.fd = fd;
        stream_.open = true;
        set_socket_options(fd);
    }

    ~ServerConnection() override { abort(); }

    void send_text(const std::string& text) override { stream_.send_frame(0x1, text, false); }

    void abort() override { stream_.abort(); }

    void on_text(std::function<void(const std::string&)> handler) override {
        std::lock_guard lock(mu_);
        on_text_ = std::move(handler);
    }

    void run() {
        try {
            const std::string headers = stream_.read_http_headers();
            const std::string key = header_value(headers, "Sec-WebSocket-Key");
            if (key.empty()) return;
            const std::string accept = sha1_base64(key + kWsGuid);
            const std::string response =
                "HTTP/1.1 101 Switching Protocols\r\n"
                "Upgrade: websocket\r\n"
                "Connection: Upgrade\r\n"
                "Sec-WebSocket-Accept: " +
                accept + "\r\n\r\n";
            {
                std::lock_guard lock(stream_.write_mu);
                stream_.write_all(response.data(), response.size());
            }
            while (stream_.open) {
                auto text = stream_.read_text(false);
                if (!text) break;
                std::function<void(const std::string&)> handler;
                {
                    std::lock_guard lock(mu_);
                    handler = on_text_;
                }
                if (handler) handler(*text);
            }
        } catch (...) {
        }
        stream_.open = false;
    }

 private:
    Stream stream_;
    std::mutex mu_;
    std::function<void(const std::string&)> on_text_;
};

}  // namespace

struct WsClient::Impl {
    Stream stream;
    bool mask = true;
};

WsClient::WsClient() : impl_(std::make_unique<Impl>()) {}
WsClient::~WsClient() { abort(); }

void WsClient::connect(const std::string& url) {
    abort();
    impl_ = std::make_unique<Impl>();
    const ParsedUrl parsed = parse_url(url);
    impl_->stream.fd = tcp_connect(parsed.host, parsed.port);
    if (parsed.tls) tls_connect(impl_->stream, parsed.host);
    impl_->stream.open = true;
    const std::string key = random_key();
    std::string host_header = parsed.host;
    if (!(parsed.tls && parsed.port == 443) && !(!parsed.tls && parsed.port == 80)) {
        host_header += ":" + std::to_string(parsed.port);
    }
    const std::string request = "GET " + parsed.path +
                                " HTTP/1.1\r\n"
                                "Host: " +
                                host_header +
                                "\r\n"
                                "Upgrade: websocket\r\n"
                                "Connection: Upgrade\r\n"
                                "Sec-WebSocket-Key: " +
                                key +
                                "\r\n"
                                "Sec-WebSocket-Version: 13\r\n\r\n";
    {
        std::lock_guard lock(impl_->stream.write_mu);
        impl_->stream.write_all(request.data(), request.size());
    }
    const std::string headers = impl_->stream.read_http_headers();
    if (headers.find("101") == std::string::npos) throw std::runtime_error("websocket upgrade was rejected");
    const std::string accept = header_value(headers, "Sec-WebSocket-Accept");
    if (accept != sha1_base64(key + kWsGuid)) throw std::runtime_error("websocket accept key did not match");
}

void WsClient::send_text(const std::string& text) { impl_->stream.send_frame(0x1, text, true); }

std::optional<std::string> WsClient::read_text() { return impl_->stream.read_text(true); }

void WsClient::abort() {
    if (impl_) impl_->stream.abort();
}

bool WsClient::is_open() const { return impl_ && impl_->stream.open && impl_->stream.fd >= 0; }

struct WsServer::Impl {
    int fd = -1;
    int port = 0;
    std::atomic<bool> open{false};
    std::function<void(const std::shared_ptr<WsConnection>&)> on_connection;
    std::thread accept_thread;
    std::mutex mu;
    std::vector<std::shared_ptr<ServerConnection>> connections;
    std::vector<std::thread> threads;
};

WsServer::WsServer(std::function<void(const std::shared_ptr<WsConnection>&)> on_connection)
    : impl_(std::make_unique<Impl>()) {
    impl_->on_connection = std::move(on_connection);
    impl_->fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (impl_->fd < 0) throw std::runtime_error("could not open a websocket server socket");
    int one = 1;
    setsockopt(impl_->fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons(0);
    if (::bind(impl_->fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        throw std::runtime_error("could not bind a websocket server");
    }
    socklen_t len = sizeof(addr);
    getsockname(impl_->fd, reinterpret_cast<sockaddr*>(&addr), &len);
    impl_->port = ntohs(addr.sin_port);
    if (::listen(impl_->fd, 16) != 0) throw std::runtime_error("could not listen for websocket clients");
    impl_->open = true;
    impl_->accept_thread = std::thread([this] {
        while (impl_->open) {
            const int fd = ::accept(impl_->fd, nullptr, nullptr);
            if (fd < 0) break;
            auto connection = std::make_shared<ServerConnection>(fd);
            {
                std::lock_guard lock(impl_->mu);
                impl_->connections.push_back(connection);
            }
            if (impl_->on_connection) impl_->on_connection(connection);
            std::thread worker([connection] { connection->run(); });
            std::lock_guard lock(impl_->mu);
            impl_->threads.push_back(std::move(worker));
        }
    });
}

WsServer::~WsServer() { close(); }

int WsServer::port() const { return impl_->port; }

void WsServer::close() {
    if (!impl_) return;
    impl_->open = false;
    if (impl_->fd >= 0) {
        ::shutdown(impl_->fd, SHUT_RDWR);
        ::close(impl_->fd);
        impl_->fd = -1;
    }
    if (impl_->accept_thread.joinable()) impl_->accept_thread.join();
    std::vector<std::shared_ptr<ServerConnection>> connections;
    std::vector<std::thread> threads;
    {
        std::lock_guard lock(impl_->mu);
        connections.swap(impl_->connections);
        threads.swap(impl_->threads);
    }
    for (const auto& connection : connections) connection->abort();
    for (auto& thread : threads) {
        if (thread.joinable()) thread.join();
    }
}

}  // namespace wsi::net
