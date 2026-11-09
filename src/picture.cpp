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

// The inverse DCT of one block of 64 coefficients (natural order), into samples with 128 added:
// separable, in integers of 4096ths, libjpeg's slow integer method as stb_image has it.
void inverseBlock(int* b, uint8_t* out, size_t stride) {
    for (int i = 0; i < 8; i++) {
        if (b[i + 8] == 0 && b[i + 16] == 0 && b[i + 24] == 0 && b[i + 32] == 0 && b[i + 40] == 0 && b[i + 48] == 0 && b[i + 56] == 0) {
            const int dc = b[i] * 4;
            for (int k = 0; k < 8; k++) b[i + k * 8] = dc;
            continue;
        }
        const int p1 = (b[i + 16] + b[i + 48]) * 2217;
        const int t2 = p1 + b[i + 48] * -7567, t3 = p1 + b[i + 16] * 3135;
        const int t0 = (b[i] + b[i + 32]) * 4096, t1 = (b[i] - b[i + 32]) * 4096;
        const int x0 = t0 + t3 + 512, x3 = t0 - t3 + 512, x1 = t1 + t2 + 512, x2 = t1 - t2 + 512;
        int o0 = b[i + 56], o1 = b[i + 40], o2 = b[i + 24], o3 = b[i + 8];
        const int q3 = o0 + o2, q4 = o1 + o3, q1 = o0 + o3, q2 = o1 + o2;
        const int q5 = (q3 + q4) * 4816;
        o0 *= 1223;
        o1 *= 8410;
        o2 *= 12586;
        o3 *= 6149;
        const int r1 = q5 + q1 * -3685, r2 = q5 + q2 * -10497, r3 = q3 * -8034, r4 = q4 * -1597;
        o3 += r1 + r4;
        o2 += r2 + r3;
        o1 += r2 + r4;
        o0 += r1 + r3;
        b[i] = (x0 + o3) >> 10;
        b[i + 56] = (x0 - o3) >> 10;
        b[i + 8] = (x1 + o2) >> 10;
        b[i + 48] = (x1 - o2) >> 10;
        b[i + 16] = (x2 + o1) >> 10;
        b[i + 40] = (x2 - o1) >> 10;
        b[i + 24] = (x3 + o0) >> 10;
        b[i + 32] = (x3 - o0) >> 10;
    }
    const int bias = 65536 + (128 << 17);
    for (int row = 0; row < 8; row++) {
        const int* r = b + row * 8;
        const int p1 = (r[2] + r[6]) * 2217;
        const int t2 = p1 + r[6] * -7567, t3 = p1 + r[2] * 3135;
        const int t0 = (r[0] + r[4]) * 4096, t1 = (r[0] - r[4]) * 4096;
        const int x0 = t0 + t3 + bias, x3 = t0 - t3 + bias, x1 = t1 + t2 + bias, x2 = t1 - t2 + bias;
        int o0 = r[7], o1 = r[5], o2 = r[3], o3 = r[1];
        const int q3 = o0 + o2, q4 = o1 + o3, q1 = o0 + o3, q2 = o1 + o2;
        const int q5 = (q3 + q4) * 4816;
        o0 *= 1223;
        o1 *= 8410;
        o2 *= 12586;
        o3 *= 6149;
        const int r1 = q5 + q1 * -3685, r2 = q5 + q2 * -10497, r3 = q3 * -8034, r4 = q4 * -1597;
        o3 += r1 + r4;
        o2 += r2 + r3;
        o1 += r2 + r4;
        o0 += r1 + r3;
        uint8_t* o = out + static_cast<size_t>(row) * stride;
        o[0] = clampSample((x0 + o3) >> 17);
        o[7] = clampSample((x0 - o3) >> 17);
        o[1] = clampSample((x1 + o2) >> 17);
        o[6] = clampSample((x1 - o2) >> 17);
        o[2] = clampSample((x2 + o1) >> 17);
        o[5] = clampSample((x2 - o1) >> 17);
        o[3] = clampSample((x3 + o0) >> 17);
        o[4] = clampSample((x3 - o0) >> 17);
    }
}

// One pass of the forward DCT, AAN's way in floats (libjpeg's float method), over 8 lines of 8
// samples `step` apart, the lines `lineStep` apart.
template <int step, int lineStep>
inline void forwardPass(float* b) {
    for (int i = 0; i < 8; i++) {
        float* s = b + i * lineStep;
        const float t0 = s[0] + s[7 * step], t7 = s[0] - s[7 * step];
        const float t1 = s[step] + s[6 * step], t6 = s[step] - s[6 * step];
        const float t2 = s[2 * step] + s[5 * step], t5 = s[2 * step] - s[5 * step];
        const float t3 = s[3 * step] + s[4 * step], t4 = s[3 * step] - s[4 * step];
        const float t10 = t0 + t3, t13 = t0 - t3, t11 = t1 + t2, t12 = t1 - t2;
        s[0] = t10 + t11;
        s[4 * step] = t10 - t11;
        const float z1 = (t12 + t13) * 0.707106781f;
        s[2 * step] = t13 + z1;
        s[6 * step] = t13 - z1;
        const float u10 = t4 + t5, u11 = t5 + t6, u12 = t6 + t7;
        const float z5 = (u10 - u12) * 0.382683433f;
        const float z2 = 0.541196100f * u10 + z5, z4 = 1.306562965f * u12 + z5, z3 = u11 * 0.707106781f;
        const float z11 = t7 + z3, z13 = t7 - z3;
        s[5 * step] = z13 + z2;
        s[3 * step] = z13 - z2;
        s[step] = z11 + z4;
        s[7 * step] = z11 - z4;
    }
}

