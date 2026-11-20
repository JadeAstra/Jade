// BLAKE2b (RFC 7693) and Argon2id (RFC 9106, version 0x13), the password hash. Argon2id's first
// half pass reads memory at places that depend only on the parameters (as Argon2i); the rest at
// places the data chooses (as Argon2d), by design. Lanes are worked one after another here: the
// result is the same as working them at once, which the RFC allows.

#include "crypto.h"

#include <new>
#include <vector>

namespace ww {
namespace crypto {

// ---- BLAKE2b ----

namespace {

const uint64_t blake2bIv[8] = {0x6a09e667f3bcc908ULL, 0xbb67ae8584caa73bULL, 0x3c6ef372fe94f82bULL, 0xa54ff53a5f1d36f1ULL,
                               0x510e527fade682d1ULL, 0x9b05688c2b3e6c1fULL, 0x1f83d9abfb41bd6bULL, 0x5be0cd19137e2179ULL};
const uint8_t sigma[12][16] = {
    {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15}, {14, 10, 4, 8, 9, 15, 13, 6, 1, 12, 0, 2, 11, 7, 5, 3},
    {11, 8, 12, 0, 5, 2, 15, 13, 10, 14, 3, 6, 7, 1, 9, 4}, {7, 9, 3, 1, 13, 12, 11, 14, 2, 6, 5, 10, 4, 0, 15, 8},
    {9, 0, 5, 7, 2, 4, 10, 15, 14, 1, 11, 12, 6, 8, 3, 13}, {2, 12, 6, 10, 0, 11, 8, 3, 4, 13, 7, 5, 15, 14, 1, 9},
    {12, 5, 1, 15, 14, 13, 4, 10, 0, 7, 6, 3, 9, 2, 8, 11}, {13, 11, 7, 14, 12, 1, 3, 9, 5, 0, 15, 4, 8, 6, 2, 10},
    {6, 15, 14, 9, 11, 3, 0, 8, 12, 2, 13, 7, 1, 4, 10, 5}, {10, 2, 8, 4, 7, 6, 1, 5, 15, 11, 9, 14, 3, 12, 13, 0},
    {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15}, {14, 10, 4, 8, 9, 15, 13, 6, 1, 12, 0, 2, 11, 7, 5, 3}};

struct Blake2b {
    uint64_t h[8];
    uint64_t t[2] = {0, 0};
    uint8_t buffer[128];
    size_t held = 0;
    size_t outSize;

    Blake2b(size_t out, const uint8_t* key, size_t keySize) : outSize(out) {
        for (int i = 0; i < 8; i++) h[i] = blake2bIv[i];
        h[0] ^= 0x01010000ULL ^ (uint64_t(keySize) << 8) ^ out;
        if (keySize > 0) {
            uint8_t block[128] = {};
            std::memcpy(block, key, keySize);
            update(block, 128);
            wipe(block, sizeof(block));
        }
    }
    ~Blake2b() {
        wipe(h, sizeof(h));
        wipe(buffer, sizeof(buffer));
    }

