// A timing test of the primitives in src/crypto (networking plan, part 2), in the way of dudect
// (Reparaz, Balasch, Verbauwhede, "Dude, is my code constant time?", 2017): each primitive is
// timed many times on inputs of two classes, a fixed secret and a random one, taken in random
// order, and Welch's t-test asks whether the two classes' times differ. |t| above 4.5 says they
// may (a leak to look for); below it, no difference was found in that many runs. The times are
// cropped at several percentiles too, as dudect does, so that the noise of the machine's other
// work in the slow tail does not hide a difference in the body.
//
// Built against the primitives alone (no VM), with the processor's instructions and without:
//
//   clang++ -O2 -std=c++17 -I src tools/crypto/ct_check.cpp src/crypto/{common,sha2,sha3,mac,argon2,aes_gcm,chacha_poly}.cpp -o ct_check
//   ./ct_check [runs]

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <random>
#include <string>
#include <vector>

#include <x86intrin.h>

#include "crypto/crypto.h"

using namespace ww::crypto;

// The system's secure random numbers, which src/crypto/random.cpp gives ww: here, for RSA's salts and
// primes only, a stand-in.
namespace ww {
bool secureRandomFill(uint8_t* out, size_t size) {
    static std::mt19937_64 made(7);
    for (size_t i = 0; i < size; i++) out[i] = static_cast<uint8_t>(made());
    return true;
}
}  // namespace ww

namespace {

std::mt19937_64 chance(20261120);

void randomBytes(uint8_t* p, size_t n) {
    for (size_t i = 0; i < n; i++) p[i] = static_cast<uint8_t>(chance());
}

struct Welch {
    double n[2] = {0, 0}, mean[2] = {0, 0}, m2[2] = {0, 0};
    void add(int cls, double x) {
        n[cls]++;
        const double d = x - mean[cls];
        mean[cls] += d / n[cls];
        m2[cls] += d * (x - mean[cls]);
    }
    double t() const {
        const double v0 = m2[0] / (n[0] - 1), v1 = m2[1] / (n[1] - 1);
        return (mean[0] - mean[1]) / std::sqrt(v0 / n[0] + v1 / n[1]);
    }
};

// `prepare(cls, i)` makes the i-th input of its class, untimed; `run(i)` is timed.
double measure(const char* name, size_t runs, const std::function<void(int, size_t)>& prepare, const std::function<void(size_t)>& run) {
    std::vector<int> classes(runs);
    std::vector<uint64_t> times(runs);
    for (size_t i = 0; i < runs; i++) {
        classes[i] = static_cast<int>(chance() & 1);
        prepare(classes[i], i);
    }
    for (size_t i = 0; i < runs / 20; i++) run(i);  // warm the caches and the branch predictors
    for (size_t i = 0; i < runs; i++) {
        unsigned aux;
        const uint64_t start = __rdtscp(&aux);
        run(i);
        const uint64_t end = __rdtscp(&aux);
        times[i] = end - start;
    }
    std::vector<uint64_t> sorted = times;
    std::sort(sorted.begin(), sorted.end());
    double worst = 0;
    std::string detail;
    for (double keep : {1.0, 0.9, 0.75, 0.5}) {
        const uint64_t limit = sorted[static_cast<size_t>((runs - 1) * keep)];
        Welch w;
        for (size_t i = 0; i < runs; i++)
            if (times[i] <= limit) w.add(classes[i], static_cast<double>(times[i]));
        const double t = w.t();
        if (std::fabs(t) > std::fabs(worst)) worst = t;
        char part[64];
        std::snprintf(part, sizeof part, " %3.0f%%: %6.2f", keep * 100, t);
        detail += part;
    }
    std::printf("%-44s |t| at most %5.2f  %s  (median %llu cycles)%s\n", name, std::fabs(worst), std::fabs(worst) > 4.5 ? "LOOK" : "ok  ",
                static_cast<unsigned long long>(sorted[runs / 2]), detail.c_str());
    return std::fabs(worst);
}

}  // namespace

