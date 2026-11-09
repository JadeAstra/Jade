// What general picture operations (src/std/picture.jade) are built on, over a picture's pixels as
// kept in a Grid<Color> or an Image (four bytes each: red, green, blue, alpha): the colours that
// stand best for a picture, and the picture put into them, dithered or not (M9/M10 part one).

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "builtins.h"
#include "vm.h"

// SSE2, which every x64 processor has, for the loops over every sample; each has a plain twin
// that gives the same numbers, for other processors.
#if (defined(__SSE2__) || defined(_M_X64) || (defined(_M_IX86_FP) && _M_IX86_FP >= 2)) && !defined(JADE_NO_SSE2)
#define JADE_SSE2 1
#include <emmintrin.h>
#endif

namespace jade {

namespace {

// A picture's pixels: a packed list of colours, four bytes each.
const std::vector<uint8_t>& pixelsOf(VM& vm, const Value& v) {
    if (v.isObjType(ObjType::Bytes)) return asBytes(v)->data;
    if (!v.isObjType(ObjType::List) || !asList(v)->packed() || asList(v)->pack->width != 4)
        throw vm.runtimeError(Thrown::IllegalArgumentException, "expected a picture's pixels");
    return asList(v)->pack->block;
}

Value bytesValue(VM& vm, std::vector<uint8_t>&& data) {
    vm.checkRoomFor(data.size());
    ObjBytes* made = vm.alloc<ObjBytes>();
    made->data = std::move(data);
    vm.account(made->data.size());
    return Value::object(made);
}

constexpr uint8_t kSeeThrough = 128;  // a pixel of less alpha is left see-through

// ---- The colours that stand best: an octree ----

// Up to `count` colours for the pixels that are not see-through: the colours themselves where
// there are no more than that, or else an octree's, over the pixels' colours to 5 bits a channel,
// the branches with fewest pixels joined until no more are left than `count`.
std::vector<uint8_t> paletteOf(const std::vector<uint8_t>& px, size_t count) {
    std::vector<uint8_t> palette;
    // Few enough colours: those, in the order first met.
    {
        std::unordered_set<uint32_t> seen;
        uint32_t last = 0xFFFFFFFFu;
        bool few = true;
        for (size_t i = 0; i + 3 < px.size(); i += 4) {
            if (px[i + 3] < kSeeThrough) continue;
            const uint32_t c = (static_cast<uint32_t>(px[i]) << 16) | (static_cast<uint32_t>(px[i + 1]) << 8) | px[i + 2];
            if (c == last) continue;
            last = c;
            if (seen.insert(c).second) {
                if (seen.size() > count) {
                    few = false;
                    break;
                }
                palette.insert(palette.end(), {px[i], px[i + 1], px[i + 2], 255});
            }
        }
        if (few) return palette;
        palette.clear();
    }
    // How many pixels have each colour, to 5 bits a channel, with their exact sums.
    struct Bin {
        uint64_t r = 0, g = 0, b = 0, n = 0;
    };
    std::vector<Bin> bins(32768);
    for (size_t i = 0; i + 3 < px.size(); i += 4) {
        if (px[i + 3] < kSeeThrough) continue;
        Bin& bin = bins[(static_cast<size_t>(px[i] >> 3) << 10) | (static_cast<size_t>(px[i + 1] >> 3) << 5) | (px[i + 2] >> 3)];
        bin.r += px[i];
        bin.g += px[i + 1];
        bin.b += px[i + 2];
        bin.n++;
    }
    struct Node {
        uint64_t r = 0, g = 0, b = 0, n = 0;
        int32_t children[8] = {-1, -1, -1, -1, -1, -1, -1, -1};
        int level = 0;
        bool leaf = false;
        bool joined = false;
    };
    std::vector<Node> nodes(1);
    size_t leaves = 0;
    for (size_t key = 0; key < bins.size(); key++) {
        const Bin& bin = bins[key];
        if (bin.n == 0) continue;
        const int r = static_cast<int>(key >> 10), g = static_cast<int>((key >> 5) & 31), b = static_cast<int>(key & 31);
        size_t at = 0;
        for (int level = 0; level < 5; level++) {
            nodes[at].n += bin.n;
            const int bit = 4 - level;
            const int child = (((r >> bit) & 1) << 2) | (((g >> bit) & 1) << 1) | ((b >> bit) & 1);
            if (nodes[at].children[child] < 0) {
                nodes[at].children[child] = static_cast<int32_t>(nodes.size());
                Node made;
                made.level = level + 1;
                nodes.push_back(made);
            }
            at = static_cast<size_t>(nodes[at].children[child]);
        }
        Node& leaf = nodes[at];
        leaf.r += bin.r;
        leaf.g += bin.g;
        leaf.b += bin.b;
        leaf.n += bin.n;
        leaf.leaf = true;
        leaves++;
    }
    // Join the branch of fewest pixels at the deepest level that has one, until few enough.
    std::array<std::vector<size_t>, 5> branches;
    for (size_t i = 0; i < nodes.size(); i++)
        if (!nodes[i].leaf && nodes[i].level < 5) branches[static_cast<size_t>(nodes[i].level)].push_back(i);
    for (int level = 4; level >= 0 && leaves > count; level--) {
        std::vector<size_t>& here = branches[static_cast<size_t>(level)];
        std::sort(here.begin(), here.end(), [&](size_t a, size_t b) { return nodes[a].n < nodes[b].n; });
        for (size_t k = 0; k < here.size() && leaves > count; k++) {
            Node& node = nodes[here[k]];
            int children = 0;
            for (int32_t child : node.children) {
                if (child < 0) continue;
                const Node& c = nodes[static_cast<size_t>(child)];
                node.r += c.r;
                node.g += c.g;
                node.b += c.b;
                nodes[static_cast<size_t>(child)].joined = true;
                children++;
            }
            node.leaf = true;
            leaves -= static_cast<size_t>(children) - 1;
        }
    }
    for (const Node& node : nodes) {
        if (!node.leaf || node.joined || node.n == 0) continue;
        palette.insert(palette.end(), {static_cast<uint8_t>((node.r + node.n / 2) / node.n), static_cast<uint8_t>((node.g + node.n / 2) / node.n),
                                       static_cast<uint8_t>((node.b + node.n / 2) / node.n), 255});
    }
    return palette;
}

// ---- The picture put into a palette ----

// The nearest of a palette's colours to any colour: a colour of the palette itself is its own
// index; any other the nearest to the middle of its cell of colours, 6 bits a channel, found once
// for each cell that comes up and kept.
class Nearest {
public:
    explicit Nearest(const std::vector<uint8_t>& palette) : palette_(palette), cells_(size_t{1} << 18, -1) {
        for (size_t i = 0; i * 4 < palette.size(); i++) exact_.emplace(colorKey(palette[i * 4], palette[i * 4 + 1], palette[i * 4 + 2]), static_cast<int>(i));
    }

