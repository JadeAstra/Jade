// RSA signatures (RFC 8017): key pairs, and PKCS#1 v1.5 and PSS signatures both ways. Numbers in
// 64-bit limbs, little-endian; arithmetic mod n, p and q in Montgomery form. A private key's
// exponentiation takes the same work for every exponent of its size (a fixed 4-bit window, its
// table read whole for every entry) and every product the same steps; a signature is checked
// with the public key before it is given, so a fault in the work cannot give away the key. Key
// generation (trial division, then Miller-Rabin) may take a time that depends on the primes it
// tries and throws away; it is done once, where nothing watches.

#include "crypto.h"

#include <algorithm>
#include <numeric>
#include <vector>

namespace ww {
bool secureRandomFill(uint8_t* out, size_t size);  // random.cpp
}

namespace ww {
namespace crypto {

namespace {

using u128 = unsigned __int128;
using Limbs = std::vector<uint64_t>;

Limbs fromBytes(const uint8_t* b, size_t size, size_t limbs) {
    Limbs r(limbs, 0);
    for (size_t i = 0; i < size; i++) {
        const size_t bit = 8 * (size - 1 - i);
        if (bit / 64 < limbs) r[bit / 64] |= uint64_t(b[i]) << (bit % 64);
    }
    return r;
}
std::vector<uint8_t> toBytes(const Limbs& a, size_t size) {
    std::vector<uint8_t> b(size, 0);
    for (size_t i = 0; i < size; i++) {
        const size_t bit = 8 * (size - 1 - i);
        if (bit / 64 < a.size()) b[i] = uint8_t(a[bit / 64] >> (bit % 64));
    }
    return b;
}
// The bytes without their leading zeros (a key's numbers, as stored).
std::vector<uint8_t> minimal(const Limbs& a) {
    std::vector<uint8_t> b = toBytes(a, a.size() * 8);
    size_t first = 0;
    while (first + 1 < b.size() && b[first] == 0) first++;
    return std::vector<uint8_t>(b.begin() + static_cast<std::ptrdiff_t>(first), b.end());
}
size_t bitLength(const Limbs& a) {
    for (size_t i = a.size(); i-- > 0;)
        if (a[i] != 0) return 64 * i + 64 - static_cast<size_t>(__builtin_clzll(a[i]));
    return 0;
}
size_t usedLimbs(const Limbs& a) { return (bitLength(a) + 63) / 64; }
Limbs trimmed(const Limbs& a) { return Limbs(a.begin(), a.begin() + static_cast<std::ptrdiff_t>(std::max<size_t>(usedLimbs(a), 1))); }
Limbs sized(Limbs a, size_t limbs) {
    a.resize(limbs, 0);
    return a;
}

// Comparisons and sums of public values (they may branch).
int compare(const Limbs& a, const Limbs& b) {
    const size_t n = std::max(a.size(), b.size());
    for (size_t i = n; i-- > 0;) {
        const uint64_t x = i < a.size() ? a[i] : 0, y = i < b.size() ? b[i] : 0;
        if (x != y) return x < y ? -1 : 1;
    }
    return 0;
}

// a - b, a at least b, as long as a.
Limbs subtract(const Limbs& a, const Limbs& b) {
    Limbs r(a.size());
    uint64_t borrow = 0;
    for (size_t i = 0; i < a.size(); i++) {
        const u128 d = u128(a[i]) - (i < b.size() ? b[i] : 0) - borrow;
        r[i] = uint64_t(d);
        borrow = uint64_t(d >> 64) & 1;
    }
    return r;
}

Limbs addSmall(Limbs a, uint64_t v) {
    u128 c = v;
    for (size_t i = 0; i < a.size() && c != 0; i++) {
        c += a[i];
        a[i] = uint64_t(c);
        c >>= 64;
    }
    if (c != 0) a.push_back(uint64_t(c));
    return a;
}

// a b, schoolbook: the same steps for every value of the same sizes.
Limbs multiply(const Limbs& a, const Limbs& b) {
    Limbs r(a.size() + b.size(), 0);
    for (size_t i = 0; i < a.size(); i++) {
        u128 c = 0;
        for (size_t j = 0; j < b.size(); j++) {
            c += u128(a[i]) * b[j] + r[i + j];
            r[i + j] = uint64_t(c);
            c >>= 64;
        }
        r[i + b.size()] = uint64_t(c);
    }
    return r;
}

// a mod m and a / m for m below 2^32, in 32-bit halves (no 128-bit division, which MSVC's
// libraries lack).
uint64_t modSmall(const Limbs& a, uint64_t m) {
    uint64_t r = 0;
    for (size_t i = a.size(); i-- > 0;) {
        r = ((r << 32) | (a[i] >> 32)) % m;
        r = ((r << 32) | (a[i] & 0xFFFFFFFFu)) % m;
    }
    return r;
}
Limbs divideSmall(const Limbs& a, uint64_t m) {
    Limbs q(a.size());
    uint64_t r = 0;
    for (size_t i = a.size(); i-- > 0;) {
        const uint64_t high = (r << 32) | (a[i] >> 32);
        const uint64_t qh = high / m;
        r = high % m;
        const uint64_t low = (r << 32) | (a[i] & 0xFFFFFFFFu);
        q[i] = (qh << 32) | (low / m);
        r = low % m;
    }
    return q;
}
Limbs multiplySmall(const Limbs& a, uint64_t v) {
    Limbs r(a.size() + 1, 0);
    u128 c = 0;
    for (size_t i = 0; i < a.size(); i++) {
        c += u128(a[i]) * v;
        r[i] = uint64_t(c);
        c >>= 64;
    }
    r[a.size()] = uint64_t(c);
    return r;
}

// v^-1 mod m for small numbers (extended Euclid); 0 where there is none.
uint64_t inverseSmall(uint64_t v, uint64_t m) {
    int64_t t = 0, newT = 1;
    int64_t r = static_cast<int64_t>(m), newR = static_cast<int64_t>(v % m);
    while (newR != 0) {
        const int64_t q = r / newR;
        std::swap(t, newT);
        newT -= q * t;
        std::swap(r, newR);
        newR -= q * r;
    }
    if (r != 1) return 0;
    return static_cast<uint64_t>(t < 0 ? t + static_cast<int64_t>(m) : t);
}

// ---- Montgomery arithmetic mod an odd m of k limbs ----

struct Mont {
    Limbs m;
    size_t k = 0;
    uint64_t m0inv = 0;
    Limbs one, r2;  // R mod m, R^2 mod m; R = 2^(64k)
};

// (a + b) mod m, both below m, with no branch.
Limbs modAdd(const Limbs& a, const Limbs& b, const Mont& md) {
    Limbs s(md.k), t(md.k);
    u128 c = 0;
    for (size_t i = 0; i < md.k; i++) {
        c += u128(a[i]) + b[i];
        s[i] = uint64_t(c);
        c >>= 64;
    }
    const uint64_t carryOut = uint64_t(c);
    uint64_t borrow = 0;
    for (size_t i = 0; i < md.k; i++) {
        const u128 d = u128(s[i]) - md.m[i] - borrow;
        t[i] = uint64_t(d);
        borrow = uint64_t(d >> 64) & 1;
    }
    const uint64_t takeT = 0 - (carryOut | (borrow ^ 1));
    for (size_t i = 0; i < md.k; i++) s[i] = (t[i] & takeT) | (s[i] & ~takeT);
    return s;
}

Limbs modSub(const Limbs& a, const Limbs& b, const Mont& md) {
    Limbs d(md.k);
    uint64_t borrow = 0;
    for (size_t i = 0; i < md.k; i++) {
        const u128 x = u128(a[i]) - b[i] - borrow;
        d[i] = uint64_t(x);
        borrow = uint64_t(x >> 64) & 1;
    }
    const uint64_t mask = 0 - borrow;
    u128 c = 0;
    for (size_t i = 0; i < md.k; i++) {
        c += u128(d[i]) + (md.m[i] & mask);
        d[i] = uint64_t(c);
        c >>= 64;
    }
    return d;
}

// The k limbs of t (k + 1 of them, below 2m) less m where they are m or more, by mask.
Limbs finalSubtract(const uint64_t* t, uint64_t top, const Mont& md) {
    Limbs r(t, t + md.k), s(md.k);
    uint64_t borrow = 0;
    for (size_t i = 0; i < md.k; i++) {
        const u128 x = u128(r[i]) - md.m[i] - borrow;
        s[i] = uint64_t(x);
        borrow = uint64_t(x >> 64) & 1;
    }
    const uint64_t takeS = 0 - (top | (borrow ^ 1));
    for (size_t i = 0; i < md.k; i++) r[i] = (s[i] & takeS) | (r[i] & ~takeS);
    return r;
}

Limbs montMul(const Limbs& a, const Limbs& b, const Mont& md) {
    const size_t k = md.k;
    std::vector<uint64_t> t(k + 2, 0);
    for (size_t i = 0; i < k; i++) {
        u128 c = 0;
        for (size_t j = 0; j < k; j++) {
            c += u128(a[j]) * b[i] + t[j];
            t[j] = uint64_t(c);
            c >>= 64;
        }
        c += t[k];
        t[k] = uint64_t(c);
        t[k + 1] = uint64_t(c >> 64);
        const uint64_t q = t[0] * md.m0inv;
        c = u128(q) * md.m[0] + t[0];
        c >>= 64;
        for (size_t j = 1; j < k; j++) {
            c += u128(q) * md.m[j] + t[j];
            t[j - 1] = uint64_t(c);
            c >>= 64;
        }
        c += t[k];
        t[k - 1] = uint64_t(c);
        t[k] = t[k + 1] + uint64_t(c >> 64);
    }
    return finalSubtract(t.data(), t[k], md);
}

// x R^-1 mod m for x of up to 2k limbs, below m R.
Limbs montReduce(Limbs x, const Mont& md) {
    const size_t k = md.k;
    x.resize(2 * k + 1, 0);
    for (size_t i = 0; i < k; i++) {
        const uint64_t q = x[i] * md.m0inv;
        u128 c = 0;
        for (size_t j = 0; j < k; j++) {
            c += u128(q) * md.m[j] + x[i + j];
            x[i + j] = uint64_t(c);
            c >>= 64;
        }
        for (size_t j = i + k; j < 2 * k + 1; j++) {  // to the end, the same steps whatever is carried
            c += x[j];
            x[j] = uint64_t(c);
            c >>= 64;
        }
    }
    return finalSubtract(x.data() + k, x[2 * k], md);
}

Mont makeMont(const Limbs& modulus) {
    Mont md;
    md.m = trimmed(modulus);
    md.k = md.m.size();
    uint64_t inv = 1;
    for (int i = 0; i < 6; i++) inv *= 2 - md.m[0] * inv;
    md.m0inv = 0 - inv;
    // R mod m: from 2^(bits - 1), below m (odd, so no power of two), doubled up to 2^(64k).
    const size_t bits = bitLength(md.m);
    Limbs x(md.k, 0);
    x[(bits - 1) / 64] = uint64_t(1) << ((bits - 1) % 64);
    for (size_t i = bits - 1; i < 64 * md.k; i++) x = modAdd(x, x, md);
    md.one = x;
    // R^2 mod m = 2^(64k) R: the Montgomery product of 2^a R and 2^b R is 2^(a+b) R, so 64k is built
    // bit by bit from R and 2R, a squaring a bit (thousands of doublings before).
    const Limbs twoR = modAdd(md.one, md.one, md);
    const uint64_t e = 64 * md.k;
    Limbs r = md.one;
    for (int bit = 63 - __builtin_clzll(e); bit >= 0; bit--) {
        r = montMul(r, r, md);
        if ((e >> bit) & 1) r = montMul(r, twoR, md);
    }
    md.r2 = r;
    return md;
}

Limbs toMont(const Limbs& a, const Mont& md) { return montMul(sized(a, md.k), md.r2, md); }
Limbs fromMont(const Limbs& a, const Mont& md) {
    Limbs one(md.k, 0);
    one[0] = 1;
    return montMul(a, one, md);
}

// base^exponent, both in their forms (base Montgomery, exponent plain): a fixed 4-bit window over
// all the exponent's limbs, each table entry read for every window.
Limbs modPow(const Limbs& base, const Limbs& exponent, const Mont& md) {
    const size_t k = md.k;
    std::vector<Limbs> table(16);
    table[0] = md.one;
    for (int i = 1; i < 16; i++) table[size_t(i)] = montMul(table[size_t(i - 1)], base, md);
    Limbs r = md.one;
    Limbs chosen(k);
    for (size_t bit = 64 * exponent.size(); bit >= 4;) {
        bit -= 4;
        for (int s = 0; s < 4; s++) r = montMul(r, r, md);
        const uint64_t window = (exponent[bit / 64] >> (bit % 64)) & 15;
        for (size_t j = 0; j < k; j++) chosen[j] = 0;
        for (uint64_t e = 0; e < 16; e++) {
            const uint64_t mask = 0 - uint64_t(((e ^ window) - 1) >> 63);  // all ones where e == window
            for (size_t j = 0; j < k; j++) chosen[j] |= table[e][j] & mask;
        }
        r = montMul(r, chosen, md);
    }
    return r;
}

// base^exponent for a public exponent: square and multiply over its bits, which may be seen.
Limbs modPowPublic(const Limbs& base, const Limbs& exponent, const Mont& md) {
    Limbs r = md.one;
    for (size_t bit = bitLength(exponent); bit-- > 0;) {
        r = montMul(r, r, md);
        if ((exponent[bit / 64] >> (bit % 64)) & 1) r = montMul(r, base, md);
    }
    return r;
}

// a mod m (plain), a of up to 2k limbs: x R^-1, then times R^2, then R^-1 again.
Limbs reduceInto(const Limbs& a, const Mont& md) { return montMul(montReduce(a, md), md.r2, md); }

// ---- Primes ----

const std::vector<uint32_t>& smallPrimes() {
    static const std::vector<uint32_t> made = [] {
        std::vector<uint32_t> primes;
        std::vector<bool> composite(4096, false);
        for (uint32_t i = 3; i < 4096; i += 2) {
            if (composite[i]) continue;
            primes.push_back(i);
            for (uint32_t j = i * i; j < 4096; j += 2 * i) composite[j] = true;
        }
        return primes;
    }();
    return made;
}

bool randomLimbs(Limbs& out) { return secureRandomFill(reinterpret_cast<uint8_t*>(out.data()), out.size() * 8); }

// Miller-Rabin with `rounds` random bases.
bool probablyPrime(const Limbs& n, int rounds) {
    const Mont md = makeMont(n);
    Limbs nm1 = subtract(md.m, Limbs{1});
    size_t s = 0;
    while (((nm1[s / 64] >> (s % 64)) & 1) == 0) s++;
    // d = (n - 1) >> s
    Limbs d(md.k, 0);
    for (size_t i = 0; i < 64 * md.k; i++) {
        const size_t from = i + s;
        if (from < 64 * md.k && ((nm1[from / 64] >> (from % 64)) & 1)) d[i / 64] |= uint64_t(1) << (i % 64);
    }
    const Limbs minusOne = toMont(nm1, md);
    for (int round = 0; round < rounds; round++) {
        Limbs a(md.k);
        do {
            if (!randomLimbs(a)) return false;
            a[md.k - 1] &= (md.m[md.k - 1] >> 1);  // below n
        } while (compare(a, Limbs{2}) < 0);
        Limbs x = modPow(toMont(a, md), d, md);
        if (compare(x, md.one) == 0 || compare(x, minusOne) == 0) continue;
        bool witness = true;
        for (size_t i = 1; i < s && witness; i++) {
            x = montMul(x, x, md);
            if (compare(x, minusOne) == 0) witness = false;
        }
        if (witness) return false;
    }
    return true;
}

// A prime of `bits` bits, its top two bits set (so that p q has all its bits), with
// gcd(e, p - 1) = 1.
bool generatePrime(size_t bits, uint64_t e, Limbs& p) {
    const size_t limbs = (bits + 63) / 64;
    const auto& primes = smallPrimes();
    for (int attempt = 0; attempt < 100000; attempt++) {
        p.assign(limbs, 0);
        if (!randomLimbs(p)) return false;
        const size_t topBits = bits - 64 * (limbs - 1);
        if (topBits < 64) p[limbs - 1] &= (uint64_t(1) << topBits) - 1;
        p[(bits - 1) / 64] |= uint64_t(1) << ((bits - 1) % 64);
        p[(bits - 2) / 64] |= uint64_t(1) << ((bits - 2) % 64);
        p[0] |= 1;
        bool divisible = false;
        for (uint32_t q : primes) {
            if (modSmall(p, q) == 0) {
                divisible = true;
                break;
            }
        }
        if (divisible) continue;
        if (std::gcd(e, (modSmall(p, e) + e - 1) % e) != 1 && e != 1) continue;
        if (probablyPrime(p, 8)) return true;
    }
    return false;
}

// ---- Padding ----

// The DER DigestInfo before a digest of each hash (RFC 8017, 9.2, note 1).
bool digestPrefix(HashKind kind, std::vector<uint8_t>& prefix) {
    uint8_t last = 0;
    switch (kind) {
        case HashKind::Sha256: last = 1; break;
        case HashKind::Sha384: last = 2; break;
        case HashKind::Sha512: last = 3; break;
        case HashKind::Sha224: last = 4; break;
        case HashKind::Sha512_256: last = 6; break;
        case HashKind::Sha3_224: last = 7; break;
        case HashKind::Sha3_256: last = 8; break;
        case HashKind::Sha3_384: last = 9; break;
        case HashKind::Sha3_512: last = 10; break;
        default: return false;
    }
    const uint8_t size = uint8_t(newHash(kind)->size());
    prefix = {0x30, uint8_t(size + 17), 0x30, 0x0d, 0x06, 0x09, 0x60, 0x86, 0x48, 0x01, 0x65, 0x03, 0x04, 0x02, last, 0x05, 0x00, 0x04, size};
    return true;
}

std::vector<uint8_t> digestOf(HashKind kind, const uint8_t* data, size_t size) {
    std::unique_ptr<Hash> h = newHash(kind);
    h->update(data, size);
    std::vector<uint8_t> out(h->size());
    h->finish(out.data());
    return out;
}

// EMSA-PKCS1-v1_5: 00 01 FF... 00 DigestInfo; false where emLen is too small.
bool pkcs1Encode(HashKind kind, const uint8_t* message, size_t size, size_t emLen, std::vector<uint8_t>& em) {
    std::vector<uint8_t> t;
    if (!digestPrefix(kind, t)) return false;
    const std::vector<uint8_t> digest = digestOf(kind, message, size);
    t.insert(t.end(), digest.begin(), digest.end());
    if (emLen < t.size() + 11) return false;
    em.assign(emLen, 0xFF);
    em[0] = 0;
    em[1] = 1;
    em[emLen - t.size() - 1] = 0;
    std::copy(t.begin(), t.end(), em.end() - static_cast<std::ptrdiff_t>(t.size()));
    return true;
}

std::vector<uint8_t> mgf1(HashKind kind, const uint8_t* seed, size_t seedSize, size_t size) {
    std::vector<uint8_t> out;
    uint8_t counter[4];
    for (uint32_t i = 0; out.size() < size; i++) {
        std::unique_ptr<Hash> h = newHash(kind);
        h->update(seed, seedSize);
        store32be(counter, i);
        h->update(counter, 4);
        std::vector<uint8_t> block(h->size());
        h->finish(block.data());
        out.insert(out.end(), block.begin(), block.end());
    }
    out.resize(size);
    return out;
}

// EMSA-PSS-ENCODE (RFC 8017, 9.1.1), emBits = modBits - 1.
bool pssEncode(HashKind kind, HashKind mgfKind, const uint8_t* message, size_t size, size_t saltLen, size_t emBits, std::vector<uint8_t>& em) {
    const std::vector<uint8_t> mHash = digestOf(kind, message, size);
    const size_t hLen = mHash.size();
    const size_t emLen = (emBits + 7) / 8;
    if (emLen < hLen + saltLen + 2) return false;
    std::vector<uint8_t> salt(saltLen);
    if (saltLen > 0 && !secureRandomFill(salt.data(), saltLen)) return false;
    std::vector<uint8_t> mPrime(8, 0);
    mPrime.insert(mPrime.end(), mHash.begin(), mHash.end());
    mPrime.insert(mPrime.end(), salt.begin(), salt.end());
    const std::vector<uint8_t> h = digestOf(kind, mPrime.data(), mPrime.size());
    std::vector<uint8_t> db(emLen - hLen - 1, 0);
    db[emLen - saltLen - hLen - 2] = 1;
    std::copy(salt.begin(), salt.end(), db.end() - static_cast<std::ptrdiff_t>(saltLen));
    const std::vector<uint8_t> mask = mgf1(mgfKind, h.data(), hLen, db.size());
    for (size_t i = 0; i < db.size(); i++) db[i] ^= mask[i];
    db[0] &= uint8_t(0xFF >> (8 * emLen - emBits));
    em = db;
    em.insert(em.end(), h.begin(), h.end());
    em.push_back(0xBC);
    return true;
}

// EMSA-PSS-VERIFY (RFC 8017, 9.1.2).
bool pssVerify(HashKind kind, HashKind mgfKind, const uint8_t* message, size_t size, size_t saltLen, size_t emBits, const std::vector<uint8_t>& em) {
    const std::vector<uint8_t> mHash = digestOf(kind, message, size);
    const size_t hLen = mHash.size();
    const size_t emLen = (emBits + 7) / 8;
    if (em.size() != emLen || emLen < hLen + saltLen + 2) return false;
    if (em[emLen - 1] != 0xBC) return false;
    std::vector<uint8_t> db(em.begin(), em.begin() + static_cast<std::ptrdiff_t>(emLen - hLen - 1));
    const uint8_t* h = em.data() + emLen - hLen - 1;
    const uint8_t topMask = uint8_t(0xFF << (8 - (8 * emLen - emBits)));
    if ((8 * emLen - emBits) > 0 && (db[0] & topMask) != 0) return false;
    const std::vector<uint8_t> mask = mgf1(mgfKind, h, hLen, db.size());
    for (size_t i = 0; i < db.size(); i++) db[i] ^= mask[i];
    db[0] &= uint8_t(0xFF >> (8 * emLen - emBits));
    const size_t zeros = emLen - hLen - saltLen - 2;
    for (size_t i = 0; i < zeros; i++)
        if (db[i] != 0) return false;
    if (db[zeros] != 1) return false;
    std::vector<uint8_t> mPrime(8, 0);
    mPrime.insert(mPrime.end(), mHash.begin(), mHash.end());
    mPrime.insert(mPrime.end(), db.end() - static_cast<std::ptrdiff_t>(saltLen), db.end());
    const std::vector<uint8_t> check = digestOf(kind, mPrime.data(), mPrime.size());
    return std::equal(check.begin(), check.end(), h);
}

}  // namespace

// ---- What the rest of ww calls ----

// s^e mod n for a public key, n and e big-endian; false where s is not below n.
bool rsaPublic(const std::vector<uint8_t>& n, const std::vector<uint8_t>& e, const std::vector<uint8_t>& s, std::vector<uint8_t>& m) {
    const size_t k = (n.size() + 7) / 8;
    const Limbs nl = trimmed(fromBytes(n.data(), n.size(), k));
    if (nl.size() == 1 && nl[0] == 0) return false;
    if ((nl[0] & 1) == 0) return false;
    const Limbs sl = fromBytes(s.data(), s.size(), std::max(k, (s.size() + 7) / 8));
    if (compare(sl, nl) >= 0) return false;
    const Mont md = makeMont(nl);
    const Limbs el = fromBytes(e.data(), e.size(), (e.size() + 7) / 8 + 1);
    const size_t modBytes = (bitLength(nl) + 7) / 8;
    m = toBytes(fromMont(modPowPublic(toMont(sized(sl, md.k), md), el, md), md), modBytes);
    return true;
}

bool rsaVerify(const std::vector<uint8_t>& n, const std::vector<uint8_t>& e, HashKind kind, const uint8_t* message, size_t size,
               const std::vector<uint8_t>& signature, bool pss, size_t saltLen, HashKind mgfKind) {
    size_t first = 0;
    while (first < n.size() && n[first] == 0) first++;
    const std::vector<uint8_t> modulus(n.begin() + static_cast<std::ptrdiff_t>(first), n.end());
    if (modulus.empty()) return false;
    const size_t modBits = bitLength(fromBytes(modulus.data(), modulus.size(), (modulus.size() + 7) / 8));
    const size_t k = (modBits + 7) / 8;
    if (signature.size() != k) return false;
    std::vector<uint8_t> em;
    if (!rsaPublic(modulus, e, signature, em)) return false;
    if (!pss) {
        std::vector<uint8_t> expected;
        if (!pkcs1Encode(kind, message, size, k, expected)) return false;
        return em == expected;
    }
    // emLen is one byte less where modBits - 1 is a whole number of bytes: its first byte must be 0.
    const size_t emBits = modBits - 1;
    if ((emBits + 7) / 8 < k) {
        if (em[0] != 0) return false;
        em.erase(em.begin());
    }
    return pssVerify(kind, mgfKind, message, size, saltLen, emBits, em);
}

bool rsaGenerate(size_t bits, uint64_t e, RsaKey& key, std::string& why) {
    if (bits < 1024 || bits > 16384 || bits % 2 != 0) return why = "an RSA key is 1024 to 16384 bits, an even number of them", false;
    if (e < 3 || e % 2 == 0 || e >= (uint64_t(1) << 32)) return why = "an RSA public exponent is odd, from 3 to 2^32 - 1", false;
    for (int attempt = 0; attempt < 100; attempt++) {
        Limbs p, q;
        if (!generatePrime(bits / 2, e, p) || !generatePrime(bits / 2, e, q)) return why = "no prime found", false;
        if (compare(p, q) == 0) continue;
        if (compare(p, q) < 0) std::swap(p, q);  // p > q, as qInv is q^-1 mod p
        // |p - q| above 2^(bits/2 - 100) (FIPS 186-5, A.1.3).
        if (bitLength(subtract(p, q)) <= bits / 2 - 100) continue;
        const Limbs n = trimmed(multiply(p, q));
        if (bitLength(n) != bits) continue;
        const Limbs pm1 = subtract(p, Limbs{1}), qm1 = subtract(q, Limbs{1});
        // e^-1 mod m for a small e: (1 + t m) / e, where t = -m^-1 mod e.
        auto inverseOfE = [e](const Limbs& m, Limbs& out) {
            const uint64_t mInv = inverseSmall(modSmall(m, e), e);
            if (mInv == 0) return false;
            const uint64_t t = (e - mInv) % e;
            out = trimmed(divideSmall(addSmall(multiplySmall(m, t), 1), e));
            return true;
        };
        const Limbs phi = trimmed(multiply(pm1, qm1));
        Limbs d, dp, dq;
        if (!inverseOfE(phi, d) || !inverseOfE(pm1, dp) || !inverseOfE(qm1, dq)) continue;
        // qInv = q^(p - 2) mod p.
        const Mont mp = makeMont(p);
        const Limbs qinv = fromMont(modPow(toMont(sized(q, mp.k), mp), subtract(p, Limbs{2}), mp), mp);  // q is below p
        key.n = minimal(n);
        key.e = minimal(Limbs{e});
        key.d = minimal(d);
        key.p = minimal(p);
        key.q = minimal(q);
        key.dp = minimal(dp);
        key.dq = minimal(dq);
        key.qinv = minimal(qinv);
        return true;
    }
    return why = "no key found", false;
}

// A signature with the private key (CRT where it has p and q), checked with the public key before
// it is given.
bool rsaSign(const RsaKey& key, HashKind kind, const uint8_t* message, size_t size, bool pss, size_t saltLen, HashKind mgfKind, std::vector<uint8_t>& signature,
             std::string& why) {
    const size_t kn = (key.n.size() + 7) / 8 + 1;
    const Limbs n = trimmed(fromBytes(key.n.data(), key.n.size(), kn));
    const size_t modBits = bitLength(n);
    const size_t k = (modBits + 7) / 8;
    std::vector<uint8_t> em;
    if (!pss) {
        if (!pkcs1Encode(kind, message, size, k, em)) return why = "the key is too small for this hash's signature", false;
    } else {
        if (!pssEncode(kind, mgfKind, message, size, saltLen, modBits - 1, em)) return why = "the key is too small for this hash and salt", false;
        if (em.size() < k) em.insert(em.begin(), 0);
    }
    const Mont mn = makeMont(n);
    const Limbs m = fromBytes(em.data(), em.size(), mn.k);
    Limbs s;
    if (!key.p.empty() && !key.q.empty() && !key.dp.empty() && !key.dq.empty() && !key.qinv.empty()) {
        const Limbs p = trimmed(fromBytes(key.p.data(), key.p.size(), (key.p.size() + 7) / 8));
        const Limbs q = trimmed(fromBytes(key.q.data(), key.q.size(), (key.q.size() + 7) / 8));
        const Mont mp = makeMont(p), mq = makeMont(q);
        const Limbs dp = fromBytes(key.dp.data(), key.dp.size(), (key.dp.size() + 7) / 8);
        const Limbs dq = fromBytes(key.dq.data(), key.dq.size(), (key.dq.size() + 7) / 8);
        const Limbs qinv = fromBytes(key.qinv.data(), key.qinv.size(), mp.k);
        auto into = [](const Limbs& x, const Mont& md) { return reduceInto(sized(x, 2 * md.k), md); };
        if (m.size() > 2 * mp.k || m.size() > 2 * mq.k) return why = "the key's primes do not fit its modulus", false;
        const Limbs s1 = fromMont(modPow(toMont(into(m, mp), mp), dp, mp), mp);
        const Limbs s2 = fromMont(modPow(toMont(into(m, mq), mq), dq, mq), mq);
        // h = qInv (s1 - s2) mod p; s = s2 + h q.
        const Limbs s2p = into(s2, mp);
        const Limbs h = fromMont(montMul(toMont(modSub(s1, s2p, mp), mp), toMont(sized(qinv, mp.k), mp), mp), mp);
        Limbs sum = multiply(h, q);
        u128 c = 0;
        for (size_t i = 0; i < sum.size(); i++) {
            c += u128(sum[i]) + (i < s2.size() ? s2[i] : 0);
            sum[i] = uint64_t(c);
            c >>= 64;
        }
        s = sized(sum, mn.k);
    } else {
        const Limbs d = fromBytes(key.d.data(), key.d.size(), (key.d.size() + 7) / 8);
        s = fromMont(modPow(toMont(m, mn), d, mn), mn);
    }
    signature = toBytes(s, k);
    // Checked before it is given.
    std::vector<uint8_t> back;
    if (!rsaPublic(key.n, key.e, signature, back) || toBytes(fromBytes(back.data(), back.size(), mn.k), k) != toBytes(m, k))
        return why = "the private key does not match its public key", false;
    return true;
}

}  // namespace crypto
}  // namespace ww
