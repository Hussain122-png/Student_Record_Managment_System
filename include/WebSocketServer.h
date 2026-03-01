#pragma once
/**
 * WebSocketServer – RFC 6455 compliant WebSocket server.
 *
 * Features:
 *  - Multi-client via one thread per connection
 *  - HTTP upgrade handshake
 *  - Text / ping / pong / close frame handling
 *  - Thread-safe broadcast
 *  - Pluggable message handler callback
 *  - Automatic ping/pong keepalive (configurable interval)
 *  - Graceful handling of idle connections — no spurious disconnects
 */

#include "Crypto.h"
#include "WSFrame.h"
#include "Logger.h"

#include <string>
#include <vector>
#include <functional>
#include <thread>
#include <mutex>
#include <atomic>
#include <memory>
#include <chrono>
#include <sstream>
#include <algorithm>

// POSIX
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
#include <fcntl.h>
#include <arpa/inet.h>
#include <sys/time.h>
#include <errno.h>

class WebSocketServer {
public:
    using MessageHandler    = std::function<void(int clientId, const std::string& msg)>;
    using ConnectHandler    = std::function<void(int clientId)>;
    using DisconnectHandler = std::function<void(int clientId)>;

    struct Metrics {
        std::atomic<size_t> totalMessagesSent{0};
        std::atomic<size_t> totalBytesTransmitted{0};
        std::atomic<size_t> totalMessagesReceived{0};
        std::atomic<size_t> activeClients{0};
        double              lastBroadcastMs = 0.0;
        double              lastSendMs      = 0.0;
    };

    // ping_interval_s : send a Ping to idle clients this often (seconds)
    // pong_timeout_s  : disconnect if Pong not received within this many seconds
    explicit WebSocketServer(uint16_t port = 9001,
                             int ping_interval_s = 30,
                             int pong_timeout_s  = 10)
        : port_(port)
        , serverFd_(-1)
        , running_(false)
        , pingIntervalSec_(ping_interval_s)
        , pongTimeoutSec_(pong_timeout_s)
    {}

    ~WebSocketServer() { stop(); }

    void onMessage   (MessageHandler    h) { msgHandler_    = std::move(h); }
    void onConnect   (ConnectHandler    h) { connHandler_   = std::move(h); }
    void onDisconnect(DisconnectHandler h) { disconnHandler_= std::move(h); }

