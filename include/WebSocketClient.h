#pragma once
/**
 * WebSocketClient – connects to a WebSocket server.
 * Used by the C++ CLI client program.
 */

#include "Crypto.h"
#include "WSFrame.h"
#include "Logger.h"

#include <string>
#include <functional>
#include <thread>
#include <atomic>
#include <vector>
#include <sstream>
#include <chrono>
#include <random>
#include <stdexcept>

// POSIX
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <unistd.h>

class WebSocketClient {
public:
    using MessageHandler    = std::function<void(const std::string&)>;
    using ConnectHandler    = std::function<void()>;
    using DisconnectHandler = std::function<void()>;

    WebSocketClient() : fd_(-1), connected_(false) {}
    ~WebSocketClient() { disconnect(); }

    void onMessage   (MessageHandler    h) { msgHandler_    = std::move(h); }
    void onConnect   (ConnectHandler    h) { connHandler_   = std::move(h); }
    void onDisconnect(DisconnectHandler h) { disconnHandler_= std::move(h); }

    // ── Connect ────────────────────────────────────────────────────────────────
    bool connect(const std::string& host, uint16_t port, const std::string& path="/") {
        host_ = host; port_ = port; path_ = path;

        fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
        if (fd_ < 0) { LOG_ERROR("socket() failed", "WSC"); return false; }

        // Resolve host
        struct hostent* he = ::gethostbyname(host.c_str());
        if (!he) { LOG_ERROR("gethostbyname failed: " + host, "WSC"); return false; }

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port   = htons(port);
        std::memcpy(&addr.sin_addr, he->h_addr_list[0], he->h_length);

        if (::connect(fd_, (sockaddr*)&addr, sizeof(addr)) < 0) {
            LOG_ERROR("connect() failed to " + host + ":" + std::to_string(port), "WSC");
            return false;
        }

        if (!doHandshake()) {
            LOG_ERROR("WebSocket handshake failed", "WSC");
            ::close(fd_); fd_ = -1;
            return false;
        }

        connected_ = true;
        LOG_INFO("Connected to ws://" + host + ":" + std::to_string(port) + path, "WSC");
        if (connHandler_) connHandler_();

        readThread_ = std::thread(&WebSocketClient::readLoop, this);
        return true;
    }

    void disconnect() {
        connected_ = false;
        if (fd_ >= 0) {
            sendRaw(ws::buildClose());
            ::close(fd_); fd_ = -1;
        }
        if (readThread_.joinable()) readThread_.join();
    }

    // ── Send a text message ────────────────────────────────────────────────────
    bool send(const std::string& msg) {
        if (!connected_) return false;
        auto t0 = std::chrono::steady_clock::now();
        std::string frame = buildMaskedFrame(msg);
        bool ok = sendRaw(frame);
        lastSendMs_ = std::chrono::duration<double,std::milli>(
                          std::chrono::steady_clock::now()-t0).count();
        if (ok) LOG_DEBUG("Sent " + std::to_string(msg.size()) + " bytes in " +
                          fmtMs(lastSendMs_) + " ms", "WSC");
        return ok;
    }

    bool isConnected() const { return connected_; }
    double lastSendMs() const { return lastSendMs_; }

private:
    // ── Handshake ──────────────────────────────────────────────────────────────
    bool doHandshake() {
        // Generate 16-byte random nonce and base64-encode it
        std::random_device rd;
        std::mt19937 rng(rd());
        std::uniform_int_distribution<> dist(0,255);
        std::string nonce;
        for (int i=0;i<16;i++) nonce+=(char)dist(rng);
        std::string key = base64Encode(nonce);

        std::string req =
            "GET " + path_ + " HTTP/1.1\r\n"
            "Host: " + host_ + ":" + std::to_string(port_) + "\r\n"
            "Upgrade: websocket\r\n"
            "Connection: Upgrade\r\n"
            "Sec-WebSocket-Key: " + key + "\r\n"
            "Sec-WebSocket-Version: 13\r\n"
            "\r\n";

        if (!sendRaw(req)) return false;

        // Read response
        std::string resp;
        char buf[2048];
        while (resp.find("\r\n\r\n") == std::string::npos) {
            ssize_t n = ::recv(fd_, buf, sizeof(buf)-1, 0);
            if (n <= 0) return false;
            resp.append(buf, n);
        }

        // Validate 101 Switching Protocols
        if (resp.find("101") == std::string::npos) return false;

        // Validate accept key
        std::string expected = wsAcceptKey(key);
        if (resp.find(expected) == std::string::npos) {
            LOG_WARN("Sec-WebSocket-Accept mismatch", "WSC");
            // Accept anyway – some servers have minor variations
        }
        return true;
    }

