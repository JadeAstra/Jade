// ChaCha20 and Poly1305, and the AEAD made of them (RFC 8439). Additions, rotations and XORs
// only, so constant-time as written; Poly1305 in 26-bit limbs (poly1305-donna's way), whose
// products fit 64 bits.

#include "crypto.h"

namespace ww {
namespace crypto {

namespace {

inline void quarter(uint32_t& a, uint32_t& b, uint32_t& c, uint32_t& d) {
    a += b;
    d = rotl32(d ^ a, 16);
    c += d;
    b = rotl32(b ^ c, 12);
    a += b;
    d = rotl32(d ^ a, 8);
    c += d;
    b = rotl32(b ^ c, 7);
}

void chachaBlock(const uint32_t input[16], uint8_t out[64]) {
    uint32_t x[16];
    std::memcpy(x, input, sizeof(x));
    for (int i = 0; i < 10; i++) {
        quarter(x[0], x[4], x[8], x[12]);
        quarter(x[1], x[5], x[9], x[13]);
        quarter(x[2], x[6], x[10], x[14]);
        quarter(x[3], x[7], x[11], x[15]);
        quarter(x[0], x[5], x[10], x[15]);
        quarter(x[1], x[6], x[11], x[12]);
        quarter(x[2], x[7], x[8], x[13]);
        quarter(x[3], x[4], x[9], x[14]);
    }
    for (int i = 0; i < 16; i++) store32le(out + 4 * i, x[i] + input[i]);
    wipe(x, sizeof(x));
}

class Poly1305 {
public:
    explicit Poly1305(const uint8_t key[32]) {
        r_[0] = load32le(key) & 0x3ffffff;
        r_[1] = (load32le(key + 3) >> 2) & 0x3ffff03;
        r_[2] = (load32le(key + 6) >> 4) & 0x3ffc0ff;
        r_[3] = (load32le(key + 9) >> 6) & 0x3f03fff;
        r_[4] = (load32le(key + 12) >> 8) & 0x00fffff;
        for (int i = 0; i < 4; i++) pad_[i] = load32le(key + 16 + 4 * i);
    }
    ~Poly1305() {
        wipe(r_, sizeof(r_));
        wipe(h_, sizeof(h_));
        wipe(pad_, sizeof(pad_));
        wipe(buffer_, sizeof(buffer_));
    }
    void update(const uint8_t* m, size_t size) {
        if (held_ > 0) {
            const size_t now = size < 16 - held_ ? size : 16 - held_;
            std::memcpy(buffer_ + held_, m, now);
            held_ += now;
            m += now;
            size -= now;
            if (held_ < 16) return;
            blocks(buffer_, 16, 1u << 24);
            held_ = 0;
        }
        if (size >= 16) {
            blocks(m, size & ~size_t(15), 1u << 24);
            m += size & ~size_t(15);
            size &= 15;
        }
        if (size > 0) {
            std::memcpy(buffer_, m, size);
            held_ = size;
        }
    }
    // Zeros up to the next 16 bytes, as the AEAD pads each part.
    void padTo16() {
        if (held_ == 0) return;
        std::memset(buffer_ + held_, 0, 16 - held_);
        blocks(buffer_, 16, 1u << 24);
        held_ = 0;
    }
    void finish(uint8_t mac[16]) {
        if (held_ > 0) {
            buffer_[held_] = 1;
            std::memset(buffer_ + held_ + 1, 0, 16 - held_ - 1);
            blocks(buffer_, 16, 0);
        }
        uint32_t h0 = h_[0], h1 = h_[1], h2 = h_[2], h3 = h_[3], h4 = h_[4], c;
        c = h1 >> 26, h1 &= 0x3ffffff;
        h2 += c, c = h2 >> 26, h2 &= 0x3ffffff;
        h3 += c, c = h3 >> 26, h3 &= 0x3ffffff;
        h4 += c, c = h4 >> 26, h4 &= 0x3ffffff;
        h0 += c * 5, c = h0 >> 26, h0 &= 0x3ffffff;
        h1 += c;
        // h - p, and whichever of h and h - p is in range, chosen by a mask.
        uint32_t g0 = h0 + 5;
        c = g0 >> 26, g0 &= 0x3ffffff;
        uint32_t g1 = h1 + c;
        c = g1 >> 26, g1 &= 0x3ffffff;
        uint32_t g2 = h2 + c;
        c = g2 >> 26, g2 &= 0x3ffffff;
        uint32_t g3 = h3 + c;
        c = g3 >> 26, g3 &= 0x3ffffff;
        const uint32_t g4 = h4 + c - (1u << 26);
        uint32_t mask = (g4 >> 31) - 1;
        h0 = (h0 & ~mask) | (g0 & mask);
        h1 = (h1 & ~mask) | (g1 & mask);
        h2 = (h2 & ~mask) | (g2 & mask);
        h3 = (h3 & ~mask) | (g3 & mask);
        h4 = (h4 & ~mask) | (g4 & mask);
        const uint32_t w0 = h0 | (h1 << 26);
        const uint32_t w1 = (h1 >> 6) | (h2 << 20);
        const uint32_t w2 = (h2 >> 12) | (h3 << 14);
        const uint32_t w3 = (h3 >> 18) | (h4 << 8);
        uint64_t f = uint64_t(w0) + pad_[0];
        store32le(mac, uint32_t(f));
        f = uint64_t(w1) + pad_[1] + (f >> 32);
        store32le(mac + 4, uint32_t(f));
        f = uint64_t(w2) + pad_[2] + (f >> 32);
        store32le(mac + 8, uint32_t(f));
        f = uint64_t(w3) + pad_[3] + (f >> 32);
        store32le(mac + 12, uint32_t(f));
    }

private:
    void blocks(const uint8_t* m, size_t size, uint32_t hibit) {
        const uint32_t r0 = r_[0], r1 = r_[1], r2 = r_[2], r3 = r_[3], r4 = r_[4];
        const uint32_t s1 = r1 * 5, s2 = r2 * 5, s3 = r3 * 5, s4 = r4 * 5;
        uint32_t h0 = h_[0], h1 = h_[1], h2 = h_[2], h3 = h_[3], h4 = h_[4];
        while (size >= 16) {
            h0 += load32le(m) & 0x3ffffff;
            h1 += (load32le(m + 3) >> 2) & 0x3ffffff;
            h2 += (load32le(m + 6) >> 4) & 0x3ffffff;
            h3 += (load32le(m + 9) >> 6) & 0x3ffffff;
            h4 += (load32le(m + 12) >> 8) | hibit;
            const uint64_t d0 = uint64_t(h0) * r0 + uint64_t(h1) * s4 + uint64_t(h2) * s3 + uint64_t(h3) * s2 + uint64_t(h4) * s1;
            uint64_t d1 = uint64_t(h0) * r1 + uint64_t(h1) * r0 + uint64_t(h2) * s4 + uint64_t(h3) * s3 + uint64_t(h4) * s2;
            uint64_t d2 = uint64_t(h0) * r2 + uint64_t(h1) * r1 + uint64_t(h2) * r0 + uint64_t(h3) * s4 + uint64_t(h4) * s3;
            uint64_t d3 = uint64_t(h0) * r3 + uint64_t(h1) * r2 + uint64_t(h2) * r1 + uint64_t(h3) * r0 + uint64_t(h4) * s4;
            uint64_t d4 = uint64_t(h0) * r4 + uint64_t(h1) * r3 + uint64_t(h2) * r2 + uint64_t(h3) * r1 + uint64_t(h4) * r0;
            uint32_t c = uint32_t(d0 >> 26);
            h0 = uint32_t(d0) & 0x3ffffff;
            d1 += c, c = uint32_t(d1 >> 26), h1 = uint32_t(d1) & 0x3ffffff;
            d2 += c, c = uint32_t(d2 >> 26), h2 = uint32_t(d2) & 0x3ffffff;
            d3 += c, c = uint32_t(d3 >> 26), h3 = uint32_t(d3) & 0x3ffffff;
            d4 += c, c = uint32_t(d4 >> 26), h4 = uint32_t(d4) & 0x3ffffff;
            h0 += c * 5, c = h0 >> 26, h0 &= 0x3ffffff;
            h1 += c;
            m += 16;
            size -= 16;
        }
        h_[0] = h0, h_[1] = h1, h_[2] = h2, h_[3] = h3, h_[4] = h4;
    }