    int of(int r, int g, int b) {
        if (auto found = exact_.find(colorKey(r, g, b)); found != exact_.end()) return found->second;
        const size_t key = (static_cast<size_t>(r >> 2) << 12) | (static_cast<size_t>(g >> 2) << 6) | static_cast<size_t>(b >> 2);
        int16_t& cell = cells_[key];
        if (cell < 0) cell = static_cast<int16_t>(search((r & ~3) | 2, (g & ~3) | 2, (b & ~3) | 2));
        return cell;
    }

private:
    const std::vector<uint8_t>& palette_;
    std::vector<int16_t> cells_;
    std::unordered_map<uint32_t, int> exact_;

    static uint32_t colorKey(int r, int g, int b) { return (static_cast<uint32_t>(r) << 16) | (static_cast<uint32_t>(g) << 8) | static_cast<uint32_t>(b); }

    // The nearest by distance, the channels weighted as the eye sees them.
    int search(int r, int g, int b) const {
        int best = 0;
        int64_t bestDistance = INT64_MAX;
        for (size_t i = 0; i * 4 < palette_.size(); i++) {
            const int dr = r - palette_[i * 4], dg = g - palette_[i * 4 + 1], db = b - palette_[i * 4 + 2];
            const int64_t d = 2 * static_cast<int64_t>(dr) * dr + 4 * static_cast<int64_t>(dg) * dg + 3 * static_cast<int64_t>(db) * db;
            if (d < bestDistance) {
                bestDistance = d;
                best = static_cast<int>(i);
            }
        }
        return best;
    }
};

// Each pixel's index in the palette, dithered by `method` (0 none, 1 Floyd and Steinberg, 2
// Atkinson); a see-through pixel's index is the palette's size.
std::vector<uint8_t> indicesOf(const std::vector<uint8_t>& px, int width, int height, const std::vector<uint8_t>& palette, int method) {
    const size_t w = static_cast<size_t>(width), h = static_cast<size_t>(height);
    std::vector<uint8_t> out(w * h);
    const uint8_t clear = static_cast<uint8_t>(palette.size() / 4);
    Nearest nearest(palette);
    if (method == 0) {
        for (size_t i = 0; i < w * h; i++) {
            const uint8_t* p = &px[i * 4];
            out[i] = p[3] < kSeeThrough ? clear : static_cast<uint8_t>(nearest.of(p[0], p[1], p[2]));
        }
        return out;
    }
    // The error each pixel's colour is off by, spread to those after it: three rows of it, for
    // Atkinson's reach of two rows down.
    std::vector<int32_t> error(3 * (w + 4) * 3, 0);
    auto at = [&](size_t row, size_t x) -> int32_t* { return &error[((row % 3) * (w + 4) + x + 2) * 3]; };
    for (size_t y = 0; y < h; y++) {
        std::fill(error.begin() + static_cast<std::ptrdiff_t>(((y + 2) % 3) * (w + 4) * 3), error.begin() + static_cast<std::ptrdiff_t>((((y + 2) % 3) + 1) * (w + 4) * 3), 0);
        for (size_t x = 0; x < w; x++) {
            const uint8_t* p = &px[(y * w + x) * 4];
            if (p[3] < kSeeThrough) {
                out[y * w + x] = clear;
                continue;
            }
            int32_t* e = at(y, x);
            // The error is kept in sixteenths (Floyd and Steinberg) or eighths (Atkinson).
            const int32_t scale = method == 1 ? 16 : 8;
            const int r = std::clamp(p[0] + e[0] / scale, 0, 255), g = std::clamp(p[1] + e[1] / scale, 0, 255), b = std::clamp(p[2] + e[2] / scale, 0, 255);
            const int index = nearest.of(r, g, b);
            out[y * w + x] = static_cast<uint8_t>(index);
            const int32_t er = r - palette[static_cast<size_t>(index) * 4], eg = g - palette[static_cast<size_t>(index) * 4 + 1],
                          eb = b - palette[static_cast<size_t>(index) * 4 + 2];
            auto spread = [&](size_t row, int dx, int32_t parts) {
                const int64_t nx = static_cast<int64_t>(x) + dx;
                if (nx < 0 || nx >= static_cast<int64_t>(w) || row >= h) return;
                int32_t* to = at(row, static_cast<size_t>(nx));
                to[0] += er * parts;
                to[1] += eg * parts;
                to[2] += eb * parts;
            };
            if (method == 1) {
                spread(y, 1, 7);
                spread(y + 1, -1, 3);
                spread(y + 1, 0, 5);
                spread(y + 1, 1, 1);
            } else {
                spread(y, 1, 1);
                spread(y, 2, 1);
                spread(y + 1, -1, 1);
                spread(y + 1, 0, 1);
                spread(y + 1, 1, 1);
                spread(y + 2, 0, 1);
            }
        }
        std::fill(error.begin() + static_cast<std::ptrdiff_t>((y % 3) * (w + 4) * 3), error.begin() + static_cast<std::ptrdiff_t>(((y % 3) + 1) * (w + 4) * 3), 0);
    }
    return out;
}


// ---- The DCT of 8 by 8 blocks ----

inline uint8_t clampSample(int v) { return static_cast<uint8_t>(v < 0 ? 0 : v > 255 ? 255 : v); }

// The inverse DCT of 8 by 8 blocks: libjpeg's slow integer method, in 4096ths, as stb_image has it,
// each output written as sums of pairs of inputs times constants (the same integers as its
// products of sums), so that SSE2 does a pair at a time. A pass down the columns keeps 2 more bits
// (shift 10), saturated to 16 bits; a pass along the rows adds 128 and rounds (shift 17).
// The constants: even part t0, t1 from (s0, s4); t3, t2 from (s2, s6); odd part from (s1, s3) and
// (s5, s7) for each of o3, o2, o1, o0.
constexpr int kIdctPass1Bias = 512, kIdctPass2Bias = 65536 + (128 << 17);

inline int16_t saturate16(int v) { return static_cast<int16_t>(v < -32768 ? -32768 : v > 32767 ? 32767 : v); }

// One pass over 8 lines of 8 values, `step` apart within a line and `lineStep` between lines.
inline void inverseEight(const int16_t* in, int step, int16_t* out, int bias, int shift) {
    const int s0 = in[0], s1 = in[step], s2 = in[2 * step], s3 = in[3 * step], s4 = in[4 * step], s5 = in[5 * step], s6 = in[6 * step], s7 = in[7 * step];
    if (s1 == 0 && s2 == 0 && s3 == 0 && s4 == 0 && s5 == 0 && s6 == 0 && s7 == 0) {
        const int16_t v = saturate16((s0 * 4096 + bias) >> shift);
        for (int k = 0; k < 8; k++) out[k * step] = v;
        return;
    }
    const int t0 = s0 * 4096 + s4 * 4096, t1 = s0 * 4096 - s4 * 4096;
    const int t3 = s2 * 5352 + s6 * 2217, t2 = s2 * 2217 + s6 * -5350;
    const int x0 = t0 + t3 + bias, x3 = t0 - t3 + bias, x1 = t1 + t2 + bias, x2 = t1 - t2 + bias;
    const int o3 = s1 * 5683 + s3 * 4816 + s5 * 3219 + s7 * 1131;
    const int o2 = s1 * 4816 + s3 * -1129 + s5 * -5681 + s7 * -3218;
    const int o1 = s1 * 3219 + s3 * -5681 + s5 * 1132 + s7 * 4816;
    const int o0 = s1 * 1131 + s3 * -3218 + s5 * 4816 + s7 * -5680;
    out[0] = saturate16((x0 + o3) >> shift);
    out[7 * step] = saturate16((x0 - o3) >> shift);
    out[step] = saturate16((x1 + o2) >> shift);
    out[6 * step] = saturate16((x1 - o2) >> shift);
    out[2 * step] = saturate16((x2 + o1) >> shift);
    out[5 * step] = saturate16((x2 - o1) >> shift);
    out[3 * step] = saturate16((x3 + o0) >> shift);
    out[4 * step] = saturate16((x3 - o0) >> shift);
}

// One block's coefficients (natural order), each multiplied by its step in 16 bits, to samples.
[[maybe_unused]] void inverseBlock(const int16_t* coefs, const int16_t* steps, uint8_t* out, size_t stride) {
    int16_t b[64], c[64];
    for (int i = 0; i < 64; i++) b[i] = static_cast<int16_t>(coefs[i] * steps[i]);
    for (int x = 0; x < 8; x++) inverseEight(b + x, 8, c + x, kIdctPass1Bias, 10);
    for (int y = 0; y < 8; y++) inverseEight(c + y * 8, 1, b + y * 8, kIdctPass2Bias, 17);
    for (int y = 0; y < 8; y++)
        for (int x = 0; x < 8; x++) out[static_cast<size_t>(y) * stride + static_cast<size_t>(x)] = clampSample(b[y * 8 + x]);
}

#ifdef JADE_SSE2
// Eight rows of 8 16-bit values turned about the diagonal.
inline void transpose8x16(__m128i (&r)[8]) {
    const __m128i a0 = _mm_unpacklo_epi16(r[0], r[1]), a1 = _mm_unpackhi_epi16(r[0], r[1]);
    const __m128i a2 = _mm_unpacklo_epi16(r[2], r[3]), a3 = _mm_unpackhi_epi16(r[2], r[3]);
    const __m128i a4 = _mm_unpacklo_epi16(r[4], r[5]), a5 = _mm_unpackhi_epi16(r[4], r[5]);
    const __m128i a6 = _mm_unpacklo_epi16(r[6], r[7]), a7 = _mm_unpackhi_epi16(r[6], r[7]);
    const __m128i b0 = _mm_unpacklo_epi32(a0, a2), b1 = _mm_unpackhi_epi32(a0, a2);
    const __m128i b2 = _mm_unpacklo_epi32(a1, a3), b3 = _mm_unpackhi_epi32(a1, a3);
    const __m128i b4 = _mm_unpacklo_epi32(a4, a6), b5 = _mm_unpackhi_epi32(a4, a6);
    const __m128i b6 = _mm_unpacklo_epi32(a5, a7), b7 = _mm_unpackhi_epi32(a5, a7);
    r[0] = _mm_unpacklo_epi64(b0, b4), r[1] = _mm_unpackhi_epi64(b0, b4);
    r[2] = _mm_unpacklo_epi64(b1, b5), r[3] = _mm_unpackhi_epi64(b1, b5);
    r[4] = _mm_unpacklo_epi64(b2, b6), r[5] = _mm_unpackhi_epi64(b2, b6);
    r[6] = _mm_unpacklo_epi64(b3, b7), r[7] = _mm_unpackhi_epi64(b3, b7);
}

inline __m128i pairOf(int a, int b) { return _mm_set1_epi32(static_cast<int>((static_cast<uint32_t>(b) << 16) | (static_cast<uint32_t>(a) & 0xFFFF))); }

// inverseEight on 8 lines at once, a line in each 16-bit lane: r[k] holds every line's s_k.
template <int shift>
inline void inverseEightSse(__m128i (&r)[8], __m128i bias) {
    const __m128i k04p = pairOf(4096, 4096), k04m = pairOf(4096, -4096), k26a = pairOf(5352, 2217), k26b = pairOf(2217, -5350);
    const __m128i o3a = pairOf(5683, 4816), o3b = pairOf(3219, 1131), o2a = pairOf(4816, -1129), o2b = pairOf(-5681, -3218);
    const __m128i o1a = pairOf(3219, -5681), o1b = pairOf(1132, 4816), o0a = pairOf(1131, -3218), o0b = pairOf(4816, -5680);
    __m128i res[8][2];
    for (int h = 0; h < 2; h++) {
        auto mix = [&](__m128i a, __m128i b) { return h == 0 ? _mm_unpacklo_epi16(a, b) : _mm_unpackhi_epi16(a, b); };
        const __m128i p04 = mix(r[0], r[4]), p26 = mix(r[2], r[6]), p13 = mix(r[1], r[3]), p57 = mix(r[5], r[7]);
        const __m128i t0 = _mm_madd_epi16(p04, k04p), t1 = _mm_madd_epi16(p04, k04m);
        const __m128i t3 = _mm_madd_epi16(p26, k26a), t2 = _mm_madd_epi16(p26, k26b);
        const __m128i x0 = _mm_add_epi32(_mm_add_epi32(t0, t3), bias), x3 = _mm_add_epi32(_mm_sub_epi32(t0, t3), bias);
        const __m128i x1 = _mm_add_epi32(_mm_add_epi32(t1, t2), bias), x2 = _mm_add_epi32(_mm_sub_epi32(t1, t2), bias);
        const __m128i o3 = _mm_add_epi32(_mm_madd_epi16(p13, o3a), _mm_madd_epi16(p57, o3b));
        const __m128i o2 = _mm_add_epi32(_mm_madd_epi16(p13, o2a), _mm_madd_epi16(p57, o2b));
        const __m128i o1 = _mm_add_epi32(_mm_madd_epi16(p13, o1a), _mm_madd_epi16(p57, o1b));
        const __m128i o0 = _mm_add_epi32(_mm_madd_epi16(p13, o0a), _mm_madd_epi16(p57, o0b));
        res[0][h] = _mm_srai_epi32(_mm_add_epi32(x0, o3), shift);
        res[7][h] = _mm_srai_epi32(_mm_sub_epi32(x0, o3), shift);
        res[1][h] = _mm_srai_epi32(_mm_add_epi32(x1, o2), shift);
        res[6][h] = _mm_srai_epi32(_mm_sub_epi32(x1, o2), shift);
        res[2][h] = _mm_srai_epi32(_mm_add_epi32(x2, o1), shift);
        res[5][h] = _mm_srai_epi32(_mm_sub_epi32(x2, o1), shift);
        res[3][h] = _mm_srai_epi32(_mm_add_epi32(x3, o0), shift);
        res[4][h] = _mm_srai_epi32(_mm_sub_epi32(x3, o0), shift);
    }
    for (int k = 0; k < 8; k++) r[k] = _mm_packs_epi32(res[k][0], res[k][1]);
}

void inverseBlockSse(const int16_t* coefs, const int16_t* steps, uint8_t* out, size_t stride) {
    __m128i r[8];
    for (int k = 0; k < 8; k++)
        r[k] = _mm_mullo_epi16(_mm_loadu_si128(reinterpret_cast<const __m128i*>(coefs + k * 8)), _mm_loadu_si128(reinterpret_cast<const __m128i*>(steps + k * 8)));
    // Down the columns: the rows are the registers, the columns their lanes.
    inverseEightSse<10>(r, _mm_set1_epi32(kIdctPass1Bias));
    transpose8x16(r);
    inverseEightSse<17>(r, _mm_set1_epi32(kIdctPass2Bias));
    transpose8x16(r);
    for (int y = 0; y < 8; y++) _mm_storel_epi64(reinterpret_cast<__m128i*>(out + static_cast<size_t>(y) * stride), _mm_packus_epi16(r[y], r[y]));
}
#endif

// The forward DCT along 8 values, AAN's way (libjpeg's float method), in place; `V` is a float or
// four of them in an SSE register, so that the plain and the SSE2 DCT do the same sums in order.
template <class V>
inline void forwardEight(V& e0, V& e1, V& e2, V& e3, V& e4, V& e5, V& e6, V& e7, V c707, V c382, V c541, V c1306) {
    const V t0 = e0 + e7, t7 = e0 - e7, t1 = e1 + e6, t6 = e1 - e6;
    const V t2 = e2 + e5, t5 = e2 - e5, t3 = e3 + e4, t4 = e3 - e4;
    const V t10 = t0 + t3, t13 = t0 - t3, t11 = t1 + t2, t12 = t1 - t2;
    e0 = t10 + t11;
    e4 = t10 - t11;
    const V z1 = (t12 + t13) * c707;
    e2 = t13 + z1;
    e6 = t13 - z1;
    const V u10 = t4 + t5, u11 = t5 + t6, u12 = t6 + t7;
    const V z5 = (u10 - u12) * c382;
    const V z2 = c541 * u10 + z5, z4 = c1306 * u12 + z5, z3 = u11 * c707;
    const V z11 = t7 + z3, z13 = t7 - z3;
    e5 = z13 + z2;
    e3 = z13 - z2;
    e1 = z11 + z4;
    e7 = z11 - z4;
}
constexpr float kC707 = 0.707106781f, kC382 = 0.382683433f, kC541 = 0.541196100f, kC1306 = 1.306562965f;

// The forward DCT of 8 by 8 samples (128 taken off): down the columns, then along the rows;
// each coefficient left scaled by `aan`, which the caller takes out.
[[maybe_unused]] void forwardBlock(float* b) {
    for (int x = 0; x < 8; x++)
        forwardEight(b[x], b[x + 8], b[x + 16], b[x + 24], b[x + 32], b[x + 40], b[x + 48], b[x + 56], kC707, kC382, kC541, kC1306);
    for (int y = 0; y < 8; y++) {
        float* r = b + y * 8;
        forwardEight(r[0], r[1], r[2], r[3], r[4], r[5], r[6], r[7], kC707, kC382, kC541, kC1306);
    }
}

#ifdef JADE_SSE2
// Four floats in a register, with the operators forwardEight uses.
struct F4 {
    __m128 v;
    F4() = default;
    F4(__m128 x) : v(x) {}
    friend F4 operator+(F4 a, F4 b) { return _mm_add_ps(a.v, b.v); }
    friend F4 operator-(F4 a, F4 b) { return _mm_sub_ps(a.v, b.v); }
    friend F4 operator*(F4 a, F4 b) { return _mm_mul_ps(a.v, b.v); }
};

// An 8 by 8 block of floats as rows of two registers, turned about its diagonal.
inline void transpose8(F4 (&r)[8][2]) {
    __m128 a0 = r[0][0].v, a1 = r[1][0].v, a2 = r[2][0].v, a3 = r[3][0].v;
    __m128 b0 = r[0][1].v, b1 = r[1][1].v, b2 = r[2][1].v, b3 = r[3][1].v;
    __m128 c0 = r[4][0].v, c1 = r[5][0].v, c2 = r[6][0].v, c3 = r[7][0].v;
    __m128 d0 = r[4][1].v, d1 = r[5][1].v, d2 = r[6][1].v, d3 = r[7][1].v;
    _MM_TRANSPOSE4_PS(a0, a1, a2, a3);
    _MM_TRANSPOSE4_PS(b0, b1, b2, b3);
    _MM_TRANSPOSE4_PS(c0, c1, c2, c3);
    _MM_TRANSPOSE4_PS(d0, d1, d2, d3);
    r[0][0] = a0, r[1][0] = a1, r[2][0] = a2, r[3][0] = a3;
    r[0][1] = c0, r[1][1] = c1, r[2][1] = c2, r[3][1] = c3;
    r[4][0] = b0, r[5][0] = b1, r[6][0] = b2, r[7][0] = b3;
    r[4][1] = d0, r[5][1] = d1, r[6][1] = d2, r[7][1] = d3;
}

// One block's samples (rows `stride` apart) to its 64 coefficients, each multiplied by its factor
// and rounded to the nearest (an even one at a half), as 16-bit numbers.
inline void forwardBlockSse(const uint8_t* p, size_t stride, const float* factor, int16_t* out) {
    const __m128i zero = _mm_setzero_si128();
    const __m128 c128 = _mm_set1_ps(128.0f);
    F4 r[8][2];
    for (int y = 0; y < 8; y++) {
        const __m128i row = _mm_unpacklo_epi8(_mm_loadl_epi64(reinterpret_cast<const __m128i*>(p + static_cast<size_t>(y) * stride)), zero);
        r[y][0] = _mm_sub_ps(_mm_cvtepi32_ps(_mm_unpacklo_epi16(row, zero)), c128);
        r[y][1] = _mm_sub_ps(_mm_cvtepi32_ps(_mm_unpackhi_epi16(row, zero)), c128);
    }
    const F4 c707 = _mm_set1_ps(kC707), c382 = _mm_set1_ps(kC382), c541 = _mm_set1_ps(kC541), c1306 = _mm_set1_ps(kC1306);
    for (int pass = 0; pass < 2; pass++) {
        for (int h = 0; h < 2; h++) forwardEight(r[0][h], r[1][h], r[2][h], r[3][h], r[4][h], r[5][h], r[6][h], r[7][h], c707, c382, c541, c1306);
        transpose8(r);
    }
    for (int y = 0; y < 8; y++) {
        const __m128i lo = _mm_cvtps_epi32(_mm_mul_ps(r[y][0].v, _mm_loadu_ps(factor + y * 8)));
        const __m128i hi = _mm_cvtps_epi32(_mm_mul_ps(r[y][1].v, _mm_loadu_ps(factor + y * 8 + 4)));
        _mm_storeu_si128(reinterpret_cast<__m128i*>(out + y * 8), _mm_packs_epi32(lo, hi));
    }
}
#endif

const std::array<float, 64>& aanScale() {
    static const std::array<float, 64> scale = [] {
        const double f[8] = {1.0, 1.387039845, 1.306562965, 1.175875602, 1.0, 0.785694958, 0.541196100, 0.275899379};
        std::array<float, 64> s{};
        for (int i = 0; i < 64; i++) s[static_cast<size_t>(i)] = static_cast<float>(f[i / 8] * f[i % 8] * 8.0);
        return s;
    }();
    return scale;
}

// Up to 64 ints of a list (natural order), or none.
bool steps(VM& vm, const Value& v, std::array<int, 64>& out) {
    if (v.isNull()) return false;
    if (!v.isObjType(ObjType::List) || asList(v)->size() != 64) throw vm.runtimeError(Thrown::IllegalArgumentException, "a table of steps has 64 of them, in natural order");
    for (size_t i = 0; i < 64; i++) {
        const Value s = vm.listGet(asList(v), i);
        if (!s.isInt() || s.as.i < 1 || s.as.i > 65535) throw vm.runtimeError(Thrown::IllegalArgumentException, "a step is from 1 to 65535");
        out[i] = static_cast<int>(s.as.i);
    }
    return true;
}

// ---- Planes ----

// A plane `width` by `height` (rows `stride` apart) made `toWidth` by `toHeight`: twice either way
// by a triangle filter, 3 parts the nearest sample and 1 the next (libjpeg's "fancy" upsampling),
// half by averaging, any other size by bilinear interpolation between sample centres.
std::vector<uint8_t> scalePlane(const uint8_t* p, size_t stride, int w, int h, int tw, int th) {
    std::vector<uint8_t> out(static_cast<size_t>(tw) * static_cast<size_t>(th));
    auto at = [&](int x, int y) -> int { return p[static_cast<size_t>(std::clamp(y, 0, h - 1)) * stride + static_cast<size_t>(std::clamp(x, 0, w - 1))]; };
    const bool twiceX = tw == 2 * w || tw == 2 * w - 1, twiceY = th == 2 * h || th == 2 * h - 1;
    const bool sameX = tw == w, sameY = th == h;
    const bool halfX = w == 2 * tw || w == 2 * tw - 1, halfY = h == 2 * th || h == 2 * th - 1;
    if ((twiceX || sameX) && (twiceY || sameY)) {
        // A row at a time: first down the columns (3 parts the nearer row and 1 the other, or 4
        // parts the one row) into `column`, its first and last repeated once past each end; then
        // across.
        std::vector<int16_t> columnRow(static_cast<size_t>(w) + 2);
        int16_t* column = columnRow.data() + 1;
        for (int y = 0; y < th; y++) {
            const int sy = twiceY ? y / 2 : y;
            const int ny = std::clamp(twiceY ? (y % 2 == 0 ? sy - 1 : sy + 1) : sy, 0, h - 1);
            const uint8_t* near = p + static_cast<size_t>(std::min(sy, h - 1)) * stride;
            const uint8_t* far = p + static_cast<size_t>(ny) * stride;
            if (twiceY) {
                for (int x = 0; x < w; x++) column[x] = static_cast<int16_t>(near[x] * 3 + far[x]);
            } else {
                for (int x = 0; x < w; x++) column[x] = static_cast<int16_t>(near[x] * 4);
            }
            column[-1] = column[0];
            column[w] = column[w - 1];
            uint8_t* o = &out[static_cast<size_t>(y) * static_cast<size_t>(tw)];
            if (twiceX) {
                const int pairs = tw / 2;
                int k0 = 0;
#ifdef JADE_SSE2
                // Eight pairs at a time: 3 parts each sample and 1 the one before (or after).
                const __m128i eight = _mm_set1_epi16(8);
                for (; k0 + 8 <= pairs; k0 += 8) {
                    const __m128i c = _mm_loadu_si128(reinterpret_cast<const __m128i*>(column + k0));
                    const __m128i before = _mm_loadu_si128(reinterpret_cast<const __m128i*>(column + k0 - 1));
                    const __m128i after = _mm_loadu_si128(reinterpret_cast<const __m128i*>(column + k0 + 1));
                    const __m128i c3 = _mm_add_epi16(_mm_add_epi16(c, c), _mm_add_epi16(c, eight));
                    const __m128i even = _mm_srli_epi16(_mm_add_epi16(c3, before), 4), odd = _mm_srli_epi16(_mm_add_epi16(c3, after), 4);
                    _mm_storeu_si128(reinterpret_cast<__m128i*>(o + 2 * k0), _mm_packus_epi16(_mm_unpacklo_epi16(even, odd), _mm_unpackhi_epi16(even, odd)));
                }
#endif
                for (int k = k0; k < pairs; k++) {
                    o[2 * k] = static_cast<uint8_t>((column[k] * 3 + column[k - 1] + 8) >> 4);
                    o[2 * k + 1] = static_cast<uint8_t>((column[k] * 3 + column[k + 1] + 8) >> 4);
                }
                if (tw % 2 != 0) o[tw - 1] = static_cast<uint8_t>((column[pairs] * 3 + column[pairs - 1] + 8) >> 4);
            } else {
                for (int x = 0; x < tw; x++) o[x] = static_cast<uint8_t>((column[x] + 2) >> 2);
            }
        }
        return out;
    }
    if (halfX && halfY) {
        // Each 2 by 2 averaged, the last column or row repeated where the size is odd.
        for (int y = 0; y < th; y++) {
            const uint8_t* r0 = p + static_cast<size_t>(2 * y) * stride;
            const uint8_t* r1 = p + static_cast<size_t>(std::min(2 * y + 1, h - 1)) * stride;
            uint8_t* o = &out[static_cast<size_t>(y) * static_cast<size_t>(tw)];
            const int whole = w / 2;
            for (int x = 0; x < whole; x++) o[x] = static_cast<uint8_t>((r0[2 * x] + r0[2 * x + 1] + r1[2 * x] + r1[2 * x + 1] + 2) >> 2);
            if (whole < tw) o[whole] = static_cast<uint8_t>((r0[w - 1] * 2 + r1[w - 1] * 2 + 2) >> 2);
        }
        return out;
    }
    if ((halfX || sameX) && (halfY || sameY)) {
        const int fx = halfX ? 2 : 1, fy = halfY ? 2 : 1;
        for (int y = 0; y < th; y++)
            for (int x = 0; x < tw; x++) {
                int sum = 0;
                for (int dy = 0; dy < fy; dy++)
                    for (int dx = 0; dx < fx; dx++) sum += at(x * fx + dx, y * fy + dy);
                out[static_cast<size_t>(y) * static_cast<size_t>(tw) + static_cast<size_t>(x)] = static_cast<uint8_t>((sum + fx * fy / 2) / (fx * fy));
            }
        return out;
    }
    for (int y = 0; y < th; y++) {
        const double sy = (y + 0.5) * h / th - 0.5;
        const int y0 = static_cast<int>(std::floor(sy));
        const double fy = sy - y0;
        for (int x = 0; x < tw; x++) {
            const double sx = (x + 0.5) * w / tw - 0.5;
            const int x0 = static_cast<int>(std::floor(sx));
            const double fx = sx - x0;
            const double v = (at(x0, y0) * (1 - fx) + at(x0 + 1, y0) * fx) * (1 - fy) + (at(x0, y0 + 1) * (1 - fx) + at(x0 + 1, y0 + 1) * fx) * fy;
            out[static_cast<size_t>(y) * static_cast<size_t>(tw) + static_cast<size_t>(x)] = static_cast<uint8_t>(std::lround(v));
        }
    }
    return out;
}

// YCbCr as JPEG (JFIF) has it, full range, to and from red, green and blue: in 16384ths, so that
// each factor fits 16 bits and SSE2 multiplies pairs of them at once.
//   Y  = 0.299 R + 0.587 G + 0.114 B             R = Y + 1.402 Cr'
//   Cb = -0.168736 R - 0.331264 G + 0.5 B + 128  G = Y - 0.344136 Cb' - 0.714136 Cr'
//   Cr = 0.5 R - 0.418688 G - 0.081312 B + 128   B = Y + 1.772 Cb'      (Cb' = Cb - 128, Cr' = Cr - 128)
constexpr int kYr = 4899, kYg = 9617, kYb = 1868, kCbR = -2765, kCbG = -5427, kCrG = -6860, kCrB = -1332, kHalf = 8192;
constexpr int kRcr = 22970, kGcb = -5638, kGcr = -11700, kBcb = 29032;

inline uint8_t lumaOf(int r, int g, int b) { return static_cast<uint8_t>((kYr * r + kYg * g + kYb * b + kHalf) >> 14); }

// `n` RGBA pixels to Y, Cb and Cr (or Y alone where cb is null).
void rgbToYcc(const uint8_t* px, size_t n, uint8_t* y, uint8_t* cb, uint8_t* cr) {
    size_t i = 0;
#ifdef JADE_SSE2
    const __m128i lowBytes = _mm_set1_epi32(0x00FF00FF);
    // Pairs of factors for (R, B) and (G, A) in each 32-bit pixel.
    const __m128i yRB = _mm_set1_epi32((kYb << 16) | kYr), yGA = _mm_set1_epi32(kYg);
    const __m128i cbRB = _mm_set1_epi32((kHalf << 16) | (kCbR & 0xFFFF)), cbGA = _mm_set1_epi32(kCbG & 0xFFFF);
    const __m128i crRB = _mm_set1_epi32(((kCrB & 0xFFFF) << 16) | kHalf), crGA = _mm_set1_epi32(kCrG & 0xFFFF);
    const __m128i roundY = _mm_set1_epi32(kHalf), roundC = _mm_set1_epi32(kHalf - 1 + (128 << 14));
    auto three = [&](__m128i p, __m128i& oy, __m128i& ob, __m128i& orr) {
        const __m128i rb = _mm_and_si128(p, lowBytes), ga = _mm_and_si128(_mm_srli_epi32(p, 8), lowBytes);
        oy = _mm_srai_epi32(_mm_add_epi32(_mm_add_epi32(_mm_madd_epi16(rb, yRB), _mm_madd_epi16(ga, yGA)), roundY), 14);
        ob = _mm_srai_epi32(_mm_add_epi32(_mm_add_epi32(_mm_madd_epi16(rb, cbRB), _mm_madd_epi16(ga, cbGA)), roundC), 14);
        orr = _mm_srai_epi32(_mm_add_epi32(_mm_add_epi32(_mm_madd_epi16(rb, crRB), _mm_madd_epi16(ga, crGA)), roundC), 14);
    };
    for (; i + 16 <= n; i += 16) {
        __m128i ys[4], bs[4], rs[4];
        for (int k = 0; k < 4; k++) three(_mm_loadu_si128(reinterpret_cast<const __m128i*>(px + (i + k * 4) * 4)), ys[k], bs[k], rs[k]);
        _mm_storeu_si128(reinterpret_cast<__m128i*>(y + i), _mm_packus_epi16(_mm_packs_epi32(ys[0], ys[1]), _mm_packs_epi32(ys[2], ys[3])));
        if (cb != nullptr) {
            _mm_storeu_si128(reinterpret_cast<__m128i*>(cb + i), _mm_packus_epi16(_mm_packs_epi32(bs[0], bs[1]), _mm_packs_epi32(bs[2], bs[3])));
            _mm_storeu_si128(reinterpret_cast<__m128i*>(cr + i), _mm_packus_epi16(_mm_packs_epi32(rs[0], rs[1]), _mm_packs_epi32(rs[2], rs[3])));
        }
    }
#endif
    for (; i < n; i++) {
        const int r = px[i * 4], g = px[i * 4 + 1], b = px[i * 4 + 2];
        y[i] = lumaOf(r, g, b);
        if (cb != nullptr) {
            cb[i] = static_cast<uint8_t>(((kCbR * r + kCbG * g + kHalf * b + kHalf - 1) >> 14) + 128);
            cr[i] = static_cast<uint8_t>(((kHalf * r + kCrG * g + kCrB * b + kHalf - 1) >> 14) + 128);
        }
    }
}

// `n` pixels of Y, Cb and Cr to RGBA, alpha 255.
void yccToRgb(const uint8_t* y, const uint8_t* cb, const uint8_t* cr, size_t n, uint8_t* out) {
    size_t i = 0;
#ifdef JADE_SSE2
    const __m128i zero = _mm_setzero_si128(), c128 = _mm_set1_epi16(128), opaque = _mm_set1_epi8(static_cast<char>(0xFF));
    const __m128i rPair = _mm_set1_epi32(kRcr << 16), gPair = _mm_set1_epi32((kGcr << 16) | (kGcb & 0xFFFF)), bPair = _mm_set1_epi32(kBcb & 0xFFFF);
    const __m128i round = _mm_set1_epi32(kHalf);
    // Four pixels' (Cb', Cr') pairs times a pair of factors, in 16384ths, made whole.
    auto part = [&](__m128i pairs, __m128i factors) { return _mm_srai_epi32(_mm_add_epi32(_mm_madd_epi16(pairs, factors), round), 14); };
    for (; i + 16 <= n; i += 16) {
        const __m128i yy = _mm_loadu_si128(reinterpret_cast<const __m128i*>(y + i));
        const __m128i bb = _mm_loadu_si128(reinterpret_cast<const __m128i*>(cb + i));
        const __m128i rr = _mm_loadu_si128(reinterpret_cast<const __m128i*>(cr + i));
        __m128i r8[2], g8[2], b8[2];
        for (int half = 0; half < 2; half++) {
            const __m128i y16 = half == 0 ? _mm_unpacklo_epi8(yy, zero) : _mm_unpackhi_epi8(yy, zero);
            const __m128i b16 = _mm_sub_epi16(half == 0 ? _mm_unpacklo_epi8(bb, zero) : _mm_unpackhi_epi8(bb, zero), c128);
            const __m128i r16 = _mm_sub_epi16(half == 0 ? _mm_unpacklo_epi8(rr, zero) : _mm_unpackhi_epi8(rr, zero), c128);
            const __m128i lo = _mm_unpacklo_epi16(b16, r16), hi = _mm_unpackhi_epi16(b16, r16);
            r8[half] = _mm_add_epi16(y16, _mm_packs_epi32(part(lo, rPair), part(hi, rPair)));
            g8[half] = _mm_add_epi16(y16, _mm_packs_epi32(part(lo, gPair), part(hi, gPair)));
            b8[half] = _mm_add_epi16(y16, _mm_packs_epi32(part(lo, bPair), part(hi, bPair)));
        }
        const __m128i R = _mm_packus_epi16(r8[0], r8[1]), G = _mm_packus_epi16(g8[0], g8[1]), B = _mm_packus_epi16(b8[0], b8[1]);
        const __m128i rgLo = _mm_unpacklo_epi8(R, G), rgHi = _mm_unpackhi_epi8(R, G);
        const __m128i baLo = _mm_unpacklo_epi8(B, opaque), baHi = _mm_unpackhi_epi8(B, opaque);
        __m128i* o = reinterpret_cast<__m128i*>(out + i * 4);
        _mm_storeu_si128(o, _mm_unpacklo_epi16(rgLo, baLo));
        _mm_storeu_si128(o + 1, _mm_unpackhi_epi16(rgLo, baLo));
        _mm_storeu_si128(o + 2, _mm_unpacklo_epi16(rgHi, baHi));
        _mm_storeu_si128(o + 3, _mm_unpackhi_epi16(rgHi, baHi));
    }
#endif
    for (; i < n; i++) {
        const int yy = y[i], b = cb[i] - 128, r = cr[i] - 128;
        out[i * 4] = clampSample(yy + ((kRcr * r + kHalf) >> 14));
        out[i * 4 + 1] = clampSample(yy + ((kGcb * b + kGcr * r + kHalf) >> 14));
        out[i * 4 + 2] = clampSample(yy + ((kBcb * b + kHalf) >> 14));
        out[i * 4 + 3] = 255;
    }
}

}  // namespace

void registerPicture(VM& vm) {
    ObjClass* holder = vm.alloc<ObjClass>(vm.intern("Picture"));
    auto def = [&](const char* name, int minArgs, int maxArgs, NativeFn fn) {
        holder->statics.set(vm.intern(name), Value::object(vm.alloc<ObjNative>(std::string("Picture.") + name, minArgs, maxArgs, fn)));
    };
    // palette(pixels, count): the colours, four bytes each, alpha 255.
    def("palette", 2, 2, [](VM& vm, int, Value* args) -> Value {
        const std::vector<uint8_t>& px = pixelsOf(vm, args[0]);
        const int64_t count = vm.toInt(args[1], "count");
        if (count < 1 || count > 256) throw vm.runtimeError(Thrown::IllegalArgumentException, "a palette has 1 to 256 colours, not " + std::to_string(count));
        return bytesValue(vm, paletteOf(px, static_cast<size_t>(count)));
    });
    // indices(pixels, width, height, palette, method): each pixel's index in the palette (four
    // bytes a colour), the palette's size for a see-through one.
    def("indices", 5, 5, [](VM& vm, int, Value* args) -> Value {
        const std::vector<uint8_t>& px = pixelsOf(vm, args[0]);
        const int64_t width = vm.toInt(args[1], "width"), height = vm.toInt(args[2], "height");
        if (width < 0 || height < 0 || static_cast<size_t>(width * height * 4) != px.size())
            throw vm.runtimeError(Thrown::IllegalArgumentException, "the pixels are not " + std::to_string(width) + " by " + std::to_string(height));
        if (!args[3].isObjType(ObjType::Bytes)) throw vm.runtimeError(Thrown::IllegalArgumentException, "expected a palette's colours as bytes");
        const std::vector<uint8_t>& palette = asBytes(args[3])->data;
        if (palette.empty() || palette.size() % 4 != 0 || palette.size() > 1024)
            throw vm.runtimeError(Thrown::IllegalArgumentException, "a palette has 1 to 256 colours");
        const int64_t method = vm.toInt(args[4], "method");
        if (palette.size() == 1024)
            for (size_t i = 3; i < px.size(); i += 4)
                if (px[i] < kSeeThrough)
                    throw vm.runtimeError(Thrown::IllegalArgumentException, "a palette of 256 colours leaves no index for the see-through pixels: give it 255 at most");
        return bytesValue(vm, indicesOf(px, static_cast<int>(width), static_cast<int>(height), palette, static_cast<int>(method)));
    });
    // inverse8x8(coefficients, blocksWide, blocksHigh, plane, stride, scale): each block's 64
    // coefficients (16 bits each, natural order), multiplied by scale's steps where given, into
    // samples of the plane.
    def("inverse8x8", 6, 6, [](VM& vm, int, Value* args) -> Value {
        if (!args[0].isObjType(ObjType::Bytes) || !args[3].isObjType(ObjType::Bytes))
            throw vm.runtimeError(Thrown::IllegalArgumentException, "Dct.inverse8x8 takes coefficients and a plane as Bytes");
        const std::vector<uint8_t>& coefs = asBytes(args[0])->data;
        std::vector<uint8_t>& plane = asBytes(args[3])->data;
        const int64_t bw = vm.toInt(args[1], "blocksWide"), bh = vm.toInt(args[2], "blocksHigh"), stride = vm.toInt(args[4], "stride");
        if (bw < 0 || bh < 0 || stride < bw * 8) throw vm.runtimeError(Thrown::IllegalArgumentException, "the blocks are wider than the plane's rows");
        if (coefs.size() < static_cast<size_t>(bw * bh * 128)) throw vm.runtimeError(Thrown::IllegalArgumentException, "fewer coefficients than " + std::to_string(bw * bh) + " blocks have");
        if (bh > 0 && plane.size() < static_cast<size_t>(((bh - 1) * 8 + 7) * stride + bw * 8)) throw vm.runtimeError(Thrown::IllegalArgumentException, "the plane is smaller than the blocks");
        std::array<int, 64> scale;
        const bool scaled = steps(vm, args[5], scale);
        int16_t steps16[64];
        for (size_t i = 0; i < 64; i++) steps16[i] = static_cast<int16_t>(scaled ? scale[i] : 1);  // in 16 bits, as the products are kept
        for (int64_t by = 0; by < bh; by++)
            for (int64_t bx = 0; bx < bw; bx++) {
                int16_t c[64];
                std::memcpy(c, &coefs[static_cast<size_t>((by * bw + bx) * 128)], 128);
                uint8_t* out = &plane[static_cast<size_t>(by * 8 * stride + bx * 8)];
#ifdef JADE_SSE2
                inverseBlockSse(c, steps16, out, static_cast<size_t>(stride));
#else
                inverseBlock(c, steps16, out, static_cast<size_t>(stride));
#endif
            }
        return Value::null();
    });
    // forward8x8(plane, stride, blocksWide, blocksHigh, divisors): each block's 64 coefficients
    // (16 bits each, natural order), divided by the divisors where given and rounded.
    def("forward8x8", 5, 5, [](VM& vm, int, Value* args) -> Value {
        if (!args[0].isObjType(ObjType::Bytes)) throw vm.runtimeError(Thrown::IllegalArgumentException, "Dct.forward8x8 takes a plane as Bytes");
        const std::vector<uint8_t>& plane = asBytes(args[0])->data;
        const int64_t stride = vm.toInt(args[1], "stride"), bw = vm.toInt(args[2], "blocksWide"), bh = vm.toInt(args[3], "blocksHigh");
        if (bw < 0 || bh < 0 || stride < bw * 8) throw vm.runtimeError(Thrown::IllegalArgumentException, "the blocks are wider than the plane's rows");
        if (bh > 0 && plane.size() < static_cast<size_t>(((bh - 1) * 8 + 7) * stride + bw * 8)) throw vm.runtimeError(Thrown::IllegalArgumentException, "the plane is smaller than the blocks");
        std::array<int, 64> divisors;
        const bool divided = steps(vm, args[4], divisors);
        std::array<float, 64> factor;
        for (size_t i = 0; i < 64; i++) factor[i] = 1.0f / (aanScale()[i] * (divided ? static_cast<float>(divisors[i]) : 1.0f));
        std::vector<uint8_t> out(static_cast<size_t>(bw * bh * 128));
        for (int64_t by = 0; by < bh; by++)
            for (int64_t bx = 0; bx < bw; bx++) {
                const uint8_t* p = &plane[static_cast<size_t>(by * 8 * stride + bx * 8)];
                int16_t q[64];
#ifdef JADE_SSE2
                forwardBlockSse(p, static_cast<size_t>(stride), factor.data(), q);
#else
                float block[64];
                for (int y = 0; y < 8; y++)
                    for (int x = 0; x < 8; x++) block[y * 8 + x] = static_cast<float>(p[static_cast<size_t>(y * stride + x)]) - 128.0f;
                forwardBlock(block);
                for (size_t i = 0; i < 64; i++) {
                    // Rounded to the nearest, an even one at a half, as SSE2 rounds.
                    const float v = std::clamp(block[i] * factor[i], -32768.0f, 32767.0f);
                    q[i] = static_cast<int16_t>(std::nearbyint(v));
                }
#endif
                std::memcpy(&out[static_cast<size_t>((by * bw + bx) * 128)], q, 128);
            }
        return bytesValue(vm, std::move(out));
    });
    // scalePlane(plane, width, height, toWidth, toHeight, stride).
    def("scalePlane", 6, 6, [](VM& vm, int, Value* args) -> Value {
        if (!args[0].isObjType(ObjType::Bytes)) throw vm.runtimeError(Thrown::IllegalArgumentException, "a plane is Bytes");
        const std::vector<uint8_t>& p = asBytes(args[0])->data;
        const int64_t w = vm.toInt(args[1], "width"), h = vm.toInt(args[2], "height"), tw = vm.toInt(args[3], "toWidth"), th = vm.toInt(args[4], "toHeight");
        const int64_t stride = vm.toInt(args[5], "stride");
        if (w < 1 || h < 1 || tw < 1 || th < 1 || w > 65536 || h > 65536 || tw > 65536 || th > 65536 || stride < w)
            throw vm.runtimeError(Thrown::IllegalArgumentException, "a plane of " + std::to_string(w) + " by " + std::to_string(h) + " made " + std::to_string(tw) + " by " +
                                                                     std::to_string(th) + ", rows " + std::to_string(stride) + " apart, cannot be");
        if (p.size() < static_cast<size_t>((h - 1) * stride + w)) throw vm.runtimeError(Thrown::IllegalArgumentException, "the plane is smaller than its size says");
        return bytesValue(vm, scalePlane(p.data(), static_cast<size_t>(stride), static_cast<int>(w), static_cast<int>(h), static_cast<int>(tw), static_cast<int>(th)));
    });
    // fromPlanes(planes, width, height, space, into): RGBA pixels from planes of one byte a sample,
    // each width by height (0 grey, 1 RGB, 2 YCbCr), written into a picture's cells of that size.
    def("fromPlanes", 5, 5, [](VM& vm, int, Value* args) -> Value {
        const int64_t w = vm.toInt(args[1], "width"), h = vm.toInt(args[2], "height"), space = vm.toInt(args[3], "space");
        if (!args[0].isObjType(ObjType::List)) throw vm.runtimeError(Thrown::IllegalArgumentException, "planes are a list of Bytes");
        ObjList* list = asList(args[0]);
        const size_t want = space == 0 ? 1 : 3;
        if (list->size() != want) throw vm.runtimeError(Thrown::IllegalArgumentException, "the colour space takes " + std::to_string(want) + " planes, not " + std::to_string(list->size()));
        const size_t n = static_cast<size_t>(w * h);
        std::vector<const uint8_t*> planes;
        for (size_t i = 0; i < want; i++) {
            const Value v = vm.listGet(list, i);
            if (!v.isObjType(ObjType::Bytes) || asBytes(v)->data.size() != n)
                throw vm.runtimeError(Thrown::IllegalArgumentException, "each plane is " + std::to_string(w) + " by " + std::to_string(h) + " bytes");
            planes.push_back(asBytes(v)->data.data());
        }
        if (!args[4].isObjType(ObjType::List) || !asList(args[4])->packed() || asList(args[4])->pack->block.size() != n * 4)
            throw vm.runtimeError(Thrown::IllegalArgumentException, "the picture is not " + std::to_string(w) + " by " + std::to_string(h));
        std::vector<uint8_t>& out = asList(args[4])->pack->block;
        if (space == 0) {
            for (size_t i = 0; i < n; i++) out[i * 4] = out[i * 4 + 1] = out[i * 4 + 2] = planes[0][i], out[i * 4 + 3] = 255;
        } else if (space == 1) {
            for (size_t i = 0; i < n; i++) out[i * 4] = planes[0][i], out[i * 4 + 1] = planes[1][i], out[i * 4 + 2] = planes[2][i], out[i * 4 + 3] = 255;
        } else {
            yccToRgb(planes[0], planes[1], planes[2], n, out.data());
        }
        return Value::null();
    });
    // isOpaque(pixels): whether no pixel is see-through at all.
    def("isOpaque", 1, 1, [](VM& vm, int, Value* args) -> Value {
        const std::vector<uint8_t>& px = pixelsOf(vm, args[0]);
        const size_t n = px.size() / 4;
        uint32_t all = 0xFFFFFFFFu;
        for (size_t i = 0; i < n; i++) {
            uint32_t pixel;
            std::memcpy(&pixel, px.data() + i * 4, 4);
            all &= pixel;
            if ((i & 4095) == 4095 && (all >> 24) != 255) return Value::boolean(false);
        }
        return Value::boolean((all >> 24) == 255);
    });
    // toPlanes(pixels, space): planes of one byte a sample from RGBA pixels (alpha left out).
    def("toPlanes", 2, 2, [](VM& vm, int, Value* args) -> Value {
        const std::vector<uint8_t>& px = pixelsOf(vm, args[0]);
        const int64_t space = vm.toInt(args[1], "space");
        const size_t n = px.size() / 4;
        const size_t count = space == 0 ? 1 : 3;
        std::vector<std::vector<uint8_t>> planes(count, std::vector<uint8_t>(n));
        const uint8_t* q = px.data();
        if (space == 1) {
            for (size_t i = 0; i < n; i++) planes[0][i] = q[i * 4], planes[1][i] = q[i * 4 + 1], planes[2][i] = q[i * 4 + 2];
        } else {
            rgbToYcc(q, n, planes[0].data(), space == 2 ? planes[1].data() : nullptr, space == 2 ? planes[2].data() : nullptr);
        }
        ObjList* list = newList(vm);
        TempRoot keep(vm, Value::object(list));
        for (auto& plane : planes) {
            const Value v = bytesValue(vm, std::move(plane));
            TempRoot keepPlane(vm, v);
            vm.listAdd(list, v);
        }
        return Value::object(list);
    });
    vm.defineGlobal("__picture", Value::object(holder));
}

}  // namespace jade
