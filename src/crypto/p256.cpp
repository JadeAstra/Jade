// P-256 (secp256r1, FIPS 186-5 and SEC 2): ECDH and ECDSA. Numbers mod p and mod n in Montgomery
// form, four 64-bit limbs; points in projective coordinates with the complete addition formulas
// of Renes, Costello and Batina (2016, algorithm 4, a = -3), which have no special cases to
// branch on, so adding a point to itself or to infinity is the same work. A scalar times a point
// is a double and an add for every bit, the sum taken or not by mask; the nonce of a signature is
// RFC 6979's, made from the key and the message, so no random number is needed to sign.

#include "crypto.h"

#include <vector>

namespace ww {
namespace crypto {

namespace {

using u128 = unsigned __int128;

struct N {
    uint64_t v[4];  // little-endian limbs
};

// A modulus and what Montgomery multiplication needs of it.
struct Modulus {
    N m;
    uint64_t m0inv;  // -m^-1 mod 2^64
    N r2;            // R^2 mod m, R = 2^256
    N one;           // R mod m: 1 in Montgomery form
};

// a + b - (m where the sum is m or more), with no branch.
N modAdd(const N& a, const N& b, const N& m) {
    N s, t;
    u128 c = 0;
    for (int i = 0; i < 4; i++) {
        c += u128(a.v[i]) + b.v[i];
        s.v[i] = uint64_t(c);
        c >>= 64;
    }
    const uint64_t carryOut = uint64_t(c);
    uint64_t borrow = 0;
    for (int i = 0; i < 4; i++) {
        const u128 d = u128(s.v[i]) - m.v[i] - borrow;
        t.v[i] = uint64_t(d);
        borrow = uint64_t(d >> 64) & 1;
    }
    // Take t where the sum carried out, or did not borrow subtracting m.
    const uint64_t takeT = 0 - (carryOut | (borrow ^ 1));
    N r;
    for (int i = 0; i < 4; i++) r.v[i] = (t.v[i] & takeT) | (s.v[i] & ~takeT);
    return r;
}

N modSub(const N& a, const N& b, const N& m) {
    N d, e;
    uint64_t borrow = 0;
    for (int i = 0; i < 4; i++) {
        const u128 x = u128(a.v[i]) - b.v[i] - borrow;
        d.v[i] = uint64_t(x);
        borrow = uint64_t(x >> 64) & 1;
    }
    // Add m back where it went below zero.
    const uint64_t mask = 0 - borrow;
    u128 c = 0;
    for (int i = 0; i < 4; i++) {
        c += u128(d.v[i]) + (m.v[i] & mask);
        e.v[i] = uint64_t(c);
        c >>= 64;
    }
    return e;
}

// a b R^-1 mod m (CIOS), both below m.
N montMul(const N& a, const N& b, const Modulus& md) {
    uint64_t t[6] = {0, 0, 0, 0, 0, 0};
    for (int i = 0; i < 4; i++) {
        u128 c = 0;
        for (int j = 0; j < 4; j++) {
            c += u128(a.v[j]) * b.v[i] + t[j];
            t[j] = uint64_t(c);
            c >>= 64;
        }
        c += t[4];
        t[4] = uint64_t(c);
        t[5] = uint64_t(c >> 64);
        const uint64_t q = t[0] * md.m0inv;
        c = u128(q) * md.m.v[0] + t[0];
        c >>= 64;
        for (int j = 1; j < 4; j++) {
            c += u128(q) * md.m.v[j] + t[j];
            t[j - 1] = uint64_t(c);
            c >>= 64;
        }
        c += t[4];
        t[3] = uint64_t(c);
        t[4] = t[5] + uint64_t(c >> 64);
    }
    // One subtraction of m at most, chosen by mask.
    N r, s;
    for (int i = 0; i < 4; i++) r.v[i] = t[i];
    uint64_t borrow = 0;
    for (int i = 0; i < 4; i++) {
        const u128 x = u128(r.v[i]) - md.m.v[i] - borrow;
        s.v[i] = uint64_t(x);
        borrow = uint64_t(x >> 64) & 1;
    }
    const uint64_t takeS = 0 - (t[4] | (borrow ^ 1));
    for (int i = 0; i < 4; i++) r.v[i] = (s.v[i] & takeS) | (r.v[i] & ~takeS);
    return r;
}

N fromBe(const uint8_t b[32]) {
    N r;
    for (int i = 0; i < 4; i++) r.v[i] = load64be(b + 8 * (3 - i));
    return r;
}
void toBe(uint8_t b[32], const N& a) {
    for (int i = 0; i < 4; i++) store64be(b + 8 * (3 - i), a.v[i]);
}

// Whether a < b, as numbers (public values only: it may branch).
bool lessThan(const N& a, const N& b) {
    for (int i = 3; i >= 0; i--) {
        if (a.v[i] != b.v[i]) return a.v[i] < b.v[i];
    }
    return false;
}
bool isZeroN(const N& a) { return (a.v[0] | a.v[1] | a.v[2] | a.v[3]) == 0; }

Modulus makeModulus(const char* hex) {
    uint8_t b[32];
    for (int i = 0; i < 32; i++) {
        auto digit = [](char c) { return c <= '9' ? c - '0' : c - 'a' + 10; };
        b[i] = uint8_t(digit(hex[2 * i]) * 16 + digit(hex[2 * i + 1]));
    }
    Modulus md;
    md.m = fromBe(b);
    uint64_t inv = 1;
    for (int i = 0; i < 6; i++) inv *= 2 - md.m.v[0] * inv;
    md.m0inv = 0 - inv;
    // R mod m and R^2 mod m by doubling 1, 256 and 512 times.
    N x{{1, 0, 0, 0}};
    for (int i = 0; i < 512; i++) {
        x = modAdd(x, x, md.m);
        if (i == 255) md.one = x;
    }
    md.r2 = x;
    return md;
}

N toMont(const N& a, const Modulus& md) { return montMul(a, md.r2, md); }
N fromMont(const N& a, const Modulus& md) { return montMul(a, N{{1, 0, 0, 0}}, md); }

// a^e for a public exponent e (as bytes, big-endian), in Montgomery form.
N montPow(const N& a, const N& e, const Modulus& md) {
    N r = md.one;
    for (int i = 255; i >= 0; i--) {
        r = montMul(r, r, md);
        if ((e.v[i / 64] >> (i % 64)) & 1) r = montMul(r, a, md);
    }
    return r;
}

// 1/a mod a prime m, as a^(m - 2); the exponent is public, so the work is the same for every a.
N montInvert(const N& a, const Modulus& md) {
    N e = md.m;
    // m - 2: m is odd and above 2, so no borrow past the first limb.
    e.v[0] -= 2;
    return montPow(a, e, md);
}

struct Curve {
    Modulus p, n;
    N b;           // Montgomery form, mod p
    N gx, gy;      // Montgomery form
    N sqrtExp;     // (p + 1) / 4
};

const Curve& curve() {
    static const Curve made = [] {
        Curve c;
        c.p = makeModulus("ffffffff00000001000000000000000000000000ffffffffffffffffffffffff");
        c.n = makeModulus("ffffffff00000000ffffffffffffffffbce6faada7179e84f3b9cac2fc632551");
        auto fromHex = [](const char* hex) {
            uint8_t b[32];
            for (int i = 0; i < 32; i++) {
                auto digit = [](char ch) { return ch <= '9' ? ch - '0' : ch - 'a' + 10; };
                b[i] = uint8_t(digit(hex[2 * i]) * 16 + digit(hex[2 * i + 1]));
            }
            return fromBe(b);
        };
        c.b = toMont(fromHex("5ac635d8aa3a93e7b3ebbd55769886bc651d06b0cc53b0f63bce3c3e27d2604b"), c.p);
        c.gx = toMont(fromHex("6b17d1f2e12c4247f8bce6e563a440f277037d812deb33a0f4a13945d898c296"), c.p);
        c.gy = toMont(fromHex("4fe342e2fe1a7f9b8ee7eb4a7c0f9e162bce33576b315ececbb6406837bf51f5"), c.p);
        // (p + 1) / 4: p + 1 = ffffffff00000001 0 0 0000000100000000 0000000000000000 shifted right by 2.
        N p1 = c.p.m;
        u128 carryOver = 1;
        for (int i = 0; i < 4; i++) {
            carryOver += p1.v[i];
            p1.v[i] = uint64_t(carryOver);
            carryOver >>= 64;
        }
        for (int i = 0; i < 4; i++) c.sqrtExp.v[i] = (p1.v[i] >> 2) | (i < 3 ? p1.v[i + 1] << 62 : 0);
        return c;
    }();
    return made;
}

// A point (X : Y : Z), Montgomery form; Z = 0 is infinity.
struct Pt {
    N x, y, z;
};

// Algorithm 4 of Renes, Costello and Batina: complete addition on y^2 = x^3 - 3x + b.
Pt ptAdd(const Pt& P, const Pt& Q) {
    const Curve& c = curve();
    const Modulus& md = c.p;
    const N& m = md.m;
    auto mul = [&](const N& a, const N& b) { return montMul(a, b, md); };
    auto add = [&](const N& a, const N& b) { return modAdd(a, b, m); };
    auto sub = [&](const N& a, const N& b) { return modSub(a, b, m); };
    N t0 = mul(P.x, Q.x), t1 = mul(P.y, Q.y), t2 = mul(P.z, Q.z);
    N t3 = add(P.x, P.y), t4 = add(Q.x, Q.y);
    t3 = mul(t3, t4);
    t4 = add(t0, t1);
    t3 = sub(t3, t4);
    t4 = add(P.y, P.z);
    N x3 = add(Q.y, Q.z);
    t4 = mul(t4, x3);
    x3 = add(t1, t2);
    t4 = sub(t4, x3);
    x3 = add(P.x, P.z);
    N y3 = add(Q.x, Q.z);
    x3 = mul(x3, y3);
    y3 = add(t0, t2);
    y3 = sub(x3, y3);
    N z3 = mul(c.b, t2);
    x3 = sub(y3, z3);
    z3 = add(x3, x3);
    x3 = add(x3, z3);
    z3 = sub(t1, x3);
    x3 = add(t1, x3);
    y3 = mul(c.b, y3);
    t1 = add(t2, t2);
    t2 = add(t1, t2);
    y3 = sub(y3, t2);
    y3 = sub(y3, t0);
    t1 = add(y3, y3);
    y3 = add(t1, y3);
    t1 = add(t0, t0);
    t0 = add(t1, t0);
    t0 = sub(t0, t2);
    t1 = mul(t4, y3);
    t2 = mul(t0, y3);
    y3 = mul(x3, z3);
    y3 = add(y3, t2);
    x3 = mul(t3, x3);
    x3 = sub(x3, t1);
    z3 = mul(t4, z3);
    t1 = mul(t3, t0);
    z3 = add(z3, t1);
    return Pt{x3, y3, z3};
}

Pt infinity() { return Pt{N{{0, 0, 0, 0}}, curve().p.one, N{{0, 0, 0, 0}}}; }

void select(Pt& r, const Pt& a, uint64_t take) {
    const uint64_t mask = 0 - take;
    for (int i = 0; i < 4; i++) {
        r.x.v[i] ^= mask & (r.x.v[i] ^ a.x.v[i]);
        r.y.v[i] ^= mask & (r.y.v[i] ^ a.y.v[i]);
        r.z.v[i] ^= mask & (r.z.v[i] ^ a.z.v[i]);
    }
}

// [k]P for a scalar of 32 bytes, big-endian.
Pt scalarMul(const uint8_t k[32], const Pt& p) {
    Pt r = infinity();
    for (int i = 0; i < 256; i++) {
        r = ptAdd(r, r);
        const Pt sum = ptAdd(r, p);
        select(r, sum, (k[i / 8] >> (7 - i % 8)) & 1);
    }
    return r;
}

Pt generator() { return Pt{curve().gx, curve().gy, curve().p.one}; }

// The affine x and y, normal form; false for infinity.
bool affine(const Pt& p, N& x, N& y) {
    const Modulus& md = curve().p;
    if (isZeroN(fromMont(p.z, md))) return false;
    const N zi = montInvert(p.z, md);
    x = fromMont(montMul(p.x, zi, md), md);
    y = fromMont(montMul(p.y, zi, md), md);
    return true;
}

// Whether (x, y), Montgomery form, is on the curve: y^2 = x^3 - 3x + b.
bool onCurve(const N& x, const N& y) {
    const Curve& c = curve();
    const Modulus& md = c.p;
    const N y2 = montMul(y, y, md);
    N rhs = montMul(montMul(x, x, md), x, md);
    const N x3 = modAdd(modAdd(x, x, md.m), x, md.m);
    rhs = modAdd(modSub(rhs, x3, md.m), c.b, md.m);
    for (int i = 0; i < 4; i++)
        if (y2.v[i] != rhs.v[i]) return false;
    return true;
}

}  // namespace

// ---- What the rest of ww calls ----

// A public point from its encoding: 04 X Y (65 bytes) or 02/03 X (33 bytes), on the curve.
bool p256Point(const uint8_t* bytes, size_t size, uint8_t out65[65]) {
    const Curve& c = curve();
    const Modulus& md = c.p;
    if (size == 65 && bytes[0] == 4) {
        const N x = fromBe(bytes + 1), y = fromBe(bytes + 33);
        if (!lessThan(x, md.m) || !lessThan(y, md.m)) return false;
        if (!onCurve(toMont(x, md), toMont(y, md))) return false;
        std::memcpy(out65, bytes, 65);
        return true;
    }
    if (size == 33 && (bytes[0] == 2 || bytes[0] == 3)) {
        const N x = fromBe(bytes + 1);
        if (!lessThan(x, md.m)) return false;
        const N xm = toMont(x, md);
        N rhs = montMul(montMul(xm, xm, md), xm, md);
        rhs = modAdd(modSub(rhs, modAdd(modAdd(xm, xm, md.m), xm, md.m), md.m), c.b, md.m);
        N ym = montPow(rhs, c.sqrtExp, md);
        if (!onCurve(xm, ym)) return false;  // rhs has no square root: x is on no point
        N y = fromMont(ym, md);
        if (int(y.v[0] & 1) != (bytes[0] & 1)) y = modSub(N{{0, 0, 0, 0}}, y, md.m);
        out65[0] = 4;
        toBe(out65 + 1, x);
        toBe(out65 + 33, y);
        return true;
    }
    return false;
}

// Whether 32 bytes are a private key: 1 to n - 1.
bool p256ValidPrivate(const uint8_t d[32]) {
    const N k = fromBe(d);
    return !isZeroN(k) && lessThan(k, curve().n.m);
}

// The public point of a private key, 04 X Y.
void p256Public(uint8_t out65[65], const uint8_t d[32]) {
    N x, y;
    affine(scalarMul(d, generator()), x, y);
    out65[0] = 4;
    toBe(out65 + 1, x);
    toBe(out65 + 33, y);
}

// The shared x of a private key and a peer's point (65 bytes, checked); false where the result
// is infinity.
bool p256Ecdh(uint8_t out32[32], const uint8_t d[32], const uint8_t point65[65]) {
    const Modulus& md = curve().p;
    const Pt q{toMont(fromBe(point65 + 1), md), toMont(fromBe(point65 + 33), md), md.one};
    N x, y;
    if (!affine(scalarMul(d, q), x, y)) return false;
    toBe(out32, x);
    return true;
}

namespace {

// bits2int of RFC 6979 for a 256-bit n: the hash's first 32 bytes (zeros before a shorter one),
// then reduced below n with one subtraction at most.
N hashToScalar(const uint8_t* hash, size_t size) {
    uint8_t b[32] = {};
    if (size >= 32) std::memcpy(b, hash, 32);
    else std::memcpy(b + 32 - size, hash, size);
    const Modulus& n = curve().n;
    N e = fromBe(b);
    return lessThan(e, n.m) ? e : modSub(e, n.m, n.m);  // the hash is public: a branch on it tells nothing
}

}  // namespace

// An ECDSA signature (r, s), each 32 bytes big-endian, of a message's hash, with RFC 6979's nonce
// made with HMAC over the same hash.
void p256Sign(uint8_t r32[32], uint8_t s32[32], const uint8_t d[32], const uint8_t* hash, size_t hashSize, const Hash& prototype) {
    const Curve& c = curve();
    const Modulus& n = c.n;
    const N e = hashToScalar(hash, hashSize);
    uint8_t h1[32];
    toBe(h1, e);  // bits2octets(h1)
    const size_t hlen = prototype.size();
    std::vector<uint8_t> v(hlen, 0x01), k(hlen, 0x00);
    auto hmac = [&](std::initializer_list<std::pair<const uint8_t*, size_t>> parts, std::vector<uint8_t>& out) {
        Hmac mac(prototype, k.data(), k.size());
        for (const auto& part : parts) mac.update(part.first, part.second);
        out.resize(hlen);
        mac.finish(out.data());
    };
    const uint8_t zero = 0, one = 1;
    hmac({{v.data(), hlen}, {&zero, 1}, {d, 32}, {h1, 32}}, k);
    hmac({{v.data(), hlen}}, v);
    hmac({{v.data(), hlen}, {&one, 1}, {d, 32}, {h1, 32}}, k);
    hmac({{v.data(), hlen}}, v);
    const N dm = toMont(fromBe(d), n);
    for (;;) {
        std::vector<uint8_t> t;
        while (t.size() < 32) {
            hmac({{v.data(), hlen}}, v);
            t.insert(t.end(), v.begin(), v.end());
        }
        uint8_t candidate[32];
        std::memcpy(candidate, t.data(), 32);
        const N kn = fromBe(candidate);
        if (!isZeroN(kn) && lessThan(kn, n.m)) {
            N x, y;
            affine(scalarMul(candidate, generator()), x, y);
            const N r = lessThan(x, n.m) ? x : modSub(x, n.m, n.m);
            if (!isZeroN(r)) {
                // s = k^-1 (e + r d) mod n.
                const N km = toMont(kn, n), rm = toMont(r, n), em = toMont(e, n);
                const N s = fromMont(montMul(montInvert(km, n), modAdd(em, montMul(rm, dm, n), n.m), n), n);
                if (!isZeroN(s)) {
                    toBe(r32, r);
                    toBe(s32, s);
                    wipe(candidate, sizeof(candidate));
                    wipe(k.data(), k.size());
                    return;
                }
            }
        }
        hmac({{v.data(), hlen}, {&zero, 1}}, k);
        hmac({{v.data(), hlen}}, v);
    }
}

// Whether (r, s) signs the hash for the public point (65 bytes, checked).
bool p256Verify(const uint8_t point65[65], const uint8_t* hash, size_t hashSize, const uint8_t r32[32], const uint8_t s32[32]) {
    const Curve& c = curve();
    const Modulus& n = c.n;
    const N r = fromBe(r32), s = fromBe(s32);
    if (isZeroN(r) || isZeroN(s) || !lessThan(r, n.m) || !lessThan(s, n.m)) return false;
    const N e = hashToScalar(hash, hashSize);
    const N w = montInvert(toMont(s, n), n);
    uint8_t u1[32], u2[32];
    toBe(u1, fromMont(montMul(toMont(e, n), w, n), n));
    toBe(u2, fromMont(montMul(toMont(r, n), w, n), n));
    const Modulus& p = c.p;
    const Pt q{toMont(fromBe(point65 + 1), p), toMont(fromBe(point65 + 33), p), p.one};
    N x, y;
    if (!affine(ptAdd(scalarMul(u1, generator()), scalarMul(u2, q)), x, y)) return false;
    const N v = lessThan(x, n.m) ? x : modSub(x, n.m, n.m);
    for (int i = 0; i < 4; i++)
        if (v.v[i] != r.v[i]) return false;
    return true;
}

}  // namespace crypto
}  // namespace ww