    bool start() {
        serverFd_ = ::socket(AF_INET, SOCK_STREAM, 0);
        if (serverFd_ < 0) { LOG_ERROR("socket() failed", "WSS"); return false; }

        int opt = 1;
        ::setsockopt(serverFd_, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

        sockaddr_in addr{};
        addr.sin_family      = AF_INET;
        addr.sin_addr.s_addr = INADDR_ANY;
        addr.sin_port        = htons(port_);

        if (::bind(serverFd_, (sockaddr*)&addr, sizeof(addr)) < 0) {
            LOG_ERROR("bind() failed on port " + std::to_string(port_), "WSS");
            return false;
        }
        if (::listen(serverFd_, 32) < 0) {
            LOG_ERROR("listen() failed", "WSS");
            return false;
        }

        running_ = true;
        acceptThread_ = std::thread(&WebSocketServer::acceptLoop, this);
        LOG_INFO("WebSocket server listening on ws://0.0.0.0:" + std::to_string(port_), "WSS");
        return true;
    }

    void stop() {
        running_ = false;
        if (serverFd_ >= 0) { ::close(serverFd_); serverFd_ = -1; }
        if (acceptThread_.joinable()) acceptThread_.join();
        std::lock_guard<std::mutex> lk(clientsMutex_);
        for (auto& c : clients_) ::close(c.fd);
        clients_.clear();
    }

    bool send(int clientId, const std::string& msg) {
        auto t0 = now();
        std::string frame = ws::buildFrame(msg);
        bool ok = false;
        {
            std::lock_guard<std::mutex> lk(clientsMutex_);
            auto it = findClient_(clientId);
            if (it != clients_.end()) {
                ok = sendRaw(it->fd, frame);
                if (ok) {
                    metrics_.totalMessagesSent++;
                    metrics_.totalBytesTransmitted += frame.size();
                }
            }
        }
        metrics_.lastSendMs = elapsed(t0);
        return ok;
    }

    size_t broadcast(const std::string& msg) {
        auto t0 = now();
        std::string frame = ws::buildFrame(msg);
        size_t count = 0;
        {
            std::lock_guard<std::mutex> lk(clientsMutex_);
            for (auto& c : clients_) {
                if (sendRaw(c.fd, frame)) {
                    ++count;
                    metrics_.totalMessagesSent++;
                    metrics_.totalBytesTransmitted += frame.size();
                }
            }
        }
        metrics_.lastBroadcastMs = elapsed(t0);
        LOG_DEBUG("Broadcast " + std::to_string(msg.size()) + " bytes to " +
                  std::to_string(count) + " clients in " +
                  fmtMs(metrics_.lastBroadcastMs) + " ms", "WSS");
        return count;
    }

    size_t clientCount() const {
        std::lock_guard<std::mutex> lk(clientsMutex_);
        return clients_.size();
    }

    const Metrics& metrics() const { return metrics_; }

private:
    struct Client {
        int         fd;
        int         id;
        std::string remoteAddr;
        std::thread thread;

        Client(int fd, int id, std::string addr)
            : fd(fd), id(id), remoteAddr(std::move(addr)) {}
    };

    void acceptLoop() {
        while (running_) {
            sockaddr_in clientAddr{};
            socklen_t   len = sizeof(clientAddr);

            fd_set fds; FD_ZERO(&fds); FD_SET(serverFd_, &fds);
            timeval tv{1, 0};
            int sel = ::select(serverFd_+1, &fds, nullptr, nullptr, &tv);
            if (sel <= 0) continue;

            int fd = ::accept(serverFd_, (sockaddr*)&clientAddr, &len);
            if (fd < 0) continue;

            // Short recv timeout so the read loop wakes up periodically for
            // keepalive checks.  EAGAIN/EWOULDBLOCK are handled gracefully below
            // and do NOT cause a disconnect.
            timeval rto{2, 0};
            ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &rto, sizeof(rto));

            // TCP-level keepalive as an OS-level backstop
            int ka = 1;
            ::setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &ka, sizeof(ka));

            std::string addr = inet_ntoa(clientAddr.sin_addr);
            int id = nextClientId_++;

            LOG_INFO("New TCP connection from " + addr +
                     " (id=" + std::to_string(id) + ")", "WSS");
            {
                std::lock_guard<std::mutex> lk(clientsMutex_);
                clients_.emplace_back(fd, id, addr);
                auto& c = clients_.back();
                c.thread = std::thread(&WebSocketServer::clientLoop, this, id, fd);
                c.thread.detach();
            }
            metrics_.activeClients++;
        }
    }

    void clientLoop(int clientId, int fd) {
        if (!doHandshake(clientId, fd)) {
            LOG_WARN("Handshake failed for client " + std::to_string(clientId), "WSS");
            ::close(fd);
            removeClient(clientId);
            metrics_.activeClients--;
            return;
        }

        LOG_INFO("WebSocket handshake OK (id=" + std::to_string(clientId) + ")", "WSS");
        if (connHandler_) connHandler_(clientId);

        std::vector<uint8_t> buf;
        uint8_t tmp[4096];

        // Keepalive state — only touched by this thread, no mutex needed
        auto lastActivity = std::chrono::steady_clock::now();
        auto pingSentAt   = std::chrono::steady_clock::time_point{};
        bool awaitingPong = false;

        while (running_) {
            ssize_t n = ::recv(fd, tmp, sizeof(tmp), 0);

            if (n > 0) {
                // Data received — reset idle clock
                lastActivity = std::chrono::steady_clock::now();
                awaitingPong = false;
                buf.insert(buf.end(), tmp, tmp + n);

            } else if (n == 0) {
                // Clean TCP close by peer
                break;

            } else {
                int err = errno;
                if (err == EAGAIN || err == EWOULDBLOCK || err == EINTR) {
                    // SO_RCVTIMEO fired — this is expected, not an error.
                    // Just fall through to the keepalive check below.
                } else {
                    // Genuine socket error (ECONNRESET, EPIPE, etc.)
                    LOG_DEBUG("recv errno=" + std::to_string(err) +
                              " on client " + std::to_string(clientId), "WSS");
                    break;
                }
            }

            // ── Parse any complete WebSocket frames in the buffer ──────────
            while (true) {
                ws::Frame frame;
                size_t consumed = ws::parseFrame(buf, frame);
                if (!consumed) break;
                buf.erase(buf.begin(), buf.begin() + consumed);

                switch (frame.opcode) {
                    case ws::Opcode::Text:
                    case ws::Opcode::Binary:
                        metrics_.totalMessagesReceived++;
                        if (msgHandler_) msgHandler_(clientId, frame.payload);
                        break;

                    case ws::Opcode::Ping:
                        // Peer-initiated ping → reply immediately
                        sendRaw(fd, ws::buildFrame(frame.payload, ws::Opcode::Pong));
                        break;

                    case ws::Opcode::Pong:
                        // Our ping was answered — client is alive
                        awaitingPong = false;
                        LOG_DEBUG("Pong from client " + std::to_string(clientId), "WSS");
                        break;

                    case ws::Opcode::Close:
                        sendRaw(fd, ws::buildClose());
                        goto done;

                    default: break;
                }
            }

            // ── Keepalive logic ────────────────────────────────────────────
            auto nowTp = std::chrono::steady_clock::now();

            if (awaitingPong) {
                auto waitSec = std::chrono::duration_cast<std::chrono::seconds>(
                                   nowTp - pingSentAt).count();
                if (waitSec >= pongTimeoutSec_) {
                    LOG_WARN("Client " + std::to_string(clientId) +
                             " did not pong within " +
                             std::to_string(pongTimeoutSec_) + "s — closing", "WSS");
                    sendRaw(fd, ws::buildClose(1001));
                    break;
                }
            } else {
                auto idleSec = std::chrono::duration_cast<std::chrono::seconds>(
                                   nowTp - lastActivity).count();
                if (idleSec >= pingIntervalSec_) {
                    LOG_DEBUG("Pinging idle client " + std::to_string(clientId), "WSS");
                    if (sendRaw(fd, ws::buildFrame("", ws::Opcode::Ping))) {
                        awaitingPong = true;
                        pingSentAt   = nowTp;
                        // Reset lastActivity so we don't immediately re-ping
                        lastActivity = nowTp;
                    } else {
                        break; // Can't send — socket is gone
                    }
                }
            }
        }
        done:
        LOG_INFO("Client " + std::to_string(clientId) + " disconnected", "WSS");
        if (disconnHandler_) disconnHandler_(clientId);
        ::close(fd);
        removeClient(clientId);
        metrics_.activeClients--;
    }

    bool doHandshake(int /*clientId*/, int fd) {
        std::string req;
        char buf[4096];
        while (req.find("\r\n\r\n") == std::string::npos) {
            ssize_t n = ::recv(fd, buf, sizeof(buf)-1, 0);
            if (n <= 0) return false;
            req.append(buf, n);
        }

        auto kp = req.find("Sec-WebSocket-Key:");
        if (kp == std::string::npos) return false;
        kp += 18;
        while (kp < req.size() && req[kp]==' ') ++kp;
        auto ke = req.find("\r\n", kp);
        if (ke == std::string::npos) return false;
        std::string key = req.substr(kp, ke - kp);

        std::string resp =
            "HTTP/1.1 101 Switching Protocols\r\n"
            "Upgrade: websocket\r\n"
            "Connection: Upgrade\r\n"
            "Sec-WebSocket-Accept: " + wsAcceptKey(key) + "\r\n"
            "\r\n";
        return sendRaw(fd, resp);
    }

    static bool sendRaw(int fd, const std::string& data) {
        size_t sent = 0;
        while (sent < data.size()) {
            ssize_t n = ::send(fd, data.data()+sent, data.size()-sent, MSG_NOSIGNAL);
            if (n <= 0) return false;
            sent += n;
        }
        return true;
    }

    void removeClient(int clientId) {
        std::lock_guard<std::mutex> lk(clientsMutex_);
        clients_.erase(
            std::remove_if(clients_.begin(), clients_.end(),
                           [clientId](const Client& c){ return c.id==clientId; }),
            clients_.end());
    }

    using Iter = std::vector<Client>::iterator;
    Iter findClient_(int id) {
        return std::find_if(clients_.begin(), clients_.end(),
                            [id](const Client& c){ return c.id==id; });
    }

    using TP = std::chrono::steady_clock::time_point;
    static TP     now()            { return std::chrono::steady_clock::now(); }
    static double elapsed(TP t0)   {
        return std::chrono::duration<double, std::milli>(
                   std::chrono::steady_clock::now() - t0).count();
    }
    static std::string fmtMs(double ms) {
        std::ostringstream ss; ss.precision(3); ss << std::fixed << ms; return ss.str();
    }

    uint16_t               port_;
    int                    serverFd_;
    std::atomic<bool>      running_;
    std::thread            acceptThread_;
    mutable std::mutex     clientsMutex_;
    std::vector<Client>    clients_;
    std::atomic<int>       nextClientId_{1};
    Metrics                metrics_;
    int                    pingIntervalSec_;
    int                    pongTimeoutSec_;

    MessageHandler         msgHandler_;
    ConnectHandler         connHandler_;
    DisconnectHandler      disconnHandler_;
};
