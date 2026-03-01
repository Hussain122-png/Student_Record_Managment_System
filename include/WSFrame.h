#pragma once
// WebSocket RFC 6455 frame encoding / decoding

#include <string>
#include <vector>
#include <cstdint>
#include <stdexcept>

namespace ws {

enum class Opcode : uint8_t {
    Continuation = 0x0,
    Text         = 0x1,
    Binary       = 0x2,
    Close        = 0x8,
    Ping         = 0x9,
    Pong         = 0xA,
};

struct Frame {
    bool    fin    = true;
    Opcode  opcode = Opcode::Text;
    bool    masked = false;
    std::string payload;
};

// ── Build a server→client frame (unmasked) ────────────────────────────────────
inline std::string buildFrame(const std::string& payload,
                               Opcode opcode = Opcode::Text) {
    std::string frame;
    frame += (char)(0x80 | (uint8_t)opcode); // FIN + opcode

    size_t len = payload.size();
    if (len <= 125) {
        frame += (char)len;
    } else if (len <= 0xFFFF) {
        frame += (char)126;
        frame += (char)((len >> 8) & 0xFF);
        frame += (char)(len & 0xFF);
    } else {
        frame += (char)127;
        for (int i = 7; i >= 0; --i)
            frame += (char)((len >> (i*8)) & 0xFF);
    }
    frame += payload;
    return frame;
}

// ── Build a close frame ───────────────────────────────────────────────────────
inline std::string buildClose(uint16_t code = 1000) {
    std::string payload;
    payload += (char)((code >> 8) & 0xFF);
    payload += (char)(code & 0xFF);
    return buildFrame(payload, Opcode::Close);
}

// ── Parse frames from raw buffer (returns consumed bytes, 0=incomplete) ───────
// result: populated frame; returns number of bytes consumed (0 = need more data)
inline size_t parseFrame(const std::vector<uint8_t>& buf, Frame& out) {
    if (buf.size() < 2) return 0;

    out.fin    = (buf[0] & 0x80) != 0;
    out.opcode = static_cast<Opcode>(buf[0] & 0x0F);
    out.masked = (buf[1] & 0x80) != 0;

    size_t payloadLen = buf[1] & 0x7F;
    size_t headerLen  = 2;

    if (payloadLen == 126) {
        if (buf.size() < 4) return 0;
        payloadLen = ((size_t)buf[2] << 8) | buf[3];
        headerLen = 4;
    } else if (payloadLen == 127) {
        if (buf.size() < 10) return 0;
        payloadLen = 0;
        for (int i = 0; i < 8; ++i)
            payloadLen = (payloadLen << 8) | buf[2+i];
        headerLen = 10;
    }

    size_t maskOffset = headerLen;
    if (out.masked) headerLen += 4;

    if (buf.size() < headerLen + payloadLen) return 0;

    out.payload.resize(payloadLen);
    if (out.masked) {
        const uint8_t* mask = buf.data() + maskOffset;
        for (size_t i = 0; i < payloadLen; ++i)
            out.payload[i] = (char)(buf[headerLen + i] ^ mask[i % 4]);
    } else {
        out.payload.assign((char*)(buf.data() + headerLen), payloadLen);
    }

    return headerLen + payloadLen;
}

} // namespace ws
