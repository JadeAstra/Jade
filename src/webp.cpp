// What WebP (src/std/webp.jade) is built on: the work for every pixel of its two codecs, VP8L
// (lossless) and VP8 (lossy), in C++ of WebP's own, as JPEG's scans are (the user, 2026-11-08).
// The RIFF container, the extended header, alpha's chunk, animation's frames and errors are Jade.
// Written from the WebP specification (RFC 9649).

#include <algorithm>
#include <array>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "builtins.h"
#include "vm.h"

namespace jade {

namespace {

// ---- VP8L: lossless ----

// Bits lowest first, through 64 kept at a time; zeros past the end, which `overrun` notes.
struct LBits {
    const uint8_t* p;
    const uint8_t* end;
    uint64_t buffer = 0;
    int count = 0;
    size_t beyond = 0;

    void fill() {
        while (count <= 56) {
            if (p < end) buffer |= static_cast<uint64_t>(*p++) << count;
            else beyond++;
            count += 8;
        }
    }
    uint32_t peek(int n) {
        if (count < n) fill();
        return static_cast<uint32_t>(buffer & ((uint64_t{1} << n) - 1));
    }
    void drop(int n) {
        buffer >>= n;
        count -= n;
    }
    uint32_t get(int n) {
        if (n == 0) return 0;
        const uint32_t v = peek(n);
        drop(n);
        return v;
    }
    bool overrun() const { return static_cast<int64_t>(beyond) * 8 > count; }
};

uint32_t reverse(uint32_t code, int length) {
    uint32_t r = 0;
    for (int i = 0; i < length; i++) r |= ((code >> i) & 1) << (length - 1 - i);
    return r;
}

// A prefix code, lowest bit first: a table of the next 10 bits, and the rest a bit at a time. A
// code of one symbol takes no bits.
struct Prefix {
    static constexpr int kFast = 10;
    std::vector<uint32_t> fast;  // (symbol << 5) | length, 0xFFFFFFFF where longer
    std::vector<uint16_t> counts, sorted;
    std::vector<uint32_t> firstCode, firstIndex;
    int single = -1;

    bool build(const std::vector<int>& lengths) {
        const size_t n = lengths.size();
        counts.assign(16, 0);
        int used = 0, last = -1;
        for (size_t i = 0; i < n; i++)
            if (lengths[i] > 0) {
                counts[static_cast<size_t>(lengths[i])]++;
                used++;
                last = static_cast<int>(i);
            }
        if (used == 0) return false;
        if (used == 1) {
            single = last;
            return true;
        }
        int64_t left = 1;
        for (int l = 1; l < 16; l++) {
            left = left * 2 - counts[static_cast<size_t>(l)];
            if (left < 0) return false;
        }
        if (left != 0) return false;  // VP8L's codes are complete
        firstCode.assign(16, 0);
        firstIndex.assign(16, 0);
        std::vector<uint32_t> next(16, 0);
        uint32_t code = 0, index = 0;
        for (int l = 1; l < 16; l++) {
            code = (code + counts[static_cast<size_t>(l - 1)]) << 1;
            firstCode[static_cast<size_t>(l)] = code;
            firstIndex[static_cast<size_t>(l)] = index;
            next[static_cast<size_t>(l)] = index;
            index += counts[static_cast<size_t>(l)];
        }
        counts[0] = 0;
        sorted.assign(index, 0);
        fast.assign(size_t{1} << kFast, 0xFFFFFFFFu);
        for (size_t s = 0; s < n; s++) {
            const int l = lengths[s];
            if (l == 0) continue;
            const uint32_t at = next[static_cast<size_t>(l)]++;
            sorted[at] = static_cast<uint16_t>(s);
            if (l > kFast) continue;
            const uint32_t c = firstCode[static_cast<size_t>(l)] + (at - firstIndex[static_cast<size_t>(l)]);
            for (uint32_t k = reverse(c, l); k < (1u << kFast); k += 1u << l) fast[k] = (static_cast<uint32_t>(s) << 5) | static_cast<uint32_t>(l);
        }
        return true;
    }

    int read(LBits& b) const {
        if (single >= 0) return single;
        const uint32_t bits = b.peek(15);
        const uint32_t entry = fast[bits & ((1u << kFast) - 1)];
        if (entry != 0xFFFFFFFFu) {
            b.drop(static_cast<int>(entry & 31));
            return static_cast<int>(entry >> 5);
        }
        uint32_t c = reverse(bits & ((1u << kFast) - 1), kFast);
        for (int l = kFast + 1; l < 16; l++) {
            c = (c << 1) | ((bits >> (l - 1)) & 1);
            const uint32_t offset = c - firstCode[static_cast<size_t>(l)];
            if (offset < counts[static_cast<size_t>(l)]) {
                b.drop(l);
                return sorted[firstIndex[static_cast<size_t>(l)] + offset];
            }
        }
        return -1;
    }
};

constexpr int kCodeLengthOrder[19] = {17, 18, 0, 1, 2, 3, 4, 5, 16, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15};

// The 120 shortest distances, as (across, down) from the pixel: a distance code d of 1 to 120
// means the pixel there; past 120, d - 120 pixels back.
constexpr int8_t kPlane[120][2] = {
    {0, 1},  {1, 0},  {1, 1},  {-1, 1}, {0, 2},  {2, 0},  {1, 2},  {-1, 2}, {2, 1},  {-2, 1}, {2, 2},  {-2, 2}, {0, 3},  {3, 0},  {1, 3},
    {-1, 3}, {3, 1},  {-3, 1}, {2, 3},  {-2, 3}, {3, 2},  {-3, 2}, {0, 4},  {4, 0},  {1, 4},  {-1, 4}, {4, 1},  {-4, 1}, {3, 3},  {-3, 3},
    {2, 4},  {-2, 4}, {4, 2},  {-4, 2}, {0, 5},  {3, 4},  {-3, 4}, {4, 3},  {-4, 3}, {5, 0},  {1, 5},  {-1, 5}, {5, 1},  {-5, 1}, {2, 5},
    {-2, 5}, {5, 2},  {-5, 2}, {4, 4},  {-4, 4}, {3, 5},  {-3, 5}, {5, 3},  {-5, 3}, {0, 6},  {6, 0},  {1, 6},  {-1, 6}, {6, 1},  {-6, 1},
    {2, 6},  {-2, 6}, {6, 2},  {-6, 2}, {4, 5},  {-4, 5}, {5, 4},  {-5, 4}, {3, 6},  {-3, 6}, {6, 3},  {-6, 3}, {0, 7},  {7, 0},  {1, 7},
    {-1, 7}, {5, 5},  {-5, 5}, {7, 1},  {-7, 1}, {4, 6},  {-4, 6}, {6, 4},  {-6, 4}, {2, 7},  {-2, 7}, {7, 2},  {-7, 2}, {3, 7},  {-3, 7},
    {7, 3},  {-7, 3}, {5, 6},  {-5, 6}, {6, 5},  {-6, 5}, {8, 0},  {4, 7},  {-4, 7}, {7, 4},  {-7, 4}, {8, 1},  {8, 2},  {6, 6},  {-6, 6},
    {8, 3},  {5, 7},  {-5, 7}, {7, 5},  {-7, 5}, {8, 4},  {6, 7},  {-6, 7}, {7, 6},  {-7, 6}, {8, 5},  {7, 7},  {-7, 7}, {8, 6},  {8, 7}};

struct LosslessError {
    std::string what;
};

[[noreturn]] void fail(const std::string& what) { throw LosslessError{what}; }

class Lossless {
    struct Transform;

public:
    Lossless(const uint8_t* data, size_t size) : b_{data, data + size} {}