    uint32_t r_[5], h_[5] = {0, 0, 0, 0, 0}, pad_[4];
    uint8_t buffer_[16];
    size_t held_ = 0;
};

void setup(uint32_t state[16], const uint8_t key[32], const uint8_t nonce[12], uint32_t counter) {
    state[0] = 0x61707865;
    state[1] = 0x3320646e;
    state[2] = 0x79622d32;
    state[3] = 0x6b206574;
    for (int i = 0; i < 8; i++) state[4 + i] = load32le(key + 4 * i);
    state[12] = counter;
    for (int i = 0; i < 3; i++) state[13 + i] = load32le(nonce + 4 * i);
}

class ChaCha20Poly1305 final : public Aead {
public:
    explicit ChaCha20Poly1305(const uint8_t* key) { std::memcpy(key_, key, 32); }
    ~ChaCha20Poly1305() override { wipe(key_, sizeof(key_)); }
    bool nonceFits(size_t nonceSize) const override { return nonceSize == 12; }

    void seal(const uint8_t* nonce, size_t, const uint8_t* aad, size_t aadSize, const uint8_t* in, size_t size, uint8_t* out, uint8_t* tag) override {
        chacha20(key_, nonce, 1, in, out, size);
        mac(nonce, aad, aadSize, out, size, tag);
    }
    bool open(const uint8_t* nonce, size_t, const uint8_t* aad, size_t aadSize, const uint8_t* in, size_t size, const uint8_t* tag, uint8_t* out) override {
        uint8_t expected[16];
        mac(nonce, aad, aadSize, in, size, expected);
        const bool authentic = equal(expected, tag, 16);
        wipe(expected, sizeof(expected));
        if (!authentic) return false;
        chacha20(key_, nonce, 1, in, out, size);
        return true;
    }

private:
    void mac(const uint8_t* nonce, const uint8_t* aad, size_t aadSize, const uint8_t* text, size_t size, uint8_t tag[16]) const {
        uint32_t state[16];
        uint8_t block[64];
        setup(state, key_, nonce, 0);
        chachaBlock(state, block);
        Poly1305 p(block);
        p.update(aad, aadSize);
        p.padTo16();
        p.update(text, size);
        p.padTo16();
        uint8_t lengths[16];
        store64le(lengths, aadSize);
        store64le(lengths + 8, size);
        p.update(lengths, 16);
        p.finish(tag);
        wipe(block, sizeof(block));
        wipe(state, sizeof(state));
    }
    uint8_t key_[32];
};

}  // namespace

void chacha20(const uint8_t key[32], const uint8_t nonce[12], uint32_t counter, const uint8_t* in, uint8_t* out, size_t size) {
    uint32_t state[16];
    uint8_t stream[64];
    setup(state, key, nonce, counter);
    while (size > 0) {
        chachaBlock(state, stream);
        state[12]++;
        const size_t now = size < 64 ? size : 64;
        for (size_t i = 0; i < now; i++) out[i] = in[i] ^ stream[i];
        in += now;
        out += now;
        size -= now;
    }
    wipe(stream, sizeof(stream));
    wipe(state, sizeof(state));
}

std::unique_ptr<Aead> newChaCha20Poly1305(const uint8_t* key, size_t keySize) {
    if (keySize != 32) return nullptr;
    return std::make_unique<ChaCha20Poly1305>(key);
}

}  // namespace crypto
}  // namespace ww
