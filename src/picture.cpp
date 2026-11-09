// What general picture operations (src/std/picture.jade) are built on, over a picture's pixels as
// kept in a Grid<Color> or an Image (four bytes each: red, green, blue, alpha): the colours that
// stand best for a picture, and the picture put into them, dithered or not (M9/M10 part one).

#include <algorithm>
#include <array>
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
    vm.defineGlobal("__picture", Value::object(holder));
}

}  // namespace jade