    // The picture: its size and ARGB pixels (0xAARRGGBB). `headerless` for ALPH's data, which has
    // no signature or size of its own.
    void read(std::vector<uint32_t>& argb, int& width, int& height, bool headerless = false) {
        if (!headerless) {
            if (b_.get(8) != 0x2F) fail("its lossless data does not start with 0x2F");
            width = static_cast<int>(b_.get(14)) + 1;
            height = static_cast<int>(b_.get(14)) + 1;
            b_.get(1);  // alpha is used: a hint only
            if (b_.get(3) != 0) fail("a lossless version other than 0");
        }
        // The transforms, in the order they were applied; undone in the other order.
        std::vector<Transform> transforms;
        int xsize = width;
        bool seen[4] = {};
        while (b_.get(1)) {
            Transform t;
            t.kind = static_cast<int>(b_.get(2));
            if (seen[t.kind]) fail("a transform used twice");
            seen[t.kind] = true;
            t.width = xsize;
            t.bits = 0;
            if (t.kind == 0 || t.kind == 1) {
                t.bits = static_cast<int>(b_.get(3)) + 2;
                t.data = image(subsampled(xsize, t.bits), subsampled(height, t.bits), false);
            } else if (t.kind == 3) {
                const int colors = static_cast<int>(b_.get(8)) + 1;
                t.data = image(colors, 1, false);
                for (size_t i = 1; i < t.data.size(); i++) t.data[i] = addPixels(t.data[i], t.data[i - 1]);
                t.data.resize(256, 0);  // an index past the table: transparent black
                t.bits = colors <= 2 ? 3 : colors <= 4 ? 2 : colors <= 16 ? 1 : 0;
                xsize = subsampled(xsize, t.bits);
            }
            transforms.push_back(std::move(t));
        }
        argb = image(xsize, height, true);
        for (size_t i = transforms.size(); i-- > 0;) undo(transforms[i], argb, height);
        if (b_.overrun()) fail("its lossless data ends early");
    }

    // A predictor's guess, for the writer.
    static uint32_t predictFor(int mode, const uint32_t* px, size_t at, size_t w) { return predict(mode, px, at, w); }

private:
    struct Transform {
        int kind, bits;
        std::vector<uint32_t> data;
        int width;  // the picture's width when it was applied
    };

    LBits b_;

    static int subsampled(int size, int bits) { return (size + (1 << bits) - 1) >> bits; }

    static uint32_t addPixels(uint32_t a, uint32_t b) {
        const uint32_t ag = (a & 0xFF00FF00u) + (b & 0xFF00FF00u), rb = (a & 0x00FF00FFu) + (b & 0x00FF00FFu);
        return (ag & 0xFF00FF00u) | (rb & 0x00FF00FFu);
    }

    std::vector<int> codeLengths(int alphabet) {
        std::vector<int> lengths(static_cast<size_t>(alphabet), 0);
        if (b_.get(1)) {  // simple: one or two symbols
            const int count = static_cast<int>(b_.get(1)) + 1;
            const int first = static_cast<int>(b_.get(b_.get(1) ? 8 : 1));
            if (first >= alphabet) fail("a code's symbol past its alphabet");
            lengths[static_cast<size_t>(first)] = 1;
            if (count == 2) {
                const int second = static_cast<int>(b_.get(8));
                if (second >= alphabet) fail("a code's symbol past its alphabet");
                lengths[static_cast<size_t>(second)] = 1;
            }
            return lengths;
        }
        const int count = static_cast<int>(b_.get(4)) + 4;
        if (count > 19) fail("too many code length codes");
        std::vector<int> lengthLengths(19, 0);
        for (int i = 0; i < count; i++) lengthLengths[static_cast<size_t>(kCodeLengthOrder[i])] = static_cast<int>(b_.get(3));
        Prefix lengthCode;
        if (!lengthCode.build(lengthLengths)) fail("a code length code of impossible lengths");
        int most = alphabet;
        if (b_.get(1)) {
            const int nbits = 2 + 2 * static_cast<int>(b_.get(3));
            most = 2 + static_cast<int>(b_.get(nbits));
            if (most > alphabet) fail("more code lengths than the alphabet has");
        }
        int previous = 8;
        for (int symbol = 0; symbol < alphabet;) {
            if (most-- == 0) break;
            const int length = lengthCode.read(b_);
            if (length < 0 || b_.overrun()) fail("bits that are no code length");
            if (length < 16) {
                lengths[static_cast<size_t>(symbol++)] = length;
                if (length != 0) previous = length;
                continue;
            }
            const int extra = length == 16 ? 2 : length == 17 ? 3 : 7;
            const int offset = length == 18 ? 11 : 3;
            const int repeat = static_cast<int>(b_.get(extra)) + offset;
            if (symbol + repeat > alphabet) fail("code lengths past the alphabet");
            const int value = length == 16 ? previous : 0;
            for (int k = 0; k < repeat; k++) lengths[static_cast<size_t>(symbol++)] = value;
        }
        return lengths;
    }