// The forward DCT of 8 by 8 samples (128 taken off): rows, then columns; each coefficient left
// scaled by `aan`, which the caller takes out.
void forwardBlock(float* b) {
    forwardPass<1, 8>(b);
    forwardPass<8, 1>(b);
}

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
        std::vector<int> columnRow(static_cast<size_t>(w) + 2);
        int* column = columnRow.data() + 1;
        for (int y = 0; y < th; y++) {
            const int sy = twiceY ? y / 2 : y;
            const int ny = std::clamp(twiceY ? (y % 2 == 0 ? sy - 1 : sy + 1) : sy, 0, h - 1);
            const uint8_t* near = p + static_cast<size_t>(std::min(sy, h - 1)) * stride;
            const uint8_t* far = p + static_cast<size_t>(ny) * stride;
            if (twiceY) {
                for (int x = 0; x < w; x++) column[x] = near[x] * 3 + far[x];
            } else {
                for (int x = 0; x < w; x++) column[x] = near[x] * 4;
            }
            column[-1] = column[0];
            column[w] = column[w - 1];
            uint8_t* o = &out[static_cast<size_t>(y) * static_cast<size_t>(tw)];
            if (twiceX) {
                const int pairs = tw / 2;
                for (int k = 0; k < pairs; k++) {
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

// YCbCr as JPEG (JFIF) has it, full range: to and from red, green and blue, in 65536ths.
struct YccTables {
    int crR[256], cbG[256], crG[256], cbB[256];
    YccTables() {
        for (int i = 0; i < 256; i++) {
            const double d = i - 128;
            crR[i] = static_cast<int>(std::lround(1.402 * 65536 * d)) + 32768;
            cbG[i] = -static_cast<int>(std::lround(0.344136 * 65536 * d)) + 32768;
            crG[i] = -static_cast<int>(std::lround(0.714136 * 65536 * d));
            cbB[i] = static_cast<int>(std::lround(1.772 * 65536 * d)) + 32768;
        }
    }
};
const YccTables& ycc() {
    static const YccTables tables;
    return tables;
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
        int block[64];
        for (int64_t by = 0; by < bh; by++)
            for (int64_t bx = 0; bx < bw; bx++) {
                const uint8_t* c = &coefs[static_cast<size_t>((by * bw + bx) * 128)];
                for (int i = 0; i < 64; i++) {
                    int16_t v;
                    std::memcpy(&v, c + i * 2, 2);
                    block[i] = scaled ? v * scale[static_cast<size_t>(i)] : v;
                }
                inverseBlock(block, &plane[static_cast<size_t>(by * 8 * stride + bx * 8)], static_cast<size_t>(stride));
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
        float block[64];
        for (int64_t by = 0; by < bh; by++)
            for (int64_t bx = 0; bx < bw; bx++) {
                const uint8_t* p = &plane[static_cast<size_t>(by * 8 * stride + bx * 8)];
                for (int y = 0; y < 8; y++)
                    for (int x = 0; x < 8; x++) block[y * 8 + x] = static_cast<float>(p[static_cast<size_t>(y * stride + x)]) - 128.0f;
                forwardBlock(block);
                uint8_t* o = &out[static_cast<size_t>((by * bw + bx) * 128)];
                int16_t q[64];
                for (size_t i = 0; i < 64; i++) {
                    // Rounded half away from zero, as lround, without its call.
                    const float v = std::clamp(block[i] * factor[i], -32768.0f, 32767.0f);
                    q[i] = static_cast<int16_t>(v + (v < 0 ? -0.5f : 0.5f));
                }
                std::memcpy(o, q, 128);
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
            const YccTables& t = ycc();
            const uint8_t* py = planes[0];
            const uint8_t* pb = planes[1];
            const uint8_t* pr = planes[2];
            uint8_t* o = out.data();
            for (size_t i = 0; i < n; i++) {
                const int y = py[i] << 16, cb = pb[i], cr = pr[i];
                const uint32_t pixel = clampSample((y + t.crR[cr]) >> 16) | (static_cast<uint32_t>(clampSample((y + t.cbG[cb] + t.crG[cr]) >> 16)) << 8) |
                                       (static_cast<uint32_t>(clampSample((y + t.cbB[cb]) >> 16)) << 16) | 0xFF000000u;
                std::memcpy(o + i * 4, &pixel, 4);
            }
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
        } else if (space == 0) {
            uint8_t* y = planes[0].data();
            for (size_t i = 0; i < n; i++) y[i] = clampSample((19595 * q[i * 4] + 38470 * q[i * 4 + 1] + 7471 * q[i * 4 + 2] + 32768) >> 16);
        } else {
            uint8_t* y = planes[0].data();
            uint8_t* cb = planes[1].data();
            uint8_t* cr = planes[2].data();
            for (size_t i = 0; i < n; i++) {
                const int r = q[i * 4], g = q[i * 4 + 1], b = q[i * 4 + 2];
                y[i] = clampSample((19595 * r + 38470 * g + 7471 * b + 32768) >> 16);
                cb[i] = clampSample(((-11059 * r - 21709 * g + 32768 * b + 32767) >> 16) + 128);
                cr[i] = clampSample(((32768 * r - 27439 * g - 5329 * b + 32767) >> 16) + 128);
            }
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
