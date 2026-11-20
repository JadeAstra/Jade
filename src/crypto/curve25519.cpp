// X25519 (RFC 7748) and Ed25519 (RFC 8032), over the field of 2^255 - 19 in five limbs of 51
// bits. Every operation on a secret is constant-time: arithmetic, a ladder or double-and-add that
// does the same work for every bit, and selections by mask. The scalar arithmetic mod L is
// TweetNaCl's (public domain), which is constant-time too. Verification works on public values
// and may take a time that depends on them.

#include "crypto.h"

namespace ww {
namespace crypto {

namespace {

using u128 = unsigned __int128;
constexpr uint64_t kMask51 = (uint64_t(1) << 51) - 1;

struct Fe {
    uint64_t v[5];
};

Fe feZero() { return Fe{{0, 0, 0, 0, 0}}; }
Fe feOne() { return Fe{{1, 0, 0, 0, 0}}; }

// Limbs back under 2^51 (the last carry folded in times 19).
inline Fe carry(Fe a) {
    uint64_t c;
    c = a.v[0] >> 51, a.v[0] &= kMask51, a.v[1] += c;
    c = a.v[1] >> 51, a.v[1] &= kMask51, a.v[2] += c;
    c = a.v[2] >> 51, a.v[2] &= kMask51, a.v[3] += c;
    c = a.v[3] >> 51, a.v[3] &= kMask51, a.v[4] += c;
    c = a.v[4] >> 51, a.v[4] &= kMask51, a.v[0] += c * 19;
    c = a.v[0] >> 51, a.v[0] &= kMask51, a.v[1] += c;
    return a;
}

inline Fe add(const Fe& a, const Fe& b) {
    Fe r;
    for (int i = 0; i < 5; i++) r.v[i] = a.v[i] + b.v[i];
    return carry(r);
}

// a - b, as a + 4p - b so that no limb goes below zero.
inline Fe sub(const Fe& a, const Fe& b) {
    Fe r;
    r.v[0] = a.v[0] + 0x1FFFFFFFFFFFB4ULL - b.v[0];
    for (int i = 1; i < 5; i++) r.v[i] = a.v[i] + 0x1FFFFFFFFFFFFCULL - b.v[i];
    return carry(r);
}

inline Fe neg(const Fe& a) { return sub(feZero(), a); }

inline Fe mul(const Fe& a, const Fe& b) {
    const uint64_t b1 = b.v[1] * 19, b2 = b.v[2] * 19, b3 = b.v[3] * 19, b4 = b.v[4] * 19;
    u128 r0 = u128(a.v[0]) * b.v[0] + u128(a.v[1]) * b4 + u128(a.v[2]) * b3 + u128(a.v[3]) * b2 + u128(a.v[4]) * b1;
    u128 r1 = u128(a.v[0]) * b.v[1] + u128(a.v[1]) * b.v[0] + u128(a.v[2]) * b4 + u128(a.v[3]) * b3 + u128(a.v[4]) * b2;
    u128 r2 = u128(a.v[0]) * b.v[2] + u128(a.v[1]) * b.v[1] + u128(a.v[2]) * b.v[0] + u128(a.v[3]) * b4 + u128(a.v[4]) * b3;
    u128 r3 = u128(a.v[0]) * b.v[3] + u128(a.v[1]) * b.v[2] + u128(a.v[2]) * b.v[1] + u128(a.v[3]) * b.v[0] + u128(a.v[4]) * b4;
    u128 r4 = u128(a.v[0]) * b.v[4] + u128(a.v[1]) * b.v[3] + u128(a.v[2]) * b.v[2] + u128(a.v[3]) * b.v[1] + u128(a.v[4]) * b.v[0];
    Fe r;
    r1 += uint64_t(r0 >> 51), r.v[0] = uint64_t(r0) & kMask51;
    r2 += uint64_t(r1 >> 51), r.v[1] = uint64_t(r1) & kMask51;
    r3 += uint64_t(r2 >> 51), r.v[2] = uint64_t(r2) & kMask51;
    r4 += uint64_t(r3 >> 51), r.v[3] = uint64_t(r3) & kMask51;
    const uint64_t c = uint64_t(r4 >> 51);
    r.v[4] = uint64_t(r4) & kMask51;
    r.v[0] += c * 19;
    r.v[1] += r.v[0] >> 51;
    r.v[0] &= kMask51;
    return r;
}

inline Fe sq(const Fe& a) { return mul(a, a); }

inline Fe mulSmall(const Fe& a, uint64_t k) {
    u128 c = 0;
    Fe r;
    for (int i = 0; i < 5; i++) {
        c += u128(a.v[i]) * k;
        r.v[i] = uint64_t(c) & kMask51;
        c >>= 51;
    }
    r.v[0] += uint64_t(c) * 19;
    return carry(r);
}

// a squared n times.
inline Fe sqn(Fe a, int n) {
    while (n-- > 0) a = sq(a);
    return a;
}

// z^(2^250 - 1), and z^11, the start of both chains below.
void chain250(const Fe& z, Fe& z250, Fe& z11) {
    const Fe z2 = sq(z);
    const Fe z9 = mul(sqn(z2, 2), z);
    z11 = mul(z9, z2);
    const Fe z5 = mul(sq(z11), z9);           // z^(2^5 - 1)
    const Fe z10 = mul(sqn(z5, 5), z5);       // z^(2^10 - 1)
    const Fe z20 = mul(sqn(z10, 10), z10);    // z^(2^20 - 1)
    const Fe z40 = mul(sqn(z20, 20), z20);    // z^(2^40 - 1)
    const Fe z50 = mul(sqn(z40, 10), z10);    // z^(2^50 - 1)
    const Fe z100 = mul(sqn(z50, 50), z50);   // z^(2^100 - 1)
    const Fe z200 = mul(sqn(z100, 100), z100);// z^(2^200 - 1)
    z250 = mul(sqn(z200, 50), z50);           // z^(2^250 - 1)
}

// 1/z, as z^(p - 2) = z^(2^255 - 21).
Fe invert(const Fe& z) {
    Fe z250, z11;
    chain250(z, z250, z11);
    return mul(sqn(z250, 5), z11);
}

// z^((p - 5) / 8) = z^(2^252 - 3), for square roots.
Fe pow22523(const Fe& z) {
    Fe z250, z11;
    chain250(z, z250, z11);
    return mul(sqn(z250, 2), z);
}

Fe fromBytes(const uint8_t s[32]) {
    const uint64_t t0 = load64le(s), t1 = load64le(s + 8), t2 = load64le(s + 16), t3 = load64le(s + 24);
    Fe h;
    h.v[0] = t0 & kMask51;
    h.v[1] = ((t0 >> 51) | (t1 << 13)) & kMask51;
    h.v[2] = ((t1 >> 38) | (t2 << 26)) & kMask51;
    h.v[3] = ((t2 >> 25) | (t3 << 39)) & kMask51;
    h.v[4] = (t3 >> 12) & kMask51;  // the top bit left out
    return h;
}

// The canonical 32 bytes: fully reduced below p.
void toBytes(uint8_t s[32], Fe h) {
    h = carry(carry(h));
    // q = 1 where h >= p: h + 19 carries out of bit 255.
    uint64_t q = (h.v[0] + 19) >> 51;
    q = (h.v[1] + q) >> 51;
    q = (h.v[2] + q) >> 51;
    q = (h.v[3] + q) >> 51;
    q = (h.v[4] + q) >> 51;
    h.v[0] += 19 * q;
    h.v[1] += h.v[0] >> 51, h.v[0] &= kMask51;
    h.v[2] += h.v[1] >> 51, h.v[1] &= kMask51;
    h.v[3] += h.v[2] >> 51, h.v[2] &= kMask51;
    h.v[4] += h.v[3] >> 51, h.v[3] &= kMask51;
    h.v[4] &= kMask51;
    store64le(s, h.v[0] | (h.v[1] << 51));
    store64le(s + 8, (h.v[1] >> 13) | (h.v[2] << 38));
    store64le(s + 16, (h.v[2] >> 26) | (h.v[3] << 25));
    store64le(s + 24, (h.v[3] >> 39) | (h.v[4] << 12));
}

bool isZero(const Fe& a) {
    uint8_t s[32];
    toBytes(s, a);
    uint8_t any = 0;
    for (int i = 0; i < 32; i++) any |= s[i];
    return any == 0;
}
bool isNegative(const Fe& a) {
    uint8_t s[32];
    toBytes(s, a);
    return s[0] & 1;
}
bool equalFe(const Fe& a, const Fe& b) { return isZero(sub(a, b)); }

// a and b swapped where `swap` is 1, by mask.
inline void cswap(Fe& a, Fe& b, uint64_t swap) {
    const uint64_t mask = 0 - swap;
    for (int i = 0; i < 5; i++) {
        const uint64_t t = mask & (a.v[i] ^ b.v[i]);
        a.v[i] ^= t;
        b.v[i] ^= t;
    }
}

}  // namespace

// ---- X25519 ----

bool x25519(uint8_t out[32], const uint8_t scalar[32], const uint8_t point[32]) {
    uint8_t k[32];
    std::memcpy(k, scalar, 32);
    k[0] &= 248;
    k[31] &= 127;
    k[31] |= 64;
    const Fe x1 = fromBytes(point);  // its top bit masked, as RFC 7748 says
    Fe x2 = feOne(), z2 = feZero(), x3 = x1, z3 = feOne();
    uint64_t swap = 0;
    for (int t = 254; t >= 0; t--) {
        const uint64_t bit = (k[t / 8] >> (t % 8)) & 1;
        swap ^= bit;
        cswap(x2, x3, swap);
        cswap(z2, z3, swap);
        swap = bit;
        const Fe a = add(x2, z2), aa = sq(a), b = sub(x2, z2), bb = sq(b), e = sub(aa, bb);
        const Fe c = add(x3, z3), d = sub(x3, z3), da = mul(d, a), cb = mul(c, b);
        x3 = sq(add(da, cb));
        z3 = mul(x1, sq(sub(da, cb)));
        x2 = mul(aa, bb);
        z2 = mul(e, add(aa, mulSmall(e, 121665)));
    }
    cswap(x2, x3, swap);
    cswap(z2, z3, swap);
    toBytes(out, mul(x2, invert(z2)));
    wipe(k, sizeof(k));
    // All zeros: the peer's point was of small order, and the secret is nothing.
    uint8_t any = 0;
    for (int i = 0; i < 32; i++) any |= out[i];
    return any != 0;
}

void x25519Public(uint8_t out[32], const uint8_t scalar[32]) {
    uint8_t base[32] = {9};
    x25519(out, scalar, base);
}

// ---- Ed25519 ----

namespace {

// A point in extended coordinates: x = X/Z, y = Y/Z, xy = T/Z.
struct Ge {
    Fe x, y, z, t;
};

struct EdConstants {
    Fe d, d2, sqrtM1;
    Ge base;
};

bool decodePoint(Ge& p, const uint8_t s[32], const EdConstants& k);

const EdConstants& ed() {
    static const EdConstants made = [] {
        EdConstants k;
        // d = -121665 / 121666; sqrt(-1) = 2^((p - 1) / 4).
        k.d = mul(neg(mulSmall(feOne(), 121665)), invert(mulSmall(feOne(), 121666)));
        k.d2 = add(k.d, k.d);
        Fe two = mulSmall(feOne(), 2), power = feOne();
        // 2^((p - 1) / 4) = 2^(2^253 - 5): (2^(2^252 - 3))^2 * 2.
        power = mul(sq(pow22523(two)), two);
        k.sqrtM1 = power;
        uint8_t encoded[32];
        std::memset(encoded, 0x66, 32);
        encoded[0] = 0x58;  // y = 4/5, x even
        decodePoint(k.base, encoded, k);
        return k;
    }();
    return made;
}

Ge identity() { return Ge{feZero(), feOne(), feOne(), feZero()}; }

Ge geAdd(const Ge& p, const Ge& q) {
    const EdConstants& k = ed();
    const Fe a = mul(sub(p.y, p.x), sub(q.y, q.x));
    const Fe b = mul(add(p.y, p.x), add(q.y, q.x));
    const Fe c = mul(mul(p.t, k.d2), q.t);
    const Fe d = mul(add(p.z, p.z), q.z);
    const Fe e = sub(b, a), f = sub(d, c), g = add(d, c), h = add(b, a);
    return Ge{mul(e, f), mul(g, h), mul(f, g), mul(e, h)};
}

Ge geDouble(const Ge& p) {
    const Fe a = sq(p.x), b = sq(p.y), c = add(sq(p.z), sq(p.z));
    const Fe h = add(a, b), e = sub(h, sq(add(p.x, p.y))), g = sub(a, b), f = add(c, g);
    return Ge{mul(e, f), mul(g, h), mul(f, g), mul(e, h)};
}

void geSelect(Ge& r, const Ge& a, uint64_t take) {
    const uint64_t mask = 0 - take;
    for (int i = 0; i < 5; i++) {
        r.x.v[i] ^= mask & (r.x.v[i] ^ a.x.v[i]);
        r.y.v[i] ^= mask & (r.y.v[i] ^ a.y.v[i]);
        r.z.v[i] ^= mask & (r.z.v[i] ^ a.z.v[i]);
        r.t.v[i] ^= mask & (r.t.v[i] ^ a.t.v[i]);
    }
}

// [s]P for a 32-byte little-endian scalar: a double and an add for every bit, the sum taken
// or not by mask.
Ge scalarMul(const uint8_t s[32], const Ge& p) {
    Ge r = identity();
    for (int i = 255; i >= 0; i--) {
        r = geDouble(r);
        const Ge sum = geAdd(r, p);
        geSelect(r, sum, (s[i / 8] >> (i % 8)) & 1);
    }
    return r;
}

void encodePoint(uint8_t s[32], const Ge& p) {
    const Fe zi = invert(p.z);
    const Fe x = mul(p.x, zi), y = mul(p.y, zi);
    toBytes(s, y);
    s[31] ^= uint8_t(isNegative(x) << 7);
}

// RFC 8032, 5.1.3: false where the bytes are no point (y not below p, no square root, or x = 0
// with its sign bit set).
bool decodePoint(Ge& p, const uint8_t s[32], const EdConstants& k) {
    uint8_t bytes[32];
    std::memcpy(bytes, s, 32);
    const int sign = bytes[31] >> 7;
    bytes[31] &= 0x7F;
    // y below p: p = 2^255 - 19, so only 0x7F 0xFF... 0xED and above are not.
    bool below = false;
    if (bytes[31] != 0x7F) below = true;
    for (int i = 30; i >= 1 && !below; i--)
        if (bytes[i] != 0xFF) below = true;
    if (!below && bytes[0] >= 0xED) return false;
    const Fe y = fromBytes(bytes);
    const Fe y2 = sq(y);
    const Fe u = sub(y2, feOne());
    const Fe v = add(mul(y2, k.d), feOne());
    const Fe v3 = mul(sq(v), v);
    Fe x = mul(mul(u, v3), pow22523(mul(u, mul(sq(v3), v))));  // u v^3 (u v^7)^((p-5)/8)
    const Fe vx2 = mul(v, sq(x));
    if (!equalFe(vx2, u)) {
        if (!equalFe(vx2, neg(u))) return false;
        x = mul(x, k.sqrtM1);
    }
    if (isZero(x) && sign == 1) return false;
    if (int(isNegative(x)) != sign) x = neg(x);
    p = Ge{x, y, feOne(), mul(x, y)};
    return true;
}

// ---- Scalars mod L (TweetNaCl's modL, public domain) ----

const int64_t kL[32] = {0xed, 0xd3, 0xf5, 0x5c, 0x1a, 0x63, 0x12, 0x58, 0xd6, 0x9c, 0xf7, 0xa2, 0xde, 0xf9, 0xde, 0x14,
                        0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0x10};

void modL(uint8_t r[32], int64_t x[64]) {
    int64_t carryOver;
    int i, j;
    for (i = 63; i >= 32; --i) {
        carryOver = 0;
        for (j = i - 32; j < i - 12; ++j) {
            x[j] += carryOver - 16 * x[i] * kL[j - (i - 32)];
            carryOver = (x[j] + 128) >> 8;
            x[j] -= carryOver * 256;
        }
        x[j] += carryOver;
        x[i] = 0;
    }
    carryOver = 0;
    for (j = 0; j < 32; j++) {
        x[j] += carryOver - (x[31] >> 4) * kL[j];
        carryOver = x[j] >> 8;
        x[j] &= 255;
    }
    for (j = 0; j < 32; j++) x[j] -= carryOver * kL[j];
    for (i = 0; i < 32; i++) {
        x[i + 1] += x[i] >> 8;
        r[i] = uint8_t(x[i] & 255);
    }
}

// 64 bytes (a hash) reduced mod L.
void reduce64(uint8_t r[32], const uint8_t h[64]) {
    int64_t x[64];
    for (int i = 0; i < 64; i++) x[i] = h[i];
    modL(r, x);
    wipe(x, sizeof(x));
}

// Whether a 32-byte scalar is below L (a signature's S must be).
bool belowL(const uint8_t s[32]) {
    for (int i = 31; i >= 0; i--) {
        if (s[i] < kL[i]) return true;
        if (s[i] > kL[i]) return false;
    }
    return false;
}

void sha512(uint8_t out[64], std::initializer_list<std::pair<const uint8_t*, size_t>> parts) {
    std::unique_ptr<Hash> h = newHash(HashKind::Sha512);
    for (const auto& part : parts) h->update(part.first, part.second);
    h->finish(out);
}

}  // namespace

void ed25519Public(uint8_t out[32], const uint8_t seed[32]) {
    uint8_t h[64];
    sha512(h, {{seed, 32}});
    h[0] &= 248;
    h[31] &= 127;
    h[31] |= 64;
    encodePoint(out, scalarMul(h, ed().base));
    wipe(h, sizeof(h));
}

void ed25519Sign(uint8_t sig[64], const uint8_t seed[32], const uint8_t* message, size_t size) {
    uint8_t h[64], a[32], publicKey[32], nonce[64], r[32], k[64], kr[32];
    sha512(h, {{seed, 32}});
    std::memcpy(a, h, 32);
    a[0] &= 248;
    a[31] &= 127;
    a[31] |= 64;
    encodePoint(publicKey, scalarMul(a, ed().base));
    sha512(nonce, {{h + 32, 32}, {message, size}});
    reduce64(r, nonce);
    encodePoint(sig, scalarMul(r, ed().base));
    sha512(k, {{sig, 32}, {publicKey, 32}, {message, size}});
    reduce64(kr, k);
    // S = r + k a mod L.
    int64_t x[64] = {};
    for (int i = 0; i < 32; i++) x[i] = r[i];
    for (int i = 0; i < 32; i++)
        for (int j = 0; j < 32; j++) x[i + j] += int64_t(kr[i]) * a[j];
    modL(sig + 32, x);
    wipe(h, sizeof(h));
    wipe(a, sizeof(a));
    wipe(nonce, sizeof(nonce));
    wipe(r, sizeof(r));
    wipe(x, sizeof(x));
}

bool ed25519Verify(const uint8_t publicKey[32], const uint8_t* message, size_t size, const uint8_t sig[64]) {
    const EdConstants& k = ed();
    Ge a;
    if (!decodePoint(a, publicKey, k)) return false;
    if (!belowL(sig + 32)) return false;
    uint8_t h[64], kr[32], check[32];
    sha512(h, {{sig, 32}, {publicKey, 32}, {message, size}});
    reduce64(kr, h);
    // [S]B - [k]A, encoded, must be R as given.
    const Ge minusA{neg(a.x), a.y, a.z, neg(a.t)};
    encodePoint(check, geAdd(scalarMul(sig + 32, k.base), scalarMul(kr, minusA)));
    return std::memcmp(check, sig, 32) == 0;
}

}  // namespace crypto
}  // namespace ww