    struct Group {
        Prefix codes[5];  // green with lengths and the cache, red, blue, alpha, distance
    };

    Group group(int cacheSize) {
        Group g;
        const int alphabets[5] = {256 + 24 + cacheSize, 256, 256, 256, 40};
        for (int i = 0; i < 5; i++)
            if (!g.codes[i].build(codeLengths(alphabets[i]))) fail("a prefix code of impossible lengths");
        return g;
    }

    int lengthOrDistance(int prefix) {
        if (prefix < 4) return prefix + 1;
        const int extra = (prefix - 2) >> 1;
        const int offset = (2 + (prefix & 1)) << extra;
        return offset + static_cast<int>(b_.get(extra)) + 1;
    }

    // An entropy-coded picture: the main one (with its meta prefix codes) or one of a transform's.
    std::vector<uint32_t> image(int w, int h, bool main) {
        int cacheBits = 0;
        if (b_.get(1)) {
            cacheBits = static_cast<int>(b_.get(4));
            if (cacheBits < 1 || cacheBits > 11) fail("a colour cache of " + std::to_string(cacheBits) + " bits");
        }
        const int cacheSize = cacheBits > 0 ? 1 << cacheBits : 0;
        int prefixBits = 0;
        std::vector<uint32_t> groupsImage;
        int groupsWide = 0;
        size_t groupCount = 1;
        if (main && b_.get(1)) {
            prefixBits = static_cast<int>(b_.get(3)) + 2;
            groupsWide = subsampled(w, prefixBits);
            groupsImage = image(groupsWide, subsampled(h, prefixBits), false);
            uint32_t most = 0;
            for (uint32_t& g : groupsImage) {
                g = (g >> 8) & 0xFFFF;
                most = std::max(most, g);
            }
            groupCount = most + 1;
        }
        std::vector<Group> groups;
        groups.reserve(groupCount);
        for (size_t i = 0; i < groupCount; i++) groups.push_back(group(cacheSize));
        std::vector<uint32_t> cache(static_cast<size_t>(cacheSize), 0);
        const size_t total = static_cast<size_t>(w) * static_cast<size_t>(h);
        std::vector<uint32_t> out(total);
        auto remember = [&](uint32_t argb) {
            if (cacheBits > 0) cache[(0x1E35A7BDu * argb) >> (32 - cacheBits)] = argb;
        };
        size_t at = 0;
        while (at < total) {
            const int x = static_cast<int>(at % static_cast<size_t>(w)), y = static_cast<int>(at / static_cast<size_t>(w));
            const Group& g = groupsImage.empty() ? groups[0] : groups[groupsImage[static_cast<size_t>((y >> prefixBits) * groupsWide + (x >> prefixBits))]];
            const int s = g.codes[0].read(b_);
            if (s < 0 || b_.overrun()) fail(b_.overrun() ? "its lossless data ends early" : "bits that are no code");
            if (s < 256) {
                const uint32_t red = static_cast<uint32_t>(g.codes[1].read(b_)), blue = static_cast<uint32_t>(g.codes[2].read(b_)),
                               alpha = static_cast<uint32_t>(g.codes[3].read(b_));
                const uint32_t argb = (alpha << 24) | (red << 16) | (static_cast<uint32_t>(s) << 8) | blue;
                out[at++] = argb;
                remember(argb);
            } else if (s < 256 + 24) {
                const int length = lengthOrDistance(s - 256);
                const int code = g.codes[4].read(b_);
                if (code < 0) fail("bits that are no code");
                const int distanceCode = lengthOrDistance(code);
                int64_t distance;
                if (distanceCode > 120) {
                    distance = distanceCode - 120;
                } else {
                    distance = kPlane[distanceCode - 1][0] + static_cast<int64_t>(kPlane[distanceCode - 1][1]) * w;
                    if (distance < 1) distance = 1;
                }
                if (static_cast<size_t>(distance) > at) fail("a copy from before the picture's start");
                if (at + static_cast<size_t>(length) > total) fail("a copy past the picture's end");
                for (int k = 0; k < length; k++) {
                    out[at] = out[at - static_cast<size_t>(distance)];
                    remember(out[at]);
                    at++;
                }
            } else {
                const int key = s - 256 - 24;
                if (key >= cacheSize) fail("a colour cache index past the cache");
                out[at] = cache[static_cast<size_t>(key)];
                remember(out[at]);
                at++;
            }
        }
        return out;
    }

