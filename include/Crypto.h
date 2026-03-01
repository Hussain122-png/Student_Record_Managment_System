#pragma once
// SHA-1 (RFC 3174) + Base64 utilities used for WebSocket handshake.
// SHA-1 implementation derived from public-domain code by Steve Reid.

#include <string>
#include <cstdint>
#include <cstring>
#include <sstream>
#include <iomanip>

// ── SHA-1 ──────────────────────────────────────────────────────────────────────
namespace sha1_impl {

constexpr uint32_t K0 = 0x5A827999u;
constexpr uint32_t K1 = 0x6ED9EBA1u;
constexpr uint32_t K2 = 0x8F1BBCDCu;
constexpr uint32_t K3 = 0xCA62C1D6u;

inline uint32_t rol32(uint32_t x, int n) { return (x << n) | (x >> (32-n)); }

struct SHA1Context {
    uint32_t state[5] = {0x67452301,0xEFCDAB89,0x98BADCFE,0x10325476,0xC3D2E1F0};
    uint64_t count = 0;
    uint8_t  buf[64]{};
    uint32_t bufLen = 0;
};

inline void processBlock(SHA1Context& ctx, const uint8_t* blk) {
    uint32_t W[80];
    for (int i=0;i<16;i++)
        W[i] = ((uint32_t)blk[i*4]<<24)|((uint32_t)blk[i*4+1]<<16)|
               ((uint32_t)blk[i*4+2]<<8)|(uint32_t)blk[i*4+3];
    for (int i=16;i<80;i++) W[i]=rol32(W[i-3]^W[i-8]^W[i-14]^W[i-16],1);

    uint32_t a=ctx.state[0],b=ctx.state[1],c=ctx.state[2],d=ctx.state[3],e=ctx.state[4];
    for(int i=0;i<80;i++){
        uint32_t f,k;
        if(i<20){f=(b&c)|(~b&d);k=K0;}
        else if(i<40){f=b^c^d;k=K1;}
        else if(i<60){f=(b&c)|(b&d)|(c&d);k=K2;}
        else{f=b^c^d;k=K3;}
        uint32_t tmp=rol32(a,5)+f+e+k+W[i];
        e=d;d=c;c=rol32(b,30);b=a;a=tmp;
    }
    ctx.state[0]+=a;ctx.state[1]+=b;ctx.state[2]+=c;
    ctx.state[3]+=d;ctx.state[4]+=e;
}

inline void update(SHA1Context& ctx, const uint8_t* data, size_t len) {
    ctx.count += len * 8;
    while(len--) {
        ctx.buf[ctx.bufLen++] = *data++;
        if(ctx.bufLen==64) { processBlock(ctx,ctx.buf); ctx.bufLen=0; }
    }
}

inline void finalise(SHA1Context& ctx, uint8_t digest[20]) {
    uint64_t bc = ctx.count;
    uint8_t pad = 0x80;
    update(ctx, &pad, 1);
    while(ctx.bufLen != 56) { uint8_t z=0; update(ctx,&z,1); }
    uint8_t len8[8];
    for(int i=7;i>=0;i--){ len8[i]=(uint8_t)(bc&0xFF); bc>>=8; }
    update(ctx, len8, 8);
    for(int i=0;i<5;i++){
        digest[i*4]   = (uint8_t)(ctx.state[i]>>24);
        digest[i*4+1] = (uint8_t)(ctx.state[i]>>16);
        digest[i*4+2] = (uint8_t)(ctx.state[i]>>8);
        digest[i*4+3] = (uint8_t)(ctx.state[i]);
    }
}
} // namespace sha1_impl

inline std::string sha1(const std::string& input) {
    sha1_impl::SHA1Context ctx;
    sha1_impl::update(ctx, (const uint8_t*)input.data(), input.size());
    uint8_t digest[20]{};
    sha1_impl::finalise(ctx, digest);
    return std::string((char*)digest, 20);
}

// ── Base64 ─────────────────────────────────────────────────────────────────────
inline std::string base64Encode(const std::string& input) {
    static const char* B64 =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    const auto* d = (const unsigned char*)input.data();
    size_t n = input.size();
    for(size_t i=0;i<n;i+=3){
        uint32_t v=(uint32_t)d[i]<<16;
        if(i+1<n) v|=(uint32_t)d[i+1]<<8;
        if(i+2<n) v|=(uint32_t)d[i+2];
        out+=B64[(v>>18)&63];
        out+=B64[(v>>12)&63];
        out+=(i+1<n)?B64[(v>>6)&63]:'=';
        out+=(i+2<n)?B64[v&63]:'=';
    }
    return out;
}

inline std::string base64Decode(const std::string& input) {
    static const int8_t dec[256] = {
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,62,-1,-1,-1,63,
        52,53,54,55,56,57,58,59,60,61,-1,-1,-1,-1,-1,-1,
        -1, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9,10,11,12,13,14,
        15,16,17,18,19,20,21,22,23,24,25,-1,-1,-1,-1,-1,
        -1,26,27,28,29,30,31,32,33,34,35,36,37,38,39,40,
        41,42,43,44,45,46,47,48,49,50,51,-1,-1,-1,-1,-1,
    };
    std::string out;
    const auto* d = (const unsigned char*)input.data();
    size_t n = input.size();
    for(size_t i=0;i+3<n+1;i+=4){
        if(i+3>=n) break;
        uint32_t v = ((uint32_t)dec[d[i]]<<18)|((uint32_t)dec[d[i+1]]<<12);
        out += (char)(v>>16);
        if(d[i+2]!='='){ v|=(uint32_t)dec[d[i+2]]<<6; out+=(char)((v>>8)&0xFF); }
        if(d[i+3]!='='){ v|=(uint32_t)dec[d[i+3]];    out+=(char)(v&0xFF); }
    }
    return out;
}

// ── WebSocket accept key ───────────────────────────────────────────────────────
inline std::string wsAcceptKey(const std::string& clientKey) {
    return base64Encode(sha1(clientKey + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"));
}
