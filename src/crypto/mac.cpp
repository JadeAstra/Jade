// HMAC (RFC 2104) and PBKDF2 (RFC 8018) over any of the hashes.

#include "crypto.h"

#include <vector>

namespace ww {
namespace crypto {

Hmac::Hmac(const Hash& prototype, const uint8_t* key, size_t keySize) {
    const size_t block = prototype.blockSize();
    std::vector<uint8_t> padded(block, 0);
    if (keySize > block) {
        // A key longer than a block is hashed first; SHAKE has no HMAC (its size is chosen).
        std::unique_ptr<Hash> h = prototype.clone();
        h->reset();
        h->update(key, keySize);
        std::vector<uint8_t> digest(h->size());
        h->finish(digest.data());
        std::memcpy(padded.data(), digest.data(), digest.size());
        wipe(digest.data(), digest.size());
    } else if (keySize > 0) {
        std::memcpy(padded.data(), key, keySize);
    }
    std::vector<uint8_t> pad(block);
    inner0_ = prototype.clone();
    inner0_->reset();
    for (size_t i = 0; i < block; i++) pad[i] = padded[i] ^ 0x36;
    inner0_->update(pad.data(), block);
    outer0_ = prototype.clone();
    outer0_->reset();
    for (size_t i = 0; i < block; i++) pad[i] = padded[i] ^ 0x5c;
    outer0_->update(pad.data(), block);
    wipe(pad.data(), block);
    wipe(padded.data(), block);
    inner_ = inner0_->clone();
}

void Hmac::finish(uint8_t* out) {
    uint8_t digest[64];
    const size_t innerSize = inner_->size();
    inner_->finish(digest);
    std::unique_ptr<Hash> outer = outer0_->clone();
    outer->update(digest, innerSize);
    outer->finish(out);
    wipe(digest, sizeof(digest));
    inner_ = inner0_->clone();
}

void Hmac::reset() { inner_ = inner0_->clone(); }

std::unique_ptr<Hmac> Hmac::clone() const {
    std::unique_ptr<Hmac> made(new Hmac());
    made->inner0_ = inner0_->clone();
    made->outer0_ = outer0_->clone();
    made->inner_ = inner_->clone();
    return made;
}

void pbkdf2(const Hash& prototype, const uint8_t* password, size_t passwordSize, const uint8_t* salt, size_t saltSize, uint64_t iterations, uint8_t* out,
            size_t size) {
    Hmac keyed(prototype, password, passwordSize);
    const size_t h = keyed.size();
    uint8_t u[64], t[64], index[4];
    for (uint32_t block = 1; size > 0; block++) {
        // T_i = U_1 xor U_2 xor ... U_c, U_1 = HMAC(salt || i), U_j = HMAC(U_j-1).
        std::unique_ptr<Hmac> mac = keyed.clone();
        mac->update(salt, saltSize);
        store32be(index, block);
        mac->update(index, 4);
        mac->finish(u);
        std::memcpy(t, u, h);
        for (uint64_t j = 1; j < iterations; j++) {
            mac->update(u, h);
            mac->finish(u);
            for (size_t k = 0; k < h; k++) t[k] ^= u[k];
        }
        const size_t now = size < h ? size : h;
        std::memcpy(out, t, now);
        out += now;
        size -= now;
    }
    wipe(u, sizeof(u));
    wipe(t, sizeof(t));
}

}  // namespace crypto
}  // namespace ww