int main(int argc, char** argv) {
    const size_t runs = argc > 1 ? std::strtoull(argv[1], nullptr, 10) : 400000;
    std::printf("%zu runs each; the processor has AES-NI %d, PCLMULQDQ %d, SHA %d\n", runs, cpu().aes, cpu().clmul, cpu().sha);
    double worst = 0;
    std::vector<std::vector<uint8_t>> in(runs, std::vector<uint8_t>(64));
    std::vector<std::vector<uint8_t>> keys(runs, std::vector<uint8_t>(32));
    uint8_t out[128], tag[16];
    uint8_t key[32], nonce[12] = {};
    randomBytes(key, 32);
    // Class 0: zeros, the fixed input; class 1: random.
    auto data = [&](int cls, size_t i) {
        if (cls == 0) std::fill(in[i].begin(), in[i].end(), 0);
        else randomBytes(in[i].data(), in[i].size());
    };
    auto secretKey = [&](int cls, size_t i) {
        if (cls == 0) std::fill(keys[i].begin(), keys[i].end(), 0);
        else randomBytes(keys[i].data(), keys[i].size());
    };

    for (int pass = 0; pass < 2; pass++) {
        setPortable(pass == 1);
        const char* way = pass == 1 ? "portable" : "instructions";
        std::printf("-- %s\n", way);
        const Aes aes(key, 32);
        std::unique_ptr<Aead> gcm = newAesGcm(key, 32), chacha = newChaCha20Poly1305(key, 32);
        std::string label;
        auto named = [&](const char* what) {
            label = std::string(what) + " (" + way + ")";
            return label.c_str();
        };
        worst = std::max(worst, measure(named("AES-256 block, the data"), runs, data, [&](size_t i) { aes.encrypt(in[i].data(), out); }));
        worst = std::max(worst, measure(named("AES key schedule, the key"), runs / 4, secretKey, [&](size_t i) { Aes made(keys[i].data(), 32); }));
        worst = std::max(worst, measure(named("AES-256-GCM seal of 64 bytes, the data"), runs / 2, data,
                                        [&](size_t i) { gcm->seal(nonce, 12, nullptr, 0, in[i].data(), 64, out, tag); }));
        worst = std::max(worst, measure(named("SHA-256 of 64 bytes, the data"), runs, data, [&](size_t i) {
            std::unique_ptr<Hash> h = newHash(HashKind::Sha256);
            h->update(in[i].data(), 64);
            h->finish(out);
        }));
        if (pass == 1) continue;  // what follows has no instructions of the processor's
        // Public keys (part 3): the secret scalar or seed fixed or random.
        worst = std::max(worst, measure("X25519, the private key", runs / 8, secretKey, [&](size_t i) { x25519(out, keys[i].data(), key); }));
        worst = std::max(worst, measure("Ed25519 sign, the seed", runs / 16, secretKey, [&](size_t i) { ed25519Sign(out, keys[i].data(), in[0].data(), 32); }));
        uint8_t point[65];
        {
            uint8_t d[32];
            randomBytes(d, 32);
            d[0] = 1;
            p256Public(point, d);
        }
        // A P-256 key below n: the fixed class 1 (not 0, which is no key), the random one top-bit clear.
        auto p256Key = [&](int cls, size_t i) {
            secretKey(cls, i);
            if (cls == 0) keys[i][31] = 1;
            keys[i][0] &= 0x7F;
        };
        worst = std::max(worst, measure("P-256 ECDH, the private key", runs / 32, p256Key, [&](size_t i) { p256Ecdh(out, keys[i].data(), point); }));
        std::unique_ptr<Hash> sha = newHash(HashKind::Sha256);
        worst = std::max(worst, measure("P-256 ECDSA sign, the private key", runs / 32, p256Key,
                                        [&](size_t i) { p256Sign(out, out + 32, keys[i].data(), in[0].data(), 32, *sha); }));
        static RsaKey rsaKey;
        static bool made = false;
        std::string why;
        if (!made) made = rsaGenerate(2048, 65537, rsaKey, why);
        worst = std::max(worst, measure("RSA-2048 sign (PKCS#1, CRT), the message", runs / 400, data, [&](size_t i) {
            std::vector<uint8_t> signature;
            std::string failure;
            rsaSign(rsaKey, HashKind::Sha256, in[i].data(), 64, false, 0, HashKind::Sha256, signature, failure);
        }));
        worst = std::max(worst, measure("ChaCha20-Poly1305 seal of 64 bytes, the key", runs / 4, secretKey, [&](size_t i) {
            std::unique_ptr<Aead> c = newChaCha20Poly1305(keys[i].data(), 32);
            c->seal(nonce, 12, nullptr, 0, in[0].data(), 64, out, tag);
        }));
        worst = std::max(worst, measure("ChaCha20-Poly1305 seal of 64 bytes, the data", runs, data,
                                        [&](size_t i) { chacha->seal(nonce, 12, nullptr, 0, in[i].data(), 64, out, tag); }));
        worst = std::max(worst, measure("SHA-512 of 64 bytes, the data", runs, data, [&](size_t i) {
            std::unique_ptr<Hash> h = newHash(HashKind::Sha512);
            h->update(in[i].data(), 64);
            h->finish(out);
        }));
        worst = std::max(worst, measure("SHA3-256 of 64 bytes, the data", runs, data, [&](size_t i) {
            std::unique_ptr<Hash> h = newHash(HashKind::Sha3_256);
            h->update(in[i].data(), 64);
            h->finish(out);
        }));
        worst = std::max(worst, measure("HMAC-SHA256, the key", runs / 4, secretKey, [&](size_t i) {
            std::unique_ptr<Hash> h = newHash(HashKind::Sha256);
            Hmac m(*h, keys[i].data(), 32);
            m.update(in[0].data(), 16);
            m.finish(out);
        }));
        worst = std::max(worst, measure("BLAKE2b of 64 bytes, the data", runs, data, [&](size_t i) { blake2b(out, 64, in[i].data(), 64); }));
        // Comparing: equal against unequal at the first byte; and a tag wrong at its first byte
        // against one wrong at its last.
        uint8_t reference[32];
        randomBytes(reference, 32);
        auto nearly = [&](int cls, size_t i) {
            std::copy(reference, reference + 32, in[i].begin());
            if (cls == 0) in[i][0] ^= 1;
        };
        worst = std::max(worst, measure("equal(32 bytes), equal or not at byte 0", runs, nearly,
                                        [&](size_t i) { volatile bool same = equal(in[i].data(), reference, 32); (void)same; }));
        uint8_t sealed[64], goodTag[16];
        chacha->seal(nonce, 12, nullptr, 0, reference, 32, sealed, goodTag);
        std::vector<std::vector<uint8_t>> tags(runs, std::vector<uint8_t>(16));
        auto wrongTag = [&](int cls, size_t i) {
            std::copy(goodTag, goodTag + 16, tags[i].begin());
            tags[i][cls == 0 ? 0 : 15] ^= 1;
        };
        worst = std::max(worst, measure("ChaCha20-Poly1305 open, tag wrong at 0 or 15", runs / 2, wrongTag,
                                        [&](size_t i) { chacha->open(nonce, 12, nullptr, 0, sealed, 32, tags[i].data(), out); }));
        gcm->seal(nonce, 12, nullptr, 0, reference, 32, sealed, goodTag);
        worst = std::max(worst, measure("AES-256-GCM open, tag wrong at 0 or 15", runs / 2, wrongTag,
                                        [&](size_t i) { gcm->open(nonce, 12, nullptr, 0, sealed, 32, tags[i].data(), out); }));
    }
    std::printf("largest |t|: %.2f%s\n", worst, worst > 4.5 ? " (look at the lines marked LOOK)" : "");
    // A control: a comparison that stops at the first difference, which this test must find.
    uint8_t reference[32];
    randomBytes(reference, 32);
    auto nearly = [&](int cls, size_t i) {
        std::copy(reference, reference + 32, in[i].begin());
        if (cls == 0) in[i][0] ^= 1;
    };
    measure("control: a comparison that stops early (must LOOK)", runs, nearly, [&](size_t i) {
        volatile bool same = true;
        for (size_t k = 0; k < 32; k++)
            if (in[i][k] != reference[k]) {
                same = false;
                break;
            }
        (void)same;
    });
    return 0;
}