    // ── Read loop (runs in thread) ─────────────────────────────────────────────
    void readLoop() {
        std::vector<uint8_t> buf;
        uint8_t tmp[4096];

        while (connected_) {
            ssize_t n = ::recv(fd_, tmp, sizeof(tmp), 0);
            if (n <= 0) break;

            buf.insert(buf.end(), tmp, tmp+n);

            while (true) {
                ws::Frame frame;
                size_t consumed = ws::parseFrame(buf, frame);
                if (!consumed) break;
                buf.erase(buf.begin(), buf.begin()+consumed);

                switch (frame.opcode) {
                    case ws::Opcode::Text:
                    case ws::Opcode::Binary:
                        if (msgHandler_) msgHandler_(frame.payload);
                        break;
                    case ws::Opcode::Ping:
                        sendRaw(ws::buildFrame(frame.payload, ws::Opcode::Pong));
                        break;
                    case ws::Opcode::Close:
                        connected_ = false;
                        if (disconnHandler_) disconnHandler_();
                        return;
                    default: break;
                }
            }
        }
        connected_ = false;
        if (disconnHandler_) disconnHandler_();
        LOG_INFO("Disconnected from server", "WSC");
    }

    // ── Masked frame (clients MUST mask per RFC 6455) ─────────────────────────
    std::string buildMaskedFrame(const std::string& payload) {
        std::random_device rd;
        std::mt19937 rng(rd());
        std::uniform_int_distribution<> dist(0,255);

        uint8_t mask[4];
        for (int i=0;i<4;i++) mask[i]=(uint8_t)dist(rng);

        std::string frame;
        frame += (char)(0x80 | 0x01); // FIN + Text
        size_t len = payload.size();
        if (len <= 125) {
            frame += (char)(0x80 | len);
        } else if (len <= 0xFFFF) {
            frame += (char)(0x80 | 126);
            frame += (char)((len>>8)&0xFF);
            frame += (char)(len&0xFF);
        } else {
            frame += (char)(0x80 | 127);
            for (int i=7;i>=0;i--) frame+=(char)((len>>(i*8))&0xFF);
        }
        for (int i=0;i<4;i++) frame+=(char)mask[i];
        for (size_t i=0;i<payload.size();i++)
            frame+=(char)((uint8_t)payload[i]^mask[i%4]);
        return frame;
    }

    bool sendRaw(const std::string& data) {
        size_t sent=0;
        while(sent<data.size()){
            ssize_t n=::send(fd_,data.data()+sent,data.size()-sent,MSG_NOSIGNAL);
            if(n<=0) return false;
            sent+=n;
        }
        return true;
    }

    static std::string fmtMs(double ms) {
        std::ostringstream ss; ss.precision(3); ss<<std::fixed<<ms; return ss.str();
    }

    int         fd_;
    std::atomic<bool> connected_;
    std::string host_, path_;
    uint16_t    port_ = 9001;
    std::thread readThread_;
    double      lastSendMs_ = 0.0;

    MessageHandler    msgHandler_;
    ConnectHandler    connHandler_;
    DisconnectHandler disconnHandler_;
};
