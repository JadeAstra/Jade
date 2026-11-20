// SHA-3 and SHAKE (FIPS 202) on Keccak-f[1600]: a sponge of 25 words, absorbing at the rate
// (200 less twice the security in bytes) and squeezing the same. Constant-time as written.

#include "crypto.h"

namespace ww {
namespace crypto {

namespace {

const uint64_t roundConstants[24] = {
    0x0000000000000001ULL, 0x0000000000008082ULL, 0x800000000000808aULL, 0x8000000080008000ULL, 0x000000000000808bULL, 0x0000000080000001ULL,
    0x8000000080008081ULL, 0x8000000000008009ULL, 0x000000000000008aULL, 0x0000000000000088ULL, 0x0000000080008009ULL, 0x000000008000000aULL,
    0x000000008000808bULL, 0x800000000000008bULL, 0x8000000000008089ULL, 0x8000000000008003ULL, 0x8000000000008002ULL, 0x8000000000000080ULL,
    0x000000000000800aULL, 0x800000008000000aULL, 0x8000000080008081ULL, 0x8000000000008080ULL, 0x0000000080000001ULL, 0x8000000080008008ULL};

// The rotation of each lane, and where pi moves it, in the order rho and pi walk them.
const int rotations[24] = {1, 3, 6, 10, 15, 21, 28, 36, 45, 55, 2, 14, 27, 41, 56, 8, 25, 43, 62, 18, 39, 61, 20, 44};
const int lanes[24] = {10, 7, 11, 17, 18, 3, 5, 16, 8, 21, 24, 4, 15, 23, 19, 13, 12, 2, 20, 14, 22, 9, 6, 1};

void keccakF(uint64_t a[25]) {
    uint64_t c[5];
    for (int round = 0; round < 24; round++) {
        // Theta.
        for (int x = 0; x < 5; x++) c[x] = a[x] ^ a[x + 5] ^ a[x + 10] ^ a[x + 15] ^ a[x + 20];
        for (int x = 0; x < 5; x++) {
            const uint64_t d = c[(x + 4) % 5] ^ rotl64(c[(x + 1) % 5], 1);
            for (int y = 0; y < 25; y += 5) a[y + x] ^= d;
        }
        // Rho and pi.
        uint64_t moving = a[1];
        for (int i = 0; i < 24; i++) {
            const int j = lanes[i];
            const uint64_t next = a[j];
            a[j] = rotl64(moving, rotations[i]);
            moving = next;
        }
        // Chi.
        for (int y = 0; y < 25; y += 5) {
            for (int x = 0; x < 5; x++) c[x] = a[y + x];
            for (int x = 0; x < 5; x++) a[y + x] = c[x] ^ (~c[(x + 1) % 5] & c[(x + 2) % 5]);
        }
        // Iota.
        a[0] ^= roundConstants[round];
    }
}

class Sha3 final : public Hash {
public:
    Sha3(HashKind kind, size_t shakeSize) : kind_(kind) {
        switch (kind) {
            case HashKind::Sha3_224: rate_ = 144, size_ = 28; break;
            case HashKind::Sha3_256: rate_ = 136, size_ = 32; break;
            case HashKind::Sha3_384: rate_ = 104, size_ = 48; break;
            case HashKind::Shake128: rate_ = 168, size_ = shakeSize; break;
            case HashKind::Shake256: rate_ = 136, size_ = shakeSize; break;
            default: rate_ = 72, size_ = 64; break;  // SHA3-512
        }
        reset();
    }
    ~Sha3() override { wipe(state_, sizeof(state_)); }

    void update(const uint8_t* data, size_t size) override {
        if (squeezing_) reset();  // a SHAKE read from, given more: a new message
        while (size > 0) {
            const size_t now = size < rate_ - at_ ? size : rate_ - at_;
            for (size_t i = 0; i < now; i++) state_[(at_ + i) / 8] ^= uint64_t(data[i]) << (8 * ((at_ + i) % 8));
            at_ += now;
            data += now;
            size -= now;
            if (at_ == rate_) {
                keccakF(state_);
                at_ = 0;
            }
        }
    }
    void finish(uint8_t* out) override {
        squeeze(out, size_);
        reset();
    }
    bool squeeze(uint8_t* out, size_t n) override {
        if (!squeezing_) {
            // The domain bits: 01 for SHA-3, 1111 for SHAKE, then the pad's first 1; its last at
            // the end of the rate.
            const uint64_t domain = kind_ == HashKind::Shake128 || kind_ == HashKind::Shake256 ? 0x1F : 0x06;
            state_[at_ / 8] ^= domain << (8 * (at_ % 8));
            state_[(rate_ - 1) / 8] ^= 0x80ULL << (8 * ((rate_ - 1) % 8));
            keccakF(state_);
            squeezing_ = true;
            at_ = 0;
        }
        while (n > 0) {
            if (at_ == rate_) {
                keccakF(state_);
                at_ = 0;
            }
            const size_t now = n < rate_ - at_ ? n : rate_ - at_;
            for (size_t i = 0; i < now; i++) out[i] = uint8_t(state_[(at_ + i) / 8] >> (8 * ((at_ + i) % 8)));
            at_ += now;
            out += now;
            n -= now;
        }
        return true;
    }
    void reset() override {
        std::memset(state_, 0, sizeof(state_));
        at_ = 0;
        squeezing_ = false;
    }
    std::unique_ptr<Hash> clone() const override { return std::make_unique<Sha3>(*this); }
    size_t size() const override { return size_; }
    size_t blockSize() const override { return rate_; }
    HashKind kind() const override { return kind_; }

private:
    HashKind kind_;
    size_t rate_ = 0, size_ = 0;
    uint64_t state_[25];
    size_t at_ = 0;  // bytes of the rate taken in (or given out, squeezing)
    bool squeezing_ = false;
};

}  // namespace

std::unique_ptr<Hash> newSha3(HashKind kind, size_t shakeSize) { return std::make_unique<Sha3>(kind, shakeSize); }

}  // namespace crypto
}  // namespace ww