    static uint32_t average(uint32_t a, uint32_t b) { return (((a ^ b) & 0xFEFEFEFEu) >> 1) + (a & b); }
    static int channel(uint32_t v, int shift) { return static_cast<int>((v >> shift) & 0xFF); }
    static uint32_t clampAddSubtractFull(uint32_t a, uint32_t b, uint32_t c) {
        uint32_t out = 0;
        for (int s = 0; s < 32; s += 8) out |= static_cast<uint32_t>(std::clamp(channel(a, s) + channel(b, s) - channel(c, s), 0, 255)) << s;
        return out;
    }
    static uint32_t clampAddSubtractHalf(uint32_t a, uint32_t b) {
        uint32_t out = 0;
        for (int s = 0; s < 32; s += 8) {
            const int x = channel(a, s);
            out |= static_cast<uint32_t>(std::clamp(x + (x - channel(b, s)) / 2, 0, 255)) << s;
        }
        return out;
    }
    static uint32_t select(uint32_t l, uint32_t t, uint32_t tl) {
        int pl = 0, pt = 0;
        for (int s = 0; s < 32; s += 8) {
            const int p = channel(l, s) + channel(t, s) - channel(tl, s);
            pl += std::abs(p - channel(l, s));
            pt += std::abs(p - channel(t, s));
        }
        return pl < pt ? l : t;
    }

    static uint32_t predict(int mode, const uint32_t* px, size_t at, size_t w) {
        const uint32_t l = px[at - 1], t = px[at - w], tr = px[at - w + 1], tl = px[at - w - 1];
        switch (mode) {
            case 0: return 0xFF000000u;
            case 1: return l;
            case 2: return t;
            case 3: return tr;
            case 4: return tl;
            case 5: return average(average(l, tr), t);
            case 6: return average(l, tl);
            case 7: return average(l, t);
            case 8: return average(tl, t);
            case 9: return average(t, tr);
            case 10: return average(average(l, tl), average(t, tr));
            case 11: return select(l, t, tl);
            case 12: return clampAddSubtractFull(l, t, tl);
            case 13: return clampAddSubtractHalf(average(l, t), tl);
            default: return 0xFF000000u;  // 14 and 15: as 0, as libwebp has them
        }
    }

    static int8_t delta(uint32_t transform, uint32_t color) { return static_cast<int8_t>((static_cast<int8_t>(transform) * static_cast<int8_t>(color)) >> 5); }