    void compress(const uint8_t* block, bool last) {
        uint64_t m[16], v[16];
        for (int i = 0; i < 16; i++) m[i] = load64le(block + 8 * i);
        for (int i = 0; i < 8; i++) {
            v[i] = h[i];
            v[i + 8] = blake2bIv[i];
        }
        v[12] ^= t[0];
        v[13] ^= t[1];
        if (last) v[14] = ~v[14];
        auto g = [&](int a, int b, int c, int d, uint64_t x, uint64_t y) {
            v[a] = v[a] + v[b] + x;
            v[d] = rotr64(v[d] ^ v[a], 32);
            v[c] = v[c] + v[d];
            v[b] = rotr64(v[b] ^ v[c], 24);
            v[a] = v[a] + v[b] + y;
            v[d] = rotr64(v[d] ^ v[a], 16);
            v[c] = v[c] + v[d];
            v[b] = rotr64(v[b] ^ v[c], 63);
        };
        for (int r = 0; r < 12; r++) {
            const uint8_t* s = sigma[r];
            g(0, 4, 8, 12, m[s[0]], m[s[1]]);
            g(1, 5, 9, 13, m[s[2]], m[s[3]]);
            g(2, 6, 10, 14, m[s[4]], m[s[5]]);
            g(3, 7, 11, 15, m[s[6]], m[s[7]]);
            g(0, 5, 10, 15, m[s[8]], m[s[9]]);
            g(1, 6, 11, 12, m[s[10]], m[s[11]]);
            g(2, 7, 8, 13, m[s[12]], m[s[13]]);
            g(3, 4, 9, 14, m[s[14]], m[s[15]]);
        }
        for (int i = 0; i < 8; i++) h[i] ^= v[i] ^ v[i + 8];
        wipe(m, sizeof(m));
        wipe(v, sizeof(v));
    }
    void count(size_t n) {
        t[0] += n;
        if (t[0] < n) t[1]++;
    }
    void update(const uint8_t* data, size_t size) {
        // The last block is compressed only at the end, flagged so: one full block is held back.
        while (size > 0) {
            if (held == 128) {
                count(128);
                compress(buffer, false);
                held = 0;
            }
            const size_t now = size < 128 - held ? size : 128 - held;
            std::memcpy(buffer + held, data, now);
            held += now;
            data += now;
            size -= now;
        }
    }
    void finish(uint8_t* out) {
        count(held);
        std::memset(buffer + held, 0, 128 - held);
        compress(buffer, true);
        uint8_t all[64];
        for (int i = 0; i < 8; i++) store64le(all + 8 * i, h[i]);
        std::memcpy(out, all, outSize);
        wipe(all, sizeof(all));
    }
};

}  // namespace

void blake2b(uint8_t* out, size_t outSize, const uint8_t* in, size_t inSize, const uint8_t* key, size_t keySize) {
    Blake2b b(outSize, key, keySize);
    b.update(in, inSize);
    b.finish(out);
}

// ---- Argon2id ----

namespace {

constexpr size_t kBlockWords = 128;  // a block is 1024 bytes
struct Block {
    uint64_t v[kBlockWords];
};

void le32(std::vector<uint8_t>& out, uint32_t v) {
    uint8_t b[4];
    store32le(b, v);
    out.insert(out.end(), b, b + 4);
}

// H' (RFC 9106, 3.3): a hash of any length from BLAKE2b's.
void hashLong(uint8_t* out, size_t outSize, const uint8_t* in, size_t inSize) {
    uint8_t length[4];
    store32le(length, static_cast<uint32_t>(outSize));
    if (outSize <= 64) {
        Blake2b b(outSize, nullptr, 0);
        b.update(length, 4);
        b.update(in, inSize);
        b.finish(out);
        return;
    }
    uint8_t v[64];
    {
        Blake2b b(64, nullptr, 0);
        b.update(length, 4);
        b.update(in, inSize);
        b.finish(v);
    }
    const size_t r = (outSize + 31) / 32 - 2;
    std::memcpy(out, v, 32);
    for (size_t i = 1; i < r; i++) {
        blake2b(v, 64, v, 64);
        std::memcpy(out + 32 * i, v, 32);
    }
    blake2b(out + 32 * r, outSize - 32 * r, v, 64);
    wipe(v, sizeof(v));
}

inline uint64_t blaMka(uint64_t x, uint64_t y) { return x + y + 2 * (x & 0xFFFFFFFFULL) * (y & 0xFFFFFFFFULL); }

inline void mix(uint64_t& a, uint64_t& b, uint64_t& c, uint64_t& d) {
    a = blaMka(a, b);
    d = rotr64(d ^ a, 32);
    c = blaMka(c, d);
    b = rotr64(b ^ c, 24);
    a = blaMka(a, b);
    d = rotr64(d ^ a, 16);
    c = blaMka(c, d);
    b = rotr64(b ^ c, 63);
}

// BLAKE2b's round, without its message, on 16 words.
inline void round16(uint64_t& v0, uint64_t& v1, uint64_t& v2, uint64_t& v3, uint64_t& v4, uint64_t& v5, uint64_t& v6, uint64_t& v7, uint64_t& v8, uint64_t& v9,
                    uint64_t& v10, uint64_t& v11, uint64_t& v12, uint64_t& v13, uint64_t& v14, uint64_t& v15) {
    mix(v0, v4, v8, v12);
    mix(v1, v5, v9, v13);
    mix(v2, v6, v10, v14);
    mix(v3, v7, v11, v15);
    mix(v0, v5, v10, v15);
    mix(v1, v6, v11, v12);
    mix(v2, v7, v8, v13);
    mix(v3, v4, v9, v14);
}

// G (RFC 9106, 3.5): next = P(prev xor ref) xor prev xor ref, xored into what next held where
// `keep` (passes after the first).
void fill(const Block& prev, const Block& ref, Block& next, bool keep) {
    Block r, t;
    for (size_t i = 0; i < kBlockWords; i++) r.v[i] = prev.v[i] ^ ref.v[i];
    t = r;
    if (keep)
        for (size_t i = 0; i < kBlockWords; i++) t.v[i] ^= next.v[i];
    uint64_t* v = r.v;
    for (int i = 0; i < 8; i++) {
        uint64_t* q = v + 16 * i;
        round16(q[0], q[1], q[2], q[3], q[4], q[5], q[6], q[7], q[8], q[9], q[10], q[11], q[12], q[13], q[14], q[15]);
    }
    for (int i = 0; i < 8; i++) {
        uint64_t* q = v + 2 * i;
        round16(q[0], q[1], q[16], q[17], q[32], q[33], q[48], q[49], q[64], q[65], q[80], q[81], q[96], q[97], q[112], q[113]);
    }
    for (size_t i = 0; i < kBlockWords; i++) next.v[i] = t.v[i] ^ r.v[i];
}

}  // namespace

bool argon2id(const uint8_t* password, size_t passwordSize, const uint8_t* salt, size_t saltSize, const uint8_t* secret, size_t secretSize, const uint8_t* ad,
              size_t adSize, uint32_t iterations, uint32_t memoryKiB, uint32_t lanes, uint8_t* tag, size_t tagSize, std::string& why) {
    if (lanes < 1 || lanes > 0xFFFFFF) return why = "Argon2's parallelism is from 1 to 16777215", false;
    if (iterations < 1) return why = "Argon2's iterations are 1 or more", false;
    if (tagSize < 4) return why = "an Argon2 hash is 4 bytes or more", false;
    if (memoryKiB < 8 * lanes) return why = "Argon2's memory is at least 8 KiB for each lane", false;
    if (saltSize < 8) return why = "an Argon2 salt is 8 bytes or more", false;
    const uint32_t blocks = 4 * lanes * (memoryKiB / (4 * lanes));
    const uint32_t laneLength = blocks / lanes;
    const uint32_t segment = laneLength / 4;
    std::vector<Block> memory;
    try {
        memory.resize(blocks);
    } catch (const std::bad_alloc&) {
        return why = "not enough memory for Argon2's " + std::to_string(memoryKiB) + " KiB", false;
    }

    // H0, then each lane's first two blocks.
    std::vector<uint8_t> h0in;
    le32(h0in, lanes);
    le32(h0in, static_cast<uint32_t>(tagSize));
    le32(h0in, memoryKiB);
    le32(h0in, iterations);
    le32(h0in, 0x13);
    le32(h0in, 2);  // Argon2id
    le32(h0in, static_cast<uint32_t>(passwordSize));
    h0in.insert(h0in.end(), password, password + passwordSize);
    le32(h0in, static_cast<uint32_t>(saltSize));
    h0in.insert(h0in.end(), salt, salt + saltSize);
    le32(h0in, static_cast<uint32_t>(secretSize));
    if (secretSize > 0) h0in.insert(h0in.end(), secret, secret + secretSize);
    le32(h0in, static_cast<uint32_t>(adSize));
    if (adSize > 0) h0in.insert(h0in.end(), ad, ad + adSize);
    uint8_t h0[72];
    blake2b(h0, 64, h0in.data(), h0in.size());
    wipe(h0in.data(), h0in.size());
    uint8_t bytes[1024];
    for (uint32_t lane = 0; lane < lanes; lane++) {
        for (uint32_t k = 0; k < 2; k++) {
            store32le(h0 + 64, k);
            store32le(h0 + 68, lane);
            hashLong(bytes, 1024, h0, 72);
            Block& b = memory[size_t(lane) * laneLength + k];
            for (size_t i = 0; i < kBlockWords; i++) b.v[i] = load64le(bytes + 8 * i);
        }
    }
    wipe(h0, sizeof(h0));

    Block zero{}, input{}, address{};
    for (uint32_t pass = 0; pass < iterations; pass++) {
        for (uint32_t slice = 0; slice < 4; slice++) {
            const bool independent = pass == 0 && slice < 2;
            for (uint32_t lane = 0; lane < lanes; lane++) {
                if (independent) {
                    input = Block{};
                    input.v[0] = pass;
                    input.v[1] = lane;
                    input.v[2] = slice;
                    input.v[3] = blocks;
                    input.v[4] = iterations;
                    input.v[5] = 2;
                }
                // The addresses of the data-independent part, 128 to a block.
                auto nextAddresses = [&] {
                    input.v[6]++;
                    fill(zero, input, address, false);
                    fill(zero, address, address, false);
                };
                uint32_t start = 0;
                if (pass == 0 && slice == 0) {
                    start = 2;
                    if (independent) nextAddresses();
                }
                size_t current = size_t(lane) * laneLength + size_t(slice) * segment + start;
                size_t previous = current % laneLength == 0 ? current + laneLength - 1 : current - 1;
                for (uint32_t i = start; i < segment; i++, current++, previous++) {
                    if (current % laneLength == 1) previous = current - 1;
                    uint64_t random;
                    if (independent) {
                        if (i % kBlockWords == 0) nextAddresses();
                        random = address.v[i % kBlockWords];
                    } else {
                        random = memory[previous].v[0];
                    }
                    uint32_t refLane = static_cast<uint32_t>((random >> 32) % lanes);
                    if (pass == 0 && slice == 0) refLane = lane;
                    const bool sameLane = refLane == lane;
                    // The blocks it may reference, and which of them (RFC 9106, 3.4.1.2).
                    uint64_t area;
                    if (pass == 0) {
                        if (slice == 0) area = i - 1;
                        else if (sameLane) area = uint64_t(slice) * segment + i - 1;
                        else area = uint64_t(slice) * segment - (i == 0 ? 1 : 0);
                    } else {
                        if (sameLane) area = laneLength - segment + i - 1;
                        else area = laneLength - segment - (i == 0 ? 1 : 0);
                    }
                    uint64_t relative = random & 0xFFFFFFFFULL;
                    relative = (relative * relative) >> 32;
                    relative = area - 1 - ((area * relative) >> 32);
                    const uint64_t startPosition = pass != 0 && slice != 3 ? uint64_t(slice + 1) * segment : 0;
                    const uint64_t refIndex = (startPosition + relative) % laneLength;
                    fill(memory[previous], memory[size_t(refLane) * laneLength + refIndex], memory[current], pass != 0);
                }
            }
        }
    }

    // The tag: H' of the last blocks of the lanes xored together.
    Block last = memory[laneLength - 1];
    for (uint32_t lane = 1; lane < lanes; lane++)
        for (size_t i = 0; i < kBlockWords; i++) last.v[i] ^= memory[size_t(lane) * laneLength + laneLength - 1].v[i];
    for (size_t i = 0; i < kBlockWords; i++) store64le(bytes + 8 * i, last.v[i]);
    hashLong(tag, tagSize, bytes, 1024);
    wipe(bytes, sizeof(bytes));
    wipe(&last, sizeof(last));
    wipe(memory.data(), memory.size() * sizeof(Block));
    wipe(&address, sizeof(address));
    return true;
}

}  // namespace crypto
}  // namespace ww