    void undo(const Transform& t, std::vector<uint32_t>& px, int height) {
        const int w = t.width;
        if (t.kind == 2) {  // green added back to red and blue
            for (uint32_t& p : px) {
                const uint32_t g = (p >> 8) & 0xFF;
                p = (p & 0xFF00FF00u) | ((((p >> 16) & 0xFF) + g) & 0xFF) << 16 | (((p & 0xFF) + g) & 0xFF);
            }
            return;
        }
        if (t.kind == 3) {  // indices made colours, several to a pixel where they are few
            const int perPixel = 1 << t.bits, bitsEach = 8 >> t.bits, mask = (1 << bitsEach) - 1;
            const int packedWidth = subsampled(w, t.bits);
            std::vector<uint32_t> out(static_cast<size_t>(w) * static_cast<size_t>(height));
            for (int y = 0; y < height; y++)
                for (int x = 0; x < w; x++) {
                    const uint32_t packed = px[static_cast<size_t>(y * packedWidth + (x >> t.bits))];
                    const int index = static_cast<int>((packed >> 8) >> ((x & (perPixel - 1)) * bitsEach)) & mask;
                    out[static_cast<size_t>(y * w + x)] = t.data[static_cast<size_t>(index)];
                }
            px = std::move(out);
            return;
        }
        const int blocksWide = subsampled(w, t.bits);
        const size_t sw = static_cast<size_t>(w);
        for (int y = 0; y < height; y++)
            for (int x = 0; x < w; x++) {
                const size_t at = static_cast<size_t>(y) * sw + static_cast<size_t>(x);
                const uint32_t element = t.data[static_cast<size_t>((y >> t.bits) * blocksWide + (x >> t.bits))];
                if (t.kind == 0) {  // predictor
                    uint32_t prediction;
                    if (y == 0) prediction = x == 0 ? 0xFF000000u : px[at - 1];
                    else if (x == 0) prediction = px[at - sw];
                    else prediction = predict(static_cast<int>((element >> 8) & 0xF), px.data(), at, sw);
                    px[at] = addPixels(px[at], prediction);
                } else {  // colour transform
                    const uint32_t p = px[at];
                    const int green = static_cast<int8_t>((p >> 8) & 0xFF);
                    int red = static_cast<int>((p >> 16) & 0xFF), blue = static_cast<int>(p & 0xFF);
                    red += delta(element & 0xFF, static_cast<uint32_t>(green));          // green to red
                    blue += delta((element >> 8) & 0xFF, static_cast<uint32_t>(green));  // green to blue
                    blue += delta((element >> 16) & 0xFF, static_cast<uint32_t>(red & 0xFF));  // red to blue
                    px[at] = (p & 0xFF00FF00u) | (static_cast<uint32_t>(red & 0xFF) << 16) | static_cast<uint32_t>(blue & 0xFF);
                }
            }
    }
};


// ---- VP8L: writing ----

// Bits lowest first.
struct LBitsOut {
    std::vector<uint8_t> out;
    uint64_t buffer = 0;
    int count = 0;
    void put(uint32_t value, int n) {
        if (n == 0) return;
        buffer |= static_cast<uint64_t>(value & ((n == 32 ? 0xFFFFFFFFu : (1u << n) - 1))) << count;
        count += n;
        while (count >= 8) {
            out.push_back(static_cast<uint8_t>(buffer));
            buffer >>= 8;
            count -= 8;
        }
    }
    void finish() {
        if (count > 0) out.push_back(static_cast<uint8_t>(buffer));
        buffer = 0;
        count = 0;
    }
};

// Code lengths for counts, none longer than `most`: Huffman's, then the deepest shortened and
// others lengthened to make room, the rarest given the longest (as codec.cpp's for deflate).
std::vector<int> limitedLengths(const std::vector<uint32_t>& counts, int most) {
    const size_t n = counts.size();
    std::vector<int> lengths(n, 0);
    std::vector<std::pair<uint32_t, int>> used;
    for (size_t i = 0; i < n; i++)
        if (counts[i] != 0) used.push_back({counts[i], static_cast<int>(i)});
    if (used.empty()) return lengths;
    if (used.size() == 1) {
        lengths[static_cast<size_t>(used[0].second)] = 1;
        return lengths;
    }
    std::sort(used.begin(), used.end());
    const size_t m = used.size();
    std::vector<uint64_t> weight(2 * m);
    std::vector<int32_t> parent(2 * m, -1);
    for (size_t i = 0; i < m; i++) weight[i] = used[i].first;
    size_t leaf = 0, node = m, made = m;
    auto next = [&]() -> size_t {
        if (leaf < m && (node >= made || weight[leaf] <= weight[node])) return leaf++;
        return node++;
    };
    while (made < 2 * m - 1) {
        const size_t a = next(), b = next();
        weight[made] = weight[a] + weight[b];
        parent[a] = static_cast<int32_t>(made);
        parent[b] = static_cast<int32_t>(made);
        made++;
    }
    std::vector<int> depth(2 * m - 1, 0);
    for (size_t i = 2 * m - 2; i-- > 0;) depth[i] = depth[static_cast<size_t>(parent[i])] + 1;
    std::vector<int> perLength(64, 0);
    for (size_t i = 0; i < m; i++) perLength[static_cast<size_t>(std::min(depth[i], 63))]++;
    for (int l = most + 1; l < 64; l++) {
        perLength[static_cast<size_t>(most)] += perLength[static_cast<size_t>(l)];
        perLength[static_cast<size_t>(l)] = 0;
    }
    uint64_t total = 0;
    for (int l = 1; l <= most; l++) total += static_cast<uint64_t>(perLength[static_cast<size_t>(l)]) << (most - l);
    while (total > (uint64_t{1} << most)) {
        perLength[static_cast<size_t>(most)]--;
        for (int l = most - 1; l > 0; l--)
            if (perLength[static_cast<size_t>(l)] != 0) {
                perLength[static_cast<size_t>(l)]--;
                perLength[static_cast<size_t>(l + 1)] += 2;
                break;
            }
        total--;
    }
    size_t i = 0;
    for (int l = most; l > 0; l--)
        for (int k = 0; k < perLength[static_cast<size_t>(l)]; k++) lengths[static_cast<size_t>(used[i++].second)] = l;
    return lengths;
}

// Codes for lengths, as the decoder numbers them, each reversed, as they go out lowest bit first.
std::vector<uint32_t> codesFor(const std::vector<int>& lengths) {
    std::vector<uint32_t> codes(lengths.size(), 0);
    uint32_t perLength[16] = {}, next[16] = {};
    for (int l : lengths) perLength[l]++;
    perLength[0] = 0;
    uint32_t code = 0;
    for (int l = 1; l < 16; l++) {
        code = (code + perLength[l - 1]) << 1;
        next[l] = code;
    }
    for (size_t s = 0; s < lengths.size(); s++)
        if (lengths[s] > 0) codes[s] = reverse(next[lengths[s]]++, lengths[s]);
    return codes;
}

// A prefix code written: simply where it has one or two symbols below 256, else its lengths
// through the code length code, runs of them shortened (16 the length before, 17 and 18 zeros).
void writeCode(LBitsOut& w, const std::vector<int>& lengths) {
    std::vector<int> symbols;
    for (size_t s = 0; s < lengths.size(); s++)
        if (lengths[s] > 0) symbols.push_back(static_cast<int>(s));
    if (symbols.empty()) symbols.push_back(0);
    if (symbols.size() <= 2 && symbols.back() < 256) {
        w.put(1, 1);
        w.put(static_cast<uint32_t>(symbols.size() - 1), 1);
        if (symbols[0] < 2) {
            w.put(0, 1);
            w.put(static_cast<uint32_t>(symbols[0]), 1);
        } else {
            w.put(1, 1);
            w.put(static_cast<uint32_t>(symbols[0]), 8);
        }
        if (symbols.size() == 2) w.put(static_cast<uint32_t>(symbols[1]), 8);
        return;
    }
    w.put(0, 1);
    std::vector<std::pair<int, int>> tokens;  // a code length symbol and its extra bits
    for (size_t i = 0; i < lengths.size();) {
        const int value = lengths[i];
        size_t run = 1;
        while (i + run < lengths.size() && lengths[i + run] == value) run++;
        i += run;
        if (value == 0) {
            while (run >= 11) {
                const size_t take = std::min<size_t>(run, 138);
                tokens.push_back({18, static_cast<int>(take - 11)});
                run -= take;
            }
            if (run >= 3) {
                tokens.push_back({17, static_cast<int>(run - 3)});
                run = 0;
            }
        } else {
            tokens.push_back({value, 0});
            run--;
            while (run >= 3) {
                const size_t take = std::min<size_t>(run, 6);
                tokens.push_back({16, static_cast<int>(take - 3)});
                run -= take;
            }
        }
        while (run-- > 0) tokens.push_back({value, 0});
    }
    std::vector<uint32_t> counts(19, 0);
    for (const auto& t : tokens) counts[static_cast<size_t>(t.first)]++;
    const std::vector<int> lengthLengths = limitedLengths(counts, 7);
    int count = 19;
    while (count > 4 && lengthLengths[static_cast<size_t>(kCodeLengthOrder[count - 1])] == 0) count--;
    w.put(static_cast<uint32_t>(count - 4), 4);
    for (int i = 0; i < count; i++) w.put(static_cast<uint32_t>(lengthLengths[static_cast<size_t>(kCodeLengthOrder[i])]), 3);
    w.put(0, 1);  // all the alphabet's lengths are given
    // A code length code of one symbol takes no bits.
    int usedLengths = 0;
    for (int l : lengthLengths) usedLengths += l > 0 ? 1 : 0;
    const std::vector<uint32_t> lengthCodes = codesFor(lengthLengths);
    for (const auto& t : tokens) {
        if (usedLengths > 1) w.put(lengthCodes[static_cast<size_t>(t.first)], lengthLengths[static_cast<size_t>(t.first)]);
        if (t.first == 16) w.put(static_cast<uint32_t>(t.second), 2);
        else if (t.first == 17) w.put(static_cast<uint32_t>(t.second), 3);
        else if (t.first == 18) w.put(static_cast<uint32_t>(t.second), 7);
    }
}

// The prefix code of a length or a distance, and its extra bits.
void prefixOf(int value, int& code, int& extraBits, int& extra) {
    if (value <= 4) {
        code = value - 1;
        extraBits = 0;
        extra = 0;
        return;
    }
    const int v = value - 1;
    int highest = 31;
    while (((v >> highest) & 1) == 0) highest--;
    const int second = (v >> (highest - 1)) & 1;
    extraBits = highest - 1;
    extra = v & ((1 << extraBits) - 1);
    code = 2 * highest + second;
}

// An entropy-coded picture written: its colour cache, no meta codes (a single group), its five
// codes and its pixels, as literals, copies (LZ77 over pixels) or cache indices.
void writeImage(LBitsOut& w, const std::vector<uint32_t>& px, int width, bool main, int cacheBits) {
    if (cacheBits > 0) {
        w.put(1, 1);
        w.put(static_cast<uint32_t>(cacheBits), 4);
    } else {
        w.put(0, 1);
    }
    if (main) w.put(0, 1);  // no meta prefix codes
    // The symbols: literals, copies and cache hits, found greedily.
    struct Symbol {
        uint32_t argb;
        int kind;  // 0 literal, 1 copy, 2 cache
        int length, distance, cacheKey;
    };
    std::vector<Symbol> symbols;
    const int cacheSize = cacheBits > 0 ? 1 << cacheBits : 0;
    std::vector<uint32_t> cache(static_cast<size_t>(cacheSize), 0);
    std::vector<uint8_t> cacheSet(static_cast<size_t>(cacheSize), 0);
    auto remember = [&](uint32_t argb) {
        if (cacheBits == 0) return;
        const size_t k = (0x1E35A7BDu * argb) >> (32 - cacheBits);
        cache[k] = argb;
        cacheSet[k] = 1;
    };
    const size_t n = px.size();
    constexpr int kHashBits = 16;
    std::vector<int32_t> head(size_t{1} << kHashBits, -1), previous(n, -1);
    auto hashAt = [&](size_t i) { return static_cast<size_t>(((px[i] * 0x9E3779B1u) ^ (px[i + 1] * 0x85EBCA77u)) >> (32 - kHashBits)); };
    // The short distances of the plane code, by their offset, for the commonest small copies.
    auto distanceCode = [&](int64_t distance) -> int {
        const int64_t across = distance % width, down = distance / width;
        for (int c = 0; c < 120; c++) {
            const int64_t d = kPlane[c][0] + static_cast<int64_t>(kPlane[c][1]) * width;
            if (d == distance && (d >= 1)) return c + 1;
        }
        (void)across;
        (void)down;
        return static_cast<int>(distance) + 120;
    };
    for (size_t i = 0; i < n;) {
        int bestLength = 0;
        int64_t bestDistance = 0;
        if (i + 1 < n) {
            int chain = 64;
            for (int32_t at = head[hashAt(i)]; at >= 0 && chain-- > 0; at = previous[static_cast<size_t>(at)]) {
                const size_t from = static_cast<size_t>(at);
                if (i - from > (size_t{1} << 20) - 120) break;  // past the longest distance there is
                const size_t limit = std::min<size_t>(4096, n - i);
                size_t length = 0;
                while (length < limit && px[from + length] == px[i + length]) length++;
                if (static_cast<int>(length) > bestLength) {
                    bestLength = static_cast<int>(length);
                    bestDistance = static_cast<int64_t>(i - from);
                    if (length == limit) break;
                }
            }
        }
        if (bestLength >= 3) {
            symbols.push_back({0, 1, bestLength, static_cast<int>(distanceCode(bestDistance)), 0});
            for (int k = 0; k < bestLength; k++) {
                if (i + 1 < n) {
                    const size_t h = hashAt(i);
                    previous[i] = head[h];
                    head[h] = static_cast<int32_t>(i);
                }
                remember(px[i]);
                i++;
            }
            continue;
        }
        const uint32_t argb = px[i];
        if (cacheBits > 0) {
            const size_t k = (0x1E35A7BDu * argb) >> (32 - cacheBits);
            if (cacheSet[k] && cache[k] == argb) {
                symbols.push_back({argb, 2, 0, 0, static_cast<int>(k)});
            } else {
                symbols.push_back({argb, 0, 0, 0, 0});
            }
        } else {
            symbols.push_back({argb, 0, 0, 0, 0});
        }
        if (i + 1 < n) {
            const size_t h = hashAt(i);
            previous[i] = head[h];
            head[h] = static_cast<int32_t>(i);
        }
        remember(argb);
        i++;
    }
    // The five codes from the symbols' counts.
    std::vector<uint32_t> counts[5] = {std::vector<uint32_t>(static_cast<size_t>(256 + 24 + cacheSize), 0), std::vector<uint32_t>(256, 0),
                                       std::vector<uint32_t>(256, 0), std::vector<uint32_t>(256, 0), std::vector<uint32_t>(40, 0)};
    for (const Symbol& s : symbols) {
        if (s.kind == 0) {
            counts[0][(s.argb >> 8) & 0xFF]++;
            counts[1][(s.argb >> 16) & 0xFF]++;
            counts[2][s.argb & 0xFF]++;
            counts[3][s.argb >> 24]++;
        } else if (s.kind == 1) {
            int code, bits, extra;
            prefixOf(s.length, code, bits, extra);
            counts[0][static_cast<size_t>(256 + code)]++;
            prefixOf(s.distance, code, bits, extra);
            counts[4][static_cast<size_t>(code)]++;
        } else {
            counts[0][static_cast<size_t>(256 + 24 + s.cacheKey)]++;
        }
    }
    std::vector<int> lengths[5];
    std::vector<uint32_t> codes[5];
    for (int c = 0; c < 5; c++) {
        lengths[c] = limitedLengths(counts[c], 15);
        writeCode(w, lengths[c]);
        codes[c] = codesFor(lengths[c]);
    }
    // A code of one symbol takes no bits.
    bool single[5];
    for (int c = 0; c < 5; c++) {
        int used = 0;
        for (int l : lengths[c]) used += l > 0 ? 1 : 0;
        single[c] = used <= 1;
    }
    auto put = [&](int c, size_t symbol) {
        if (!single[c]) w.put(codes[c][symbol], lengths[c][symbol]);
    };
    for (const Symbol& s : symbols) {
        if (s.kind == 0) {
            put(0, (s.argb >> 8) & 0xFF);
            put(1, (s.argb >> 16) & 0xFF);
            put(2, s.argb & 0xFF);
            put(3, s.argb >> 24);
        } else if (s.kind == 1) {
            int code, bits, extra;
            prefixOf(s.length, code, bits, extra);
            put(0, static_cast<size_t>(256 + code));
            w.put(static_cast<uint32_t>(extra), bits);
            prefixOf(s.distance, code, bits, extra);
            put(4, static_cast<size_t>(code));
            w.put(static_cast<uint32_t>(extra), bits);
        } else {
            put(0, static_cast<size_t>(256 + 24 + s.cacheKey));
        }
    }
}

uint32_t subPixels(uint32_t a, uint32_t b) {
    const uint32_t ag = 0x00FF00FFu + (a & 0xFF00FF00u) - (b & 0xFF00FF00u), rb = 0xFF00FF00u + (a & 0x00FF00FFu) - (b & 0x00FF00FFu);
    return (ag & 0xFF00FF00u) | (rb & 0x00FF00FFu);
}

// Lossless WebP data (VP8L) of ARGB pixels: through a palette where there are 256 colours or fewer,
// else subtract-green and a predictor chosen for each 16 by 16 block.
std::vector<uint8_t> encodeLossless(const std::vector<uint32_t>& argb, int width, int height, bool headerless) {
    LBitsOut w;
    bool alpha = false;
    for (uint32_t p : argb)
        if ((p >> 24) != 0xFF) {
            alpha = true;
            break;
        }
    if (!headerless) {
        w.put(0x2F, 8);
        w.put(static_cast<uint32_t>(width - 1), 14);
        w.put(static_cast<uint32_t>(height - 1), 14);
        w.put(alpha ? 1 : 0, 1);
        w.put(0, 3);
    }
    // The colours, if there are no more than 256.
    std::vector<uint32_t> palette;
    {
        std::vector<uint32_t> sorted(argb);
        std::sort(sorted.begin(), sorted.end());
        sorted.erase(std::unique(sorted.begin(), sorted.end()), sorted.end());
        if (sorted.size() <= 256) palette = sorted;
    }
    std::vector<uint32_t> px;
    int xsize = width;
    if (!palette.empty()) {
        w.put(1, 1);
        w.put(3, 2);
        w.put(static_cast<uint32_t>(palette.size() - 1), 8);
        std::vector<uint32_t> deltas(palette.size());
        for (size_t i = 0; i < palette.size(); i++) deltas[i] = i == 0 ? palette[0] : subPixels(palette[i], palette[i - 1]);
        writeImage(w, deltas, static_cast<int>(palette.size()), false, 0);
        const int bits = palette.size() <= 2 ? 3 : palette.size() <= 4 ? 2 : palette.size() <= 16 ? 1 : 0;
        const int perPixel = 1 << bits, bitsEach = 8 >> bits;
        xsize = (width + perPixel - 1) >> bits;
        px.assign(static_cast<size_t>(xsize) * static_cast<size_t>(height), 0xFF000000u);
        for (int y = 0; y < height; y++)
            for (int x = 0; x < width; x++) {
                const uint32_t c = argb[static_cast<size_t>(y) * static_cast<size_t>(width) + static_cast<size_t>(x)];
                const uint32_t index = static_cast<uint32_t>(std::lower_bound(palette.begin(), palette.end(), c) - palette.begin());
                px[static_cast<size_t>(y * xsize + (x >> bits))] |= (index << ((x & (perPixel - 1)) * bitsEach)) << 8;
            }
    } else {
        // Subtract green.
        px = argb;
        for (uint32_t& p : px) {
            const uint32_t g = (p >> 8) & 0xFF;
            p = (p & 0xFF00FF00u) | ((((p >> 16) & 0xFF) - g) & 0xFF) << 16 | (((p & 0xFF) - g) & 0xFF);
        }
        w.put(1, 1);
        w.put(2, 2);
        // The predictor: for each block the mode whose leftovers are smallest, tried on every other pixel.
        const int bits = 4;
        const int bw = (width + 15) >> bits, bh = (height + 15) >> bits;
        std::vector<uint32_t> modes(static_cast<size_t>(bw) * static_cast<size_t>(bh));
        const size_t sw = static_cast<size_t>(width);
        auto cost = [](uint32_t residual) {
            int c = 0;
            for (int s = 0; s < 32; s += 8) {
                const int v = static_cast<int>((residual >> s) & 0xFF);
                c += v < 128 ? v : 256 - v;
            }
            return c;
        };
        for (int by = 0; by < bh; by++)
            for (int bx = 0; bx < bw; bx++) {
                int best = 11;
                int64_t bestCost = INT64_MAX;
                for (int mode = 0; mode < 14; mode++) {
                    int64_t total = 0;
                    for (int y = std::max(1, by * 16); y < std::min(height, by * 16 + 16); y += 2)
                        for (int x = std::max(1, bx * 16); x < std::min(width, bx * 16 + 16); x += 2) {
                            const size_t at = static_cast<size_t>(y) * sw + static_cast<size_t>(x);
                            total += cost(subPixels(px[at], Lossless::predictFor(mode, px.data(), at, sw)));
                        }
                    if (total < bestCost) {
                        bestCost = total;
                        best = mode;
                    }
                }
                modes[static_cast<size_t>(by * bw + bx)] = 0xFF000000u | (static_cast<uint32_t>(best) << 8);
            }
        std::vector<uint32_t> residuals(px.size());
        for (int y = 0; y < height; y++)
            for (int x = 0; x < width; x++) {
                const size_t at = static_cast<size_t>(y) * sw + static_cast<size_t>(x);
                uint32_t prediction;
                if (y == 0) prediction = x == 0 ? 0xFF000000u : px[at - 1];
                else if (x == 0) prediction = px[at - sw];
                else prediction = Lossless::predictFor(static_cast<int>((modes[static_cast<size_t>((y >> bits) * bw + (x >> bits))] >> 8) & 0xF), px.data(), at, sw);
                residuals[at] = subPixels(px[at], prediction);
            }
        w.put(1, 1);
        w.put(0, 2);
        w.put(bits - 2, 3);
        writeImage(w, modes, bw, false, 0);
        px = std::move(residuals);
    }
    w.put(0, 1);  // no more transforms
    writeImage(w, px, xsize, true, palette.empty() ? 10 : 0);
    w.finish();
    return std::move(w.out);
}

// ARGB pixels as RGBA bytes.
std::vector<uint8_t> rgbaOf(const std::vector<uint32_t>& argb) {
    std::vector<uint8_t> out(argb.size() * 4);
    for (size_t i = 0; i < argb.size(); i++) {
        const uint32_t p = argb[i];
        out[i * 4] = static_cast<uint8_t>(p >> 16);
        out[i * 4 + 1] = static_cast<uint8_t>(p >> 8);
        out[i * 4 + 2] = static_cast<uint8_t>(p);
        out[i * 4 + 3] = static_cast<uint8_t>(p >> 24);
    }
    return out;
}

Value bytesValue(VM& vm, std::vector<uint8_t>&& data) {
    vm.checkRoomFor(data.size());
    ObjBytes* made = vm.alloc<ObjBytes>();
    made->data = std::move(data);
    vm.account(made->data.size());
    return Value::object(made);
}

}  // namespace

void registerWebp(VM& vm) {
    ObjClass* holder = vm.alloc<ObjClass>(vm.intern("Webp"));
    auto def = [&](const char* name, int minArgs, int maxArgs, NativeFn fn) {
        holder->statics.set(vm.intern(name), Value::object(vm.alloc<ObjNative>(std::string("Webp.") + name, minArgs, maxArgs, fn)));
    };
    // decodeLossless(data, from, to, width, height): [width, height, RGBA bytes] of a VP8L
    // bitstream, or a string, what is wrong. A width and height given (more than 0) mean ALPH's
    // headerless data of that size, whose green is the alpha.
    def("decodeLossless", 5, 5, [](VM& vm, int, Value* args) -> Value {
        if (!args[0].isObjType(ObjType::Bytes)) throw vm.runtimeError(Thrown::IllegalArgumentException, "WebP's data is Bytes");
        const std::vector<uint8_t>& data = asBytes(args[0])->data;
        const int64_t from = vm.toInt(args[1], "from"), to = vm.toInt(args[2], "to");
        if (from < 0 || to < from || to > static_cast<int64_t>(data.size())) throw vm.runtimeError(Thrown::IndexOutOfBoundsException, "a part outside the data");
        int width = static_cast<int>(vm.toInt(args[3], "width")), height = static_cast<int>(vm.toInt(args[4], "height"));
        const bool headerless = width > 0 && height > 0;
        std::vector<uint32_t> argb;
        try {
            Lossless(data.data() + from, static_cast<size_t>(to - from)).read(argb, width, height, headerless);
        } catch (const LosslessError& e) {
            return vm.str(e.what);
        }
        const Value pixels = bytesValue(vm, rgbaOf(argb));
        TempRoot keep(vm, pixels);
        return Value::object(newList(vm, {Value::integer(width), Value::integer(height), pixels}));
    });
    // encodeLossless(rgba, width, height, headerless): VP8L data of RGBA pixels; headerless for
    // ALPH's alpha, which has no signature or size of its own.
    def("encodeLossless", 4, 4, [](VM& vm, int, Value* args) -> Value {
        if (!args[0].isObjType(ObjType::Bytes)) throw vm.runtimeError(Thrown::IllegalArgumentException, "pixels are Bytes");
        const std::vector<uint8_t>& rgba = asBytes(args[0])->data;
        const int64_t width = vm.toInt(args[1], "width"), height = vm.toInt(args[2], "height");
        if (width < 1 || height < 1 || width > 16384 || height > 16384) throw vm.runtimeError(Thrown::IllegalArgumentException, "a WebP is 1 to 16384 pixels each way, not " + std::to_string(width) + " by " + std::to_string(height));
        if (rgba.size() != static_cast<size_t>(width * height * 4)) throw vm.runtimeError(Thrown::IllegalArgumentException, "the pixels are not " + std::to_string(width) + " by " + std::to_string(height));
        std::vector<uint32_t> argb(static_cast<size_t>(width * height));
        for (size_t i = 0; i < argb.size(); i++)
            argb[i] = (static_cast<uint32_t>(rgba[i * 4 + 3]) << 24) | (static_cast<uint32_t>(rgba[i * 4]) << 16) | (static_cast<uint32_t>(rgba[i * 4 + 1]) << 8) | rgba[i * 4 + 2];
        return bytesValue(vm, encodeLossless(argb, static_cast<int>(width), static_cast<int>(height), args[3].truthy()));
    });
    vm.defineGlobal("__webp", Value::object(holder));
}

}  // namespace jade
