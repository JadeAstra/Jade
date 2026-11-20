// The text engine and pictures as WinterWolf programs see them: the built-in __text, which
// src/std/fonts.ww is written on.
//
// Fonts are known to a program by number. A font file once read stays read, and a font at a
// size keeps the pictures of its glyphs, for as long as the program runs: there are only as
// many as the program asks for. A picture's pixels, which is where the memory goes, are an
// ordinary `bytes` value that the collector frees.

#include <algorithm>
#include <cmath>
#include <cstring>
#include <mutex>
#include <shared_mutex>
#include <unordered_map>

#include "../builtins.h"
#include "../image/png.h"
#include "../sandbox.h"
#include "../vm.h"
#include "engine.h"
#include "fonts.h"
#include "shapes.h"

#include "../section_window.h"  // last: this file's own code goes in its section (D161)

namespace ww {

namespace {

using text::Face;
using text::Font;
using text::FontBook;

// The fonts handed out to programs. Shared by every runtime of the process (workers have
// their own), so a lock goes with it.
struct Handles {
    std::shared_mutex mutex;  // shared by those that look a number up, which a drawing core does for each text (G64)
    std::vector<std::shared_ptr<Face>> faces;
    std::unordered_map<const Face*, int64_t> faceNumbers;
    std::vector<std::shared_ptr<Font>> fonts;
    std::unordered_map<const Font*, int64_t> fontNumbers;
};

Handles& handles() {
    static auto* h = new Handles();  // never torn down: a thread may still be drawing at exit
    return *h;
}

int64_t numberOf(const std::shared_ptr<Face>& face) {
    Handles& h = handles();
    std::unique_lock<std::shared_mutex> hold(h.mutex);
    auto known = h.faceNumbers.find(face.get());
    if (known != h.faceNumbers.end()) return known->second;
    h.faces.push_back(face);
    return h.faceNumbers[face.get()] = static_cast<int64_t>(h.faces.size() - 1);
}

int64_t numberOf(const std::shared_ptr<Font>& font) {
    Handles& h = handles();
    std::unique_lock<std::shared_mutex> hold(h.mutex);
    auto known = h.fontNumbers.find(font.get());
    if (known != h.fontNumbers.end()) return known->second;
    h.fonts.push_back(font);
    return h.fontNumbers[font.get()] = static_cast<int64_t>(h.fonts.size() - 1);
}

std::shared_ptr<Face> faceOf(VM& vm, Value v) {
    const int64_t number = vm.toInt(v, "typeface");
    Handles& h = handles();
    std::shared_lock<std::shared_mutex> hold(h.mutex);
    if (number < 0 || static_cast<size_t>(number) >= h.faces.size()) throw vm.runtimeError(Thrown::IllegalStateException, "not a typeface");
    return h.faces[static_cast<size_t>(number)];
}

std::shared_ptr<Font> fontOf(VM& vm, Value v) {
    const int64_t number = vm.toInt(v, "font");
    Handles& h = handles();
    std::shared_lock<std::shared_mutex> hold(h.mutex);
    if (number < 0 || static_cast<size_t>(number) >= h.fonts.size()) throw vm.runtimeError(Thrown::IllegalStateException, "not a font");
    return h.fonts[static_cast<size_t>(number)];
}

// A character given as a char, a one-character string or its number.
uint32_t characterOf(VM& vm, Value v) {
    if (v.isChar()) return static_cast<uint32_t>(v.as.i);
    if (v.isInt()) {
        const int64_t cp = v.as.i;
        if (cp < 0 || cp > 0x10FFFF) throw vm.runtimeError(Thrown::IllegalArgumentException, "not a character: " + std::to_string(cp));
        return static_cast<uint32_t>(cp);
    }
    const std::string& s = vm.toStr(v, "character");
    if (s.empty()) throw vm.runtimeError(Thrown::NoSuchElementException, "a character is wanted, and the string is empty");
    size_t at = 0;
    return text::nextCodepoint(s, at);
}

// From 0xAARRGGBB, as programs write colours, to 0xRRGGBBAA.
uint32_t colorOf(VM& vm, Value v) {
    const uint64_t argb = static_cast<uint64_t>(vm.toInt(v, "colour"));
    return static_cast<uint32_t>(((argb & 0xFFFFFF) << 8) | ((argb >> 24) & 0xFF));
}

// A picture's pixels: the bytes value, checked against the size it is said to have.
std::span<uint8_t> pixelsOf(VM& vm, Value* args, int& width, int& height) {
    // Bytes, or the cells of a Grid<Color>, which are kept as the same four bytes a pixel.
    const bool cells = args[0].isObjType(ObjType::List) && asList(args[0])->packed() && asList(args[0])->pack->width == 4;
    if (!args[0].isObjType(ObjType::Bytes) && !cells) throw vm.runtimeError(Thrown::ClassCastException, "a picture's pixels are bytes");
    const int64_t w = vm.toInt(args[1], "width"), h = vm.toInt(args[2], "height");
    const std::span<uint8_t> data = cells ? std::span<uint8_t>(asList(args[0])->pack->block) : std::span<uint8_t>(asBytes(args[0])->data);
    if (w < 0 || h < 0 || w > 32768 || h > 32768 || static_cast<uint64_t>(w) * static_cast<uint64_t>(h) * 4 != data.size())
        throw vm.runtimeError(Thrown::IllegalArgumentException, "the picture's pixels do not fit its size");
    width = static_cast<int>(w);
    height = static_cast<int>(h);
    return data;
}

int clampInt(int64_t v) { return static_cast<int>(std::clamp<int64_t>(v, -(int64_t{1} << 30), int64_t{1} << 30)); }

Value listOf(VM& vm, std::vector<Value> items) {
    ObjList* list = newList(vm, std::move(items));
    vm.account(list->values().size() * sizeof(Value));
    return Value::object(list);
}

Value matchValue(VM& vm, const text::Match& match) {
    if (!match) return Value::null();
    return listOf(vm, {Value::integer(numberOf(match.face)), Value::integer(match.weight), Value::boolean(match.fakeBold), Value::boolean(match.fakeItalic)});
}

std::string tagName(uint32_t tag) {
    std::string s;
    for (int shift = 24; shift >= 0; shift -= 8) s += static_cast<char>((tag >> shift) & 0xFF);
    return s;
}

// ---- The software engine: what an Image's canvas draws with (D123) ----
//
// An image's pixels are four bytes each, red, green, blue and alpha, not multiplied by the
// alpha: the cells of its Grid<Color>, or a Bytes. A canvas on it has an origin and a clip in
// those pixels and a scale of pixels to the unit; what it draws is given in units. Shapes are
// made outlines in pixels and filled with the text engine's rasterizer, which gives each pixel
// the share of it the shape covers, so edges are smooth; rectangles and straight lines are put
// on whole pixels first, so they are crisp.

struct Target {
    uint8_t* rgba = nullptr;
    int width = 0, height = 0;             // the picture, in pixels
    double scale = 1;                      // pixels to the unit
    double originX = 0, originY = 0;       // where the canvas's (0, 0) is, in pixels
    int clipLeft = 0, clipTop = 0, clipRight = 0, clipBottom = 0;
};

// The pixels of a picture: a Bytes, or the cells of a Grid<Color>, checked against its size.
std::span<uint8_t> rgbaOf(VM& vm, Value v, int64_t w, int64_t h) {
    std::span<uint8_t> data;
    bool found = false;
    if (v.isObjType(ObjType::Bytes)) data = asBytes(v)->data, found = true;
    else if (v.isObjType(ObjType::List) && asList(v)->packed() && asList(v)->pack->width == 4 && asList(v)->pack->store == ListStore::Items) data = asList(v)->pack->block, found = true;
    if (!found) throw vm.runtimeError(Thrown::ClassCastException, "a picture's pixels are bytes or a grid of colours");
    if (w < 0 || h < 0 || w > 32768 || h > 32768 || static_cast<uint64_t>(w) * static_cast<uint64_t>(h) * 4 != data.size())
        throw vm.runtimeError(Thrown::IllegalArgumentException, "the picture's pixels do not fit its size");
    return data;
}

// In a parallel section a canvas draws on an image the piece made, or through a part of the
// image's latest split that was handed to the piece, while the section has the image's pixels
// marked as drawn on in parts (parallel.cpp); the parts' clips never overlap. Nothing else.
void mayDraw(VM& vm, const ObjInstance* canvas, const Value& pixels) {
    if (!pixels.isObj()) return;  // rgbaOf says what is wrong
    const Obj* list = pixels.as.obj;
    if (list->core == vm.coreId() && list->frozen == 0) return;
    if (list->core == kDrawnInParts && canvas->core == vm.coreId()) {
        Value part, image, splits;
        if (canvas->readText(vm.intern("part"), part) && part.isInt() && part.as.i != 0 && canvas->readText(vm.intern("image"), image) &&
            image.isObjType(ObjType::Instance) && asInstance(image)->readText(vm.intern("splits"), splits) && splits.isInt() && splits.as.i == part.as.i)
            return;
        throw vm.runtimeError(Thrown::IllegalStateException, "this part is from an earlier split of the image: only the parts of its latest split draw on it in a parallel section");
    }
    throw vm.runtimeError(Thrown::IllegalStateException,
                          "in a parallel section a canvas draws only on an image this piece made, or through a part from its canvas's split() that was handed to this piece");
}

// target(canvas, width, height, scale, originX, originY, clipLeft, clipTop, clipRight, clipBottom):
// the first ten arguments, the canvas an ImageCanvas, whose image's pixels it holds.
Target targetOf(VM& vm, Value* args) {
    Target t;
    t.width = static_cast<int>(vm.toInt(args[1], "width"));
    t.height = static_cast<int>(vm.toInt(args[2], "height"));
    Value pixels;
    if (!args[0].isObjType(ObjType::Instance) || !asInstance(args[0])->readText(vm.intern("pixels"), pixels))
        throw vm.runtimeError(Thrown::InternalError, "an ImageCanvas is drawn on");
    if (vm.coreId() != 0) mayDraw(vm, asInstance(args[0]), pixels);
    t.rgba = rgbaOf(vm, pixels, t.width, t.height).data();
    t.scale = vm.toNumber(args[3], "scale");
    t.originX = vm.toNumber(args[4], "origin");
    t.originY = vm.toNumber(args[5], "origin");
    t.clipLeft = std::max(0, clampInt(vm.toInt(args[6], "clip")));
    t.clipTop = std::max(0, clampInt(vm.toInt(args[7], "clip")));
    t.clipRight = std::min(t.width, clampInt(vm.toInt(args[8], "clip")));
    t.clipBottom = std::min(t.height, clampInt(vm.toInt(args[9], "clip")));
    if (!(t.scale > 0)) throw vm.runtimeError(Thrown::IllegalArgumentException, "a canvas's scale is more than 0");
    return t;
}

double num(VM& vm, Value v) { return vm.toNumber(v, "a coordinate"); }

// Lays one pixel of colour (0xRRGGBBAA, its alpha already multiplied by any coverage: `sa`) over
// what is there, as drawGlyph does.
inline void blendPixel(uint8_t* d, uint32_t r, uint32_t g, uint32_t b, uint32_t sa) {
    if (sa == 0) return;
    const uint32_t da = d[3];
    if (sa == 255 || da == 0) {
        d[0] = static_cast<uint8_t>(r);
        d[1] = static_cast<uint8_t>(g);
        d[2] = static_cast<uint8_t>(b);
        d[3] = static_cast<uint8_t>(sa);
        return;
    }
    // Source over: what is underneath shows through by (255 - sa).
    const uint32_t keep = (da * (255 - sa) + 127) / 255;
    const uint32_t outA = sa + keep;
    d[0] = static_cast<uint8_t>((r * sa + d[0] * keep + outA / 2) / outA);
    d[1] = static_cast<uint8_t>((g * sa + d[1] * keep + outA / 2) / outA);
    d[2] = static_cast<uint8_t>((b * sa + d[2] * keep + outA / 2) / outA);
    d[3] = static_cast<uint8_t>(outA);
}

// A glyph or a shape's coverage laid over the target at (x, baseline) as drawGlyph places it,
// inside the clip; a colour glyph in its own colours, faded by the colour's alpha.
void blendImage(const Target& t, const text::GlyphImage& image, int x, int baseline, uint32_t color) {
    if (image.empty()) return;
    const int left = x + image.left, top = baseline - image.top;
    const int fromCol = std::max(0, t.clipLeft - left), toCol = std::min(image.width, t.clipRight - left);
    const int fromRow = std::max(0, t.clipTop - top), toRow = std::min(image.height, t.clipBottom - top);
    if (fromCol >= toCol || fromRow >= toRow) return;
    const uint32_t red = (color >> 24) & 0xFF, green = (color >> 16) & 0xFF, blue = (color >> 8) & 0xFF, alpha = color & 0xFF;
    for (int row = fromRow; row < toRow; row++) {
        uint8_t* d = t.rgba + (static_cast<size_t>(top + row) * static_cast<size_t>(t.width) + static_cast<size_t>(left + fromCol)) * 4;
        for (int col = fromCol; col < toCol; col++, d += 4) {
            if (image.color) {
                const uint8_t* s = &image.pixels[(static_cast<size_t>(row) * static_cast<size_t>(image.width) + static_cast<size_t>(col)) * 4];
                const uint32_t sa = s[3] * alpha / 255;
                if (sa == 0) continue;
                // Its colours are multiplied by its alpha: taken out again for the blend.
                blendPixel(d, std::min<uint32_t>(255, s[0] * 255 / std::max<uint32_t>(s[3], 1)), std::min<uint32_t>(255, s[1] * 255 / std::max<uint32_t>(s[3], 1)),
                           std::min<uint32_t>(255, s[2] * 255 / std::max<uint32_t>(s[3], 1)), sa);
            } else {
                const uint32_t coverage = image.pixels[static_cast<size_t>(row) * static_cast<size_t>(image.width) + static_cast<size_t>(col)];
                if (coverage != 0) blendPixel(d, red, green, blue, (coverage * alpha + 127) / 255);
            }
        }
    }
}

// An outline in the target's pixels (y growing downwards) filled in a colour.
void fillPath(const Target& t, const text::Path& downward, uint32_t color, text::FillRule rule = text::FillRule::NonZero) {
    if (downward.empty()) return;
    // The rasterizer has y growing upwards: the outline turned over, and the result put back.
    text::Path up = downward;
    for (size_t i = 1; i < up.points.size(); i += 2) up.points[i] = -up.points[i];
    // A row at a time, only the pixels the shape's edges reach worked on (a slanted line across
    // the picture was its whole box, about 1 ms); the same pixels as rasterize and blendImage.
    const uint32_t red = (color >> 24) & 0xFF, green = (color >> 16) & 0xFF, blue = (color >> 8) & 0xFF, alpha = color & 0xFF;
    auto lay = [&](const text::Path& path, int fromRow, int toRow, int fromCol, int toCol) {
        text::rasterizeRows(path, text::kShapeFineness, fromRow, toRow, rule, [&](int row, int left, int count, const uint8_t* coverage) {
            if (row < fromRow || row >= toRow) return;
            const int from = std::max(left, fromCol), to = std::min(left + count, toCol);
            if (from >= to) return;
            uint8_t* d = t.rgba + (static_cast<size_t>(row) * static_cast<size_t>(t.width) + static_cast<size_t>(from)) * 4;
            for (int col = from; col < to; col++, d += 4) {
                const uint32_t c = coverage[col - left];
                if (c != 0) blendPixel(d, red, green, blue, (c * alpha + 127) / 255);
            }
        });
    };
    if (text::rasterizesRows(up, t.clipTop, t.clipBottom)) {
        lay(up, t.clipTop, t.clipBottom, t.clipLeft, t.clipRight);
        return;
    }
    // Too large or too far off for the rasterizer as it is (2026-11-13: drawn as nothing): drawn
    // in pieces of the clip, at most 4096 pixels each way, the outline cut to each (a pixel round
    // it to spare, which is not laid on); those it does not reach left out.
    float minX, minY, maxX, maxY;
    up.bounds(minX, minY, maxX, maxY);
    constexpr int kPiece = 4096;
    for (int r0 = t.clipTop; r0 < t.clipBottom; r0 += kPiece) {
        const int r1 = std::min(t.clipBottom, r0 + kPiece);
        if (!(minY < static_cast<float>(-r0) + 1 && maxY > static_cast<float>(-r1) - 1)) continue;  // rows r0 to r1 are y -r1 to -r0, upwards
        for (int c0 = t.clipLeft; c0 < t.clipRight; c0 += kPiece) {
            const int c1 = std::min(t.clipRight, c0 + kPiece);
            if (!(minX < static_cast<float>(c1) + 1 && maxX > static_cast<float>(c0) - 1)) continue;
            const text::Path piece = text::clipped(up, text::kShapeFineness, static_cast<float>(c0 - 1), static_cast<float>(-r1 - 1),
                                                   static_cast<float>(c1 + 1), static_cast<float>(-r0 + 1));
            lay(piece, r0, r1, c0, c1);
        }
    }
}

// A rectangle in pixels, (l, t) to (r, b), clockwise; or anticlockwise to cut it out of another.
void rectPath(text::Path& p, double l, double tp, double r, double b, bool reverse = false) {
    p.moveTo(static_cast<float>(l), static_cast<float>(tp));
    if (!reverse) {
        p.lineTo(static_cast<float>(r), static_cast<float>(tp));
        p.lineTo(static_cast<float>(r), static_cast<float>(b));
        p.lineTo(static_cast<float>(l), static_cast<float>(b));
    } else {
        p.lineTo(static_cast<float>(l), static_cast<float>(b));
        p.lineTo(static_cast<float>(r), static_cast<float>(b));
        p.lineTo(static_cast<float>(r), static_cast<float>(tp));
    }
    p.close();
}

// A rectangle with its corners rounded by quarter ellipses of radii rx, ry (an ellipse when they
// are half its size), in pixels; anticlockwise with `reverse`.
void roundPath(text::Path& p, double l, double tp, double r, double b, double rx, double ry, bool reverse = false) {
    rx = std::clamp(rx, 0.0, (r - l) / 2);
    ry = std::clamp(ry, 0.0, (b - tp) / 2);
    if (r <= l || b <= tp) return;
    const double k = 0.5522847498;  // how far a quarter circle's control points are, of its radius
    const double cx = rx * k, cy = ry * k;
    auto f = [](double v) { return static_cast<float>(v); };
    if (!reverse) {
        p.moveTo(f(l + rx), f(tp));
        p.lineTo(f(r - rx), f(tp));
        p.cubicTo(f(r - rx + cx), f(tp), f(r), f(tp + ry - cy), f(r), f(tp + ry));
        p.lineTo(f(r), f(b - ry));
        p.cubicTo(f(r), f(b - ry + cy), f(r - rx + cx), f(b), f(r - rx), f(b));
        p.lineTo(f(l + rx), f(b));
        p.cubicTo(f(l + rx - cx), f(b), f(l), f(b - ry + cy), f(l), f(b - ry));
        p.lineTo(f(l), f(tp + ry));
        p.cubicTo(f(l), f(tp + ry - cy), f(l + rx - cx), f(tp), f(l + rx), f(tp));
    } else {
        p.moveTo(f(l + rx), f(tp));
        p.cubicTo(f(l + rx - cx), f(tp), f(l), f(tp + ry - cy), f(l), f(tp + ry));
        p.lineTo(f(l), f(b - ry));
        p.cubicTo(f(l), f(b - ry + cy), f(l + rx - cx), f(b), f(l + rx), f(b));
        p.lineTo(f(r - rx), f(b));
        p.cubicTo(f(r - rx + cx), f(b), f(r), f(b - ry + cy), f(r), f(b - ry));
        p.lineTo(f(r), f(tp + ry));
        p.cubicTo(f(r), f(tp + ry - cy), f(r - rx + cx), f(tp), f(r - rx), f(tp));
    }
    p.close();
}

// Units to the target's pixels, on whole pixels for what sits on pixel edges.
inline double px(const Target& t, double units) { return units * t.scale; }
inline double edgeX(const Target& t, double x) { return std::round(t.originX + x * t.scale); }
inline double edgeY(const Target& t, double y) { return std::round(t.originY + y * t.scale); }
// How thick a line of one unit is, in whole pixels.
inline double thickness(const Target& t) { return std::max(1.0, std::round(t.scale)); }
// How thick a line of the canvas's lineWidth is, in whole pixels (M8): at least one.
inline double strokePixels(const Target& t, double units) { return std::max(1.0, std::round(units * t.scale)); }

// A line `w` pixels thick (one unit by default) through the middles of the pixels at its two
// ends, with square ends, as Java draws it; across and up and down it is on whole pixels. A
// wider one spreads around the one-unit line, its larger half to the right and below.
void linePath(const Target& t, text::Path& p, double x1, double y1, double x2, double y2, double w = 0) {
    const double unit = thickness(t);
    if (w <= 0) w = unit;
    const text::Spread spread = text::spreadOf(w, unit);
    if (y1 == y2 || x1 == x2) {
        const double l = edgeX(t, std::min(x1, x2)) - spread.before, r = edgeX(t, std::max(x1, x2)) + unit + spread.after;
        const double tp = edgeY(t, std::min(y1, y2)) - spread.before, b = edgeY(t, std::max(y1, y2)) + unit + spread.after;
        rectPath(p, l, tp, r, b);
        return;
    }
    const double shift = (spread.after - spread.before) / 2;
    const double ax = t.originX + (x1 + 0.5) * t.scale + shift, ay = t.originY + (y1 + 0.5) * t.scale + shift;
    const double bx = t.originX + (x2 + 0.5) * t.scale + shift, by = t.originY + (y2 + 0.5) * t.scale + shift;
    const double dx = bx - ax, dy = by - ay, length = std::sqrt(dx * dx + dy * dy);
    const double ux = dx / length * w / 2, uy = dy / length * w / 2;  // along, half the thickness
    auto f = [](double v) { return static_cast<float>(v); };
    p.moveTo(f(ax - ux - uy), f(ay - uy + ux));
    p.lineTo(f(bx + ux - uy), f(by + uy + ux));
    p.lineTo(f(bx + ux + uy), f(by + uy - ux));
    p.lineTo(f(ax - ux + uy), f(ay - uy - ux));
    p.close();
}

// A colour given as 0xAARRGGBB, as the drawing engine wants it.
uint32_t engineColor(VM& vm, Value v) { return colorOf(vm, v); }

}  // namespace

bool text::pathOf(VM& vm, Value outline, double originX, double originY, double scale, Path& out) {
    if (!outline.isObjType(ObjType::List)) throw vm.runtimeError(Thrown::ClassCastException, "a flattened path is a list of numbers");
    ObjList* list = asList(outline);
    const size_t size = list->size();
    auto at = [&](size_t i) { return vm.toNumber(vm.listGet(list, i), "a flattened path's number"); };
    size_t i = 0;
    while (i < size) {
        const double n = at(i++);
        if (!(n >= 0) || n > static_cast<double>((size - i) / 2)) throw vm.runtimeError(Thrown::IllegalArgumentException, "a contour has more points than the path");
        const size_t count = static_cast<size_t>(n);
        for (size_t k = 0; k < count; k++, i += 2) {
            const float x = static_cast<float>(originX + at(i) * scale), y = static_cast<float>(originY + at(i + 1) * scale);
            if (k == 0) out.moveTo(x, y);
            else out.lineTo(x, y);
        }
        if (count > 0) out.close();
    }
    return !out.empty();
}

namespace {

// The fonts the code running now sees (M6.6.2). In a sandbox given a folder of fonts, those
// alone: read the first time they are wanted, and shared by every sandbox given that folder, as
// the typefaces and fonts made from them are kept for as long as the program. Anywhere else, the
// system's: null here.
std::shared_ptr<FontBook> sandboxFonts(VM& vm) {
    Host& host = vm.currentHost();
    if (!host.isSandbox()) return nullptr;
    const std::string& folder = static_cast<SandboxHost&>(host).fontsFrom;
    if (folder.empty()) return nullptr;
    static std::mutex mutex;
    static auto* books = new std::unordered_map<std::string, std::shared_ptr<FontBook>>();  // never torn down, as the fonts are not
    std::lock_guard<std::mutex> hold(mutex);
    std::shared_ptr<FontBook>& book = (*books)[folder];
    if (book == nullptr) {
        book = std::make_shared<FontBook>();
        std::vector<std::string> names;
        if (!realHost().list(folder, names)) {
            std::sort(names.begin(), names.end());
            for (const std::string& name : names) {
                const std::string kind = name.size() > 4 ? text::lowerCase(name.substr(name.size() - 4)) : std::string();
                if (kind != ".ttf" && kind != ".otf" && kind != ".ttc" && kind != ".otc") continue;
                std::string error;
                book->addFile(folder + "/" + name, error);
            }
        }
    }
    return book;
}

FontBook& fontsOf(VM& vm) {
    std::shared_ptr<FontBook> own = sandboxFonts(vm);
    return own != nullptr ? *own : FontBook::installed();  // a sandbox's is kept for the program
}

}  // namespace

// For the drawing code of the native layer, which is handed fonts by the same numbers.
std::shared_ptr<text::Font> textFontOf(VM& vm, Value v) { return fontOf(vm, v); }

void registerText(VM& vm) {
    ObjClass* lib = vm.alloc<ObjClass>(vm.intern("text"));
    auto def = [&](const char* name, int minArgs, int maxArgs, NativeFn fn) {
        lib->statics.set(vm.intern(name), Value::object(vm.alloc<ObjNative>(std::string("text.") + name, minArgs, maxArgs, fn)));
    };

    // ---- The fonts there are ----

    def("families", 0, 0, [](VM& vm, int, Value*) -> Value {
        ObjList* list = newList(vm);
        TempRoot keep(vm, Value::object(list));
        for (std::string& family : fontsOf(vm).families()) list->values().push_back(vm.str(std::move(family)));
        vm.account(list->values().size() * sizeof(Value));
        return Value::object(list);
    });

    def("hasFamily", 1, 1, [](VM& vm, int, Value* args) -> Value {
        return Value::boolean(static_cast<bool>(fontsOf(vm).matchExactly(vm.toStr(args[0], "font family"), 400, false)));
    });

    // The typefaces of a family, or every one installed if the family is null.
    def("typefaces", 1, 1, [](VM& vm, int, Value* args) -> Value {
        FontBook& book = fontsOf(vm);
        const std::string wanted = args[0].isNull() ? std::string() : text::lowerCase(vm.toStr(args[0], "font family"));
        std::vector<Value> out;
        const std::vector<text::FaceInfo> faces = book.faces();
        for (size_t i = 0; i < faces.size(); i++) {
            if (!args[0].isNull() && text::lowerCase(faces[i].family) != wanted && text::lowerCase(faces[i].legacyFamily) != wanted) continue;
            if (auto face = book.face(static_cast<int>(i))) out.push_back(Value::integer(numberOf(face)));
        }
        return listOf(vm, std::move(out));
    });

    // How many fonts a file holds; 0 if it is not a font file. In a sandbox, a file of its own.
    def("countIn", 1, 1, [](VM& vm, int, Value* args) -> Value {
        const std::string& path = vm.toStr(args[0], "file path");
        Host& host = vm.currentHost();
        if (!host.isSandbox()) return Value::integer(Face::count(path));
        std::string bytes;
        if (host.readWhole(path, bytes)) return Value::integer(0);
        return Value::integer(Face::count(reinterpret_cast<const uint8_t*>(bytes.data()), bytes.size()));
    });

    def("load", 2, 2, [](VM& vm, int, Value* args) -> Value {
        const std::string& path = vm.toStr(args[0], "file path");
        const int index = clampInt(vm.toInt(args[1], "index"));
        Host& host = vm.currentHost();
        std::string error;
        std::shared_ptr<Face> face;
        if (!host.isSandbox()) {
            face = Face::load(path, index, error);
        } else {
            std::string bytes;
            if (host.readWhole(path, bytes)) throw vm.errorOf("IOException", "cannot read " + path);
            face = Face::fromBytes(std::vector<uint8_t>(bytes.begin(), bytes.end()), index, path, error);
        }
        if (face == nullptr) throw vm.errorOf("IOException", error);
        return Value::integer(numberOf(face));
    });

    def("fromBytes", 2, 2, [](VM& vm, int, Value* args) -> Value {
        if (!args[0].isObjType(ObjType::Bytes)) throw vm.runtimeError(Thrown::ClassCastException, "a font is read from bytes, not " + vm.stringify(args[0]));
        std::string error;
        auto face = Face::fromBytes(asBytes(args[0])->data, clampInt(vm.toInt(args[1], "index")), "<bytes>", error);
        if (face == nullptr) throw vm.errorOf("IOException", error);
        return Value::integer(numberOf(face));
    });

    // find(family, weight, italic): [typeface, weight, fakeBold, fakeItalic], or null if no
    // such family is installed.
    def("find", 3, 3, [](VM& vm, int, Value* args) -> Value {
        return matchValue(vm, fontsOf(vm).matchExactly(vm.toStr(args[0], "font family"), clampInt(vm.toInt(args[1], "weight")), args[2].truthy()));
    });

    // The same for a character a font lacks: the font to take it from.
    def("fallback", 3, 3, [](VM& vm, int, Value* args) -> Value {
        return matchValue(vm, fontsOf(vm).fallback(characterOf(vm, args[0]), clampInt(vm.toInt(args[1], "weight")), args[2].truthy()));
    });

    // ---- A typeface ----

    def("faceInfo", 1, 1, [](VM& vm, int, Value* args) -> Value {
        auto face = faceOf(vm, args[0]);
        ObjList* list = newList(vm);
        TempRoot keep(vm, Value::object(list));
        auto& out = list->values();
        out.push_back(vm.str(face->path()));
        out.push_back(Value::integer(face->index()));
        out.push_back(vm.str(face->family()));
        out.push_back(vm.str(face->style()));
        out.push_back(vm.str(face->fullName()));
        out.push_back(Value::integer(face->weight()));
        out.push_back(Value::integer(face->stretch()));
        out.push_back(Value::boolean(face->italic()));
        out.push_back(Value::boolean(face->monospace()));
        out.push_back(Value::boolean(face->symbol()));
        out.push_back(Value::boolean(face->variable()));
        out.push_back(Value::boolean(face->cubic()));
        out.push_back(Value::boolean(face->hasColor()));
        out.push_back(Value::integer(face->glyphCount()));
        out.push_back(Value::integer(face->unitsPerEm()));
        out.push_back(vm.str(face->legacyFamily()));
        vm.account(out.size() * sizeof(Value));
        return Value::object(list);
    });

    // Each axis of a variable font: [tag, least, default, most].
    def("faceAxes", 1, 1, [](VM& vm, int, Value* args) -> Value {
        auto face = faceOf(vm, args[0]);
        ObjList* list = newList(vm);
        TempRoot keep(vm, Value::object(list));
        for (const text::Axis& axis : face->axes()) {
            ObjList* one = newList(vm);
            list->values().push_back(Value::object(one));
            one->values().push_back(vm.str(tagName(axis.tag)));
            one->values().push_back(Value::number(axis.minimum));
            one->values().push_back(Value::number(axis.defaultValue));
            one->values().push_back(Value::number(axis.maximum));
        }
        return Value::object(list);
    });

    def("faceGlyph", 2, 2, [](VM& vm, int, Value* args) -> Value { return Value::integer(faceOf(vm, args[0])->glyphIndex(characterOf(vm, args[1]))); });

    // Measurements in the font's own units, for a weight: [ascent, descent, lineGap, xHeight, capHeight].
    def("faceMetrics", 2, 2, [](VM& vm, int, Value* args) -> Value {
        auto face = faceOf(vm, args[0]);
        const text::Coords coords = face->instance(static_cast<float>(vm.toNumber(args[1], "weight")));
        return listOf(vm, {Value::number(face->ascent(coords)), Value::number(face->descent(coords)), Value::number(face->lineGap(coords)),
                           Value::number(face->xHeight(coords)), Value::number(face->capHeight(coords))});
    });

    // A glyph's width in the font's units, for a weight.
    def("faceAdvance", 3, 3, [](VM& vm, int, Value* args) -> Value {
        auto face = faceOf(vm, args[0]);
        return Value::number(face->advance(clampInt(vm.toInt(args[1], "glyph")), face->instance(static_cast<float>(vm.toNumber(args[2], "weight")))));
    });

    // A glyph's outline for a weight, as a list: "M" x y, "L" x y, "Q" cx cy x y,
    // "C" c1x c1y c2x c2y x y, "Z". In the font's units, y upwards. Empty if it has none.
    def("faceOutline", 3, 3, [](VM& vm, int, Value* args) -> Value {
        auto face = faceOf(vm, args[0]);
        text::Path path;
        face->outline(clampInt(vm.toInt(args[1], "glyph")), face->instance(static_cast<float>(vm.toNumber(args[2], "weight"))), path);
        ObjList* list = newList(vm);
        TempRoot keep(vm, Value::object(list));
        static const char* const names[] = {"M", "L", "Q", "C", "Z"};
        static const int counts[] = {2, 2, 4, 6, 0};
        size_t p = 0;
        for (uint8_t verb : path.verbs) {
            list->values().push_back(vm.str(names[verb]));
            for (int i = 0; i < counts[verb]; i++) list->values().push_back(Value::number(path.points[p++]));
        }
        vm.account(list->values().size() * sizeof(Value));
        return Value::object(list);
    });

    // Reads the outlines of glyphs `first`, `first + step`, ... at a weight, as a test of
    // the reader: [how many have an outline, how many points those have, how many are not
    // inside a sensible box].
    def("faceCheck", 4, 4, [](VM& vm, int, Value* args) -> Value {
        auto face = faceOf(vm, args[0]);
        const int first = std::max(0, clampInt(vm.toInt(args[1], "first glyph"))), step = std::max(1, clampInt(vm.toInt(args[2], "step")));
        const text::Coords coords = face->instance(static_cast<float>(vm.toNumber(args[3], "weight")));
        int64_t drawn = 0, points = 0, wild = 0;
        const float limit = static_cast<float>(face->unitsPerEm()) * 16.0f;
        text::Path path;
        for (int glyph = first; glyph < face->glyphCount(); glyph += step) {
            path.clear();
            if (!face->outline(glyph, coords, path)) continue;
            drawn++;
            points += static_cast<int64_t>(path.points.size() / 2);
            float minX, minY, maxX, maxY;
            if (path.bounds(minX, minY, maxX, maxY) && !(minX > -limit && minY > -limit && maxX < limit && maxY < limit)) wild++;
            const float advance = face->advance(glyph, coords);
            if (!(advance >= 0 && advance < limit)) wild++;
        }
        return listOf(vm, {Value::integer(drawn), Value::integer(points), Value::integer(wild)});
    });

    // ---- A font: a typeface at a size ----

    // font(typeface, size, scale, weight, fakeBold, fakeItalic, hinting)
    def("font", 7, 7, [](VM& vm, int, Value* args) -> Value {
        text::Match match;
        match.face = faceOf(vm, args[0]);
        text::FontRequest request;
        request.size = static_cast<float>(vm.toNumber(args[1], "font size"));
        request.scale = static_cast<float>(vm.toNumber(args[2], "scale"));
        if (!(request.size > 0)) throw vm.runtimeError(Thrown::IllegalArgumentException, "a font's size is more than 0");
        if (!(request.scale > 0)) throw vm.runtimeError(Thrown::IllegalArgumentException, "a scale is more than 0");
        match.weight = clampInt(vm.toInt(args[3], "weight"));
        match.fakeBold = args[4].truthy();
        match.fakeItalic = args[5].truthy();
        request.weight = match.weight;
        request.italic = match.face->italic() || match.fakeItalic;
        request.hinting = args[6].truthy();
        auto font = Font::of(match, request, sandboxFonts(vm));
        if (font == nullptr) throw vm.runtimeError(Thrown::IllegalStateException, "the font cannot be made");
        return Value::integer(numberOf(font));
    });

    // [ascent, descent, lineGap, xHeight, capHeight, weight], in device pixels.
    def("fontInfo", 1, 1, [](VM& vm, int, Value* args) -> Value {
        auto font = fontOf(vm, args[0]);
        return listOf(vm, {Value::integer(font->ascent()), Value::integer(font->descent()), Value::integer(font->lineGap()), Value::integer(font->xHeight()),
                           Value::integer(font->capHeight()), Value::integer(font->request().weight)});
    });

    def("width", 2, 2, [](VM& vm, int, Value* args) -> Value { return Value::integer(fontOf(vm, args[0])->width(vm.toStr(args[1], "text"))); });

    def("charWidth", 2, 2, [](VM& vm, int, Value* args) -> Value { return Value::integer(fontOf(vm, args[0])->charWidth(characterOf(vm, args[1]))); });

    // Where each character of a text starts, and where the last ends: one more number than
    // the text has characters.
    def("offsets", 2, 2, [](VM& vm, int, Value* args) -> Value {
        auto font = fontOf(vm, args[0]);
        const std::string& s = vm.toStr(args[1], "text");
        std::vector<Value> out;
        int x = 0;
        for (size_t at = 0; at < s.size();) {
            out.push_back(Value::integer(x));
            x += font->charWidth(text::nextCodepoint(s, at));
        }
        out.push_back(Value::integer(x));
        return listOf(vm, std::move(out));
    });

    // The typeface a character is taken from: the font's own, or another that has it.
    def("faceFor", 2, 2, [](VM& vm, int, Value* args) -> Value {
        auto font = fontOf(vm, args[0]);
        int glyph = 0;
        Font* used = font->fontFor(characterOf(vm, args[1]), glyph);
        if (glyph == 0) return Value::null();
        return Value::integer(numberOf(used->sharedFace()));
    });

    // What a character's picture takes up: [left, top, width, height, colour, advance], placed
    // as in drawText (left from the pen, top above the baseline). Null if nothing is drawn.
    def("glyphBox", 2, 2, [](VM& vm, int, Value* args) -> Value {
        auto font = fontOf(vm, args[0]);
        const uint32_t cp = characterOf(vm, args[1]);
        if (text::invisible(cp)) return Value::null();
        int glyph = 0;
        Font* used = font->fontFor(cp, glyph);
        const text::GlyphImage& image = used->image(glyph);
        if (image.empty()) return Value::null();
        // The picture has a margin: the box is of the pixels with something in them.
        int minX = image.width, minY = image.height, maxX = -1, maxY = -1;
        const size_t bytes = image.color ? 4 : 1;
        for (int y = 0; y < image.height; y++)
            for (int x = 0; x < image.width; x++) {
                const size_t at = (static_cast<size_t>(y) * static_cast<size_t>(image.width) + static_cast<size_t>(x)) * bytes;
                if (image.pixels[at + (image.color ? 3 : 0)] == 0) continue;
                minX = std::min(minX, x);
                maxX = std::max(maxX, x);
                minY = std::min(minY, y);
                maxY = std::max(maxY, y);
            }
        if (maxX < 0) return Value::null();
        return listOf(vm, {Value::integer(image.left + minX), Value::integer(image.top - minY), Value::integer(maxX - minX + 1), Value::integer(maxY - minY + 1),
                           Value::boolean(image.color), Value::integer(used->advance(glyph))});
    });

    // ---- Pictures ----

    // drawText(pixels, width, height, font, text, x, baseline, colour): the text's width.
    def("drawText", 8, 8, [](VM& vm, int, Value* args) -> Value {
        int width, height;
        std::span<uint8_t> pixels = pixelsOf(vm, args, width, height);
        auto font = fontOf(vm, args[3]);
        const std::string& s = vm.toStr(args[4], "text");
        return Value::integer(text::drawText(pixels.data(), width, height, *font, s, clampInt(vm.toInt(args[5], "x")), clampInt(vm.toInt(args[6], "y")), colorOf(vm, args[7])));
    });

    // fill(pixels, width, height, x, y, w, h, colour): sets every pixel of a rectangle.
    def("fill", 8, 8, [](VM& vm, int, Value* args) -> Value {
        int width, height;
        std::span<uint8_t> pixels = pixelsOf(vm, args, width, height);
        const int64_t x = vm.toInt(args[3], "x"), y = vm.toInt(args[4], "y"), w = vm.toInt(args[5], "width"), h = vm.toInt(args[6], "height");
        const uint32_t color = colorOf(vm, args[7]);
        const int x0 = static_cast<int>(std::clamp<int64_t>(x, 0, width)), x1 = static_cast<int>(std::clamp<int64_t>(x + std::max<int64_t>(w, 0), 0, width));
        const int y0 = static_cast<int>(std::clamp<int64_t>(y, 0, height)), y1 = static_cast<int>(std::clamp<int64_t>(y + std::max<int64_t>(h, 0), 0, height));
        for (int row = y0; row < y1; row++) {
            uint8_t* d = &pixels[(static_cast<size_t>(row) * static_cast<size_t>(width) + static_cast<size_t>(x0)) * 4];
            for (int col = x0; col < x1; col++, d += 4) {
                d[0] = static_cast<uint8_t>(color >> 24);
                d[1] = static_cast<uint8_t>(color >> 16);
                d[2] = static_cast<uint8_t>(color >> 8);
                d[3] = static_cast<uint8_t>(color);
            }
        }
        return Value::null();
    });

    // blend(pixels, width, height, x, y, w, h, colour): lays a colour over a rectangle.
    def("blend", 8, 8, [](VM& vm, int, Value* args) -> Value {
        int width, height;
        std::span<uint8_t> pixels = pixelsOf(vm, args, width, height);
        const int64_t x = vm.toInt(args[3], "x"), y = vm.toInt(args[4], "y"), w = vm.toInt(args[5], "width"), h = vm.toInt(args[6], "height");
        const int x0 = static_cast<int>(std::clamp<int64_t>(x, 0, width)), x1 = static_cast<int>(std::clamp<int64_t>(x + std::max<int64_t>(w, 0), 0, width));
        const int y0 = static_cast<int>(std::clamp<int64_t>(y, 0, height)), y1 = static_cast<int>(std::clamp<int64_t>(y + std::max<int64_t>(h, 0), 0, height));
        if (x1 <= x0 || y1 <= y0) return Value::null();
        // A rectangle is a glyph that covers all of its box.
        text::GlyphImage box;
        box.width = x1 - x0;
        box.height = 1;
        box.pixels.assign(static_cast<size_t>(box.width), 255);
        for (int row = y0; row < y1; row++) text::drawGlyph(pixels.data(), width, height, box, x0, row, colorOf(vm, args[7]));
        return Value::null();
    });

    // draw(pixels, width, height, other, otherWidth, otherHeight, x, y): lays another
    // picture over this one with its top-left corner at (x, y).
    def("draw", 8, 8, [](VM& vm, int, Value* args) -> Value {
        int width, height, otherWidth, otherHeight;
        std::span<uint8_t> pixels = pixelsOf(vm, args, width, height);
        const std::span<uint8_t> other = pixelsOf(vm, args + 3, otherWidth, otherHeight);
        // As a colour glyph, which is the same thing: colours multiplied by their alpha.
        text::GlyphImage image;
        image.width = otherWidth;
        image.height = otherHeight;
        image.color = true;
        image.pixels.resize(other.size());
        for (size_t i = 0; i + 3 < other.size(); i += 4) {
            const uint32_t a = other[i + 3];
            image.pixels[i] = static_cast<uint8_t>((other[i] * a + 127) / 255);
            image.pixels[i + 1] = static_cast<uint8_t>((other[i + 1] * a + 127) / 255);
            image.pixels[i + 2] = static_cast<uint8_t>((other[i + 2] * a + 127) / 255);
            image.pixels[i + 3] = static_cast<uint8_t>(a);
        }
        text::drawGlyph(pixels.data(), width, height, image, clampInt(vm.toInt(args[6], "x")), clampInt(vm.toInt(args[7], "y")), 0x000000FF);
        return Value::null();
    });

    def("encodePng", 3, 3, [](VM& vm, int, Value* args) -> Value {
        int width, height;
        const std::span<uint8_t> pixels = pixelsOf(vm, args, width, height);
        if (width == 0 || height == 0) throw vm.runtimeError(Thrown::IllegalArgumentException, "a picture with no pixels cannot be written as PNG");
        ObjBytes* made = vm.alloc<ObjBytes>();
        made->data = image::encodePng(pixels.data(), width, height);
        vm.account(made->data.size());
        return Value::object(made);
    });

    // decodePng(bytes): [width, height, pixels].
    def("decodePng", 1, 1, [](VM& vm, int, Value* args) -> Value {
        if (!args[0].isObjType(ObjType::Bytes)) throw vm.runtimeError(Thrown::ClassCastException, "a PNG picture is read from bytes, not " + vm.stringify(args[0]));
        const std::vector<uint8_t>& data = asBytes(args[0])->data;
        ObjBytes* made = vm.alloc<ObjBytes>();
        TempRoot keep(vm, Value::object(made));
        int width = 0, height = 0;
        std::string error;
        if (!image::decodePng(data.data(), data.size(), made->data, width, height, error)) throw vm.errorOf("IOException", error);
        vm.account(made->data.size());
        return listOf(vm, {Value::integer(width), Value::integer(height), Value::object(made)});
    });

    // ---- The software engine (D123): a canvas on an Image ----
    // Each takes the target first: pixels, width, height, scale, originX, originY and the clip
    // (left, top, right, bottom) in pixels; then a colour where one is drawn, then units.

    // fillRect covers w by h units; drawRect is an outline one unit thick that covers w + 1 by h + 1,
    // as Java's does.
    def("canvasFillRect", 15, 15, [](VM& vm, int, Value* args) -> Value {
        const Target t = targetOf(vm, args);
        const double x = num(vm, args[11]), y = num(vm, args[12]), w = num(vm, args[13]), h = num(vm, args[14]);
        if (!(w > 0) || !(h > 0)) return Value::null();
        // Its edges are on whole pixels (edgeX rounds), so it covers every pixel inside it whole and
        // none outside: each laid on as fillPath lays a pixel it covers whole, a row at a time,
        // without the rasterizer (2026-11-13: 10,000 of 100 by 100, 198 ms; a plain fill, 8). The
        // edges kept as rectPath keeps them, in floats.
        const double l = static_cast<float>(edgeX(t, x)), tp = static_cast<float>(edgeY(t, y));
        const double r = static_cast<float>(edgeX(t, x + w)), b = static_cast<float>(edgeY(t, y + h));
        const double fromX = std::max(l, static_cast<double>(t.clipLeft)), toX = std::min(r, static_cast<double>(t.clipRight));
        const double fromY = std::max(tp, static_cast<double>(t.clipTop)), toY = std::min(b, static_cast<double>(t.clipBottom));
        if (!(fromX < toX) || !(fromY < toY)) return Value::null();
        const uint32_t color = engineColor(vm, args[10]);
        const uint32_t red = (color >> 24) & 0xFF, green = (color >> 16) & 0xFF, blue = (color >> 8) & 0xFF, alpha = color & 0xFF;
        if (alpha == 0) return Value::null();
        const int x0 = static_cast<int>(fromX), x1 = static_cast<int>(toX), y0 = static_cast<int>(fromY), y1 = static_cast<int>(toY);
        const uint8_t whole[4] = {static_cast<uint8_t>(red), static_cast<uint8_t>(green), static_cast<uint8_t>(blue), 255};
        for (int row = y0; row < y1; row++) {
            uint8_t* d = t.rgba + (static_cast<size_t>(row) * static_cast<size_t>(t.width) + static_cast<size_t>(x0)) * 4;
            if (alpha == 255) {
                // As whole words, which Clang writes several at a time, where a pixel's four bytes
                // copied one pixel at a time took 0.5 ns a pixel. A picture's pixels start on a
                // word (a vector's block, four bytes a pixel).
                uint32_t word;
                std::memcpy(&word, whole, 4);
                std::fill_n(reinterpret_cast<uint32_t*>(d), static_cast<size_t>(x1 - x0), word);
            } else {
                for (int col = x0; col < x1; col++, d += 4) blendPixel(d, red, green, blue, alpha);
            }
        }
        return Value::null();
    });
    def("canvasDrawRect", 16, 16, [](VM& vm, int, Value* args) -> Value {
        const Target t = targetOf(vm, args);
        const double x = num(vm, args[11]), y = num(vm, args[12]), w = num(vm, args[13]), h = num(vm, args[14]);
        if (w < 0 || h < 0) return Value::null();
        const double unit = thickness(t), line = strokePixels(t, num(vm, args[15]));
        const text::Spread spread = text::spreadOf(line, unit);
        const double l = edgeX(t, x) - spread.before, tp = edgeY(t, y) - spread.before;
        const double r = edgeX(t, x + w) + unit + spread.after, b = edgeY(t, y + h) + unit + spread.after;
        text::Path p;
        rectPath(p, l, tp, r, b);
        if (r - l > 2 * line && b - tp > 2 * line) rectPath(p, l + line, tp + line, r - line, b - line, true);
        fillPath(t, p, engineColor(vm, args[10]));
        return Value::null();
    });
    // clear(target..., x, y, w, h): the pixels clear again, what is under the picture showing.
    def("canvasClear", 14, 14, [](VM& vm, int, Value* args) -> Value {
        const Target t = targetOf(vm, args);
        const double x = num(vm, args[10]), y = num(vm, args[11]), w = num(vm, args[12]), h = num(vm, args[13]);
        const int l = std::max(t.clipLeft, static_cast<int>(edgeX(t, x))), r = std::min(t.clipRight, static_cast<int>(edgeX(t, x + w)));
        const int tp = std::max(t.clipTop, static_cast<int>(edgeY(t, y))), b = std::min(t.clipBottom, static_cast<int>(edgeY(t, y + h)));
        for (int row = tp; row < b; row++)
            if (r > l) std::memset(t.rgba + (static_cast<size_t>(row) * static_cast<size_t>(t.width) + static_cast<size_t>(l)) * 4, 0, static_cast<size_t>(r - l) * 4);
        return Value::null();
    });
    // roundRect(target..., colour, x, y, w, h, arcW, arcH, fill, lineWidth)
    def("canvasRoundRect", 19, 19, [](VM& vm, int, Value* args) -> Value {
        const Target t = targetOf(vm, args);
        const double x = num(vm, args[11]), y = num(vm, args[12]), w = num(vm, args[13]), h = num(vm, args[14]);
        const double rx = px(t, num(vm, args[15])) / 2, ry = px(t, num(vm, args[16])) / 2;
        text::Path p;
        if (args[17].truthy()) {
            roundPath(p, t.originX + px(t, x), t.originY + px(t, y), t.originX + px(t, x + w), t.originY + px(t, y + h), rx, ry);
        } else {
            const double unit = thickness(t), line = strokePixels(t, num(vm, args[18]));
            const text::Spread spread = text::spreadOf(line, unit);
            const double l = t.originX + px(t, x) - spread.before, tp = t.originY + px(t, y) - spread.before;
            const double r = t.originX + px(t, x + w) + unit + spread.after, b = t.originY + px(t, y + h) + unit + spread.after;
            roundPath(p, l, tp, r, b, rx + line / 2, ry + line / 2);
            if (r - l > 2 * line && b - tp > 2 * line) roundPath(p, l + line, tp + line, r - line, b - line, std::max(0.0, rx - line / 2), std::max(0.0, ry - line / 2), true);
        }
        fillPath(t, p, engineColor(vm, args[10]));
        return Value::null();
    });
    // oval(target..., colour, x, y, w, h, fill, lineWidth): the oval inside the rectangle; an
    // outline one unit thick covers w + 1 by h + 1, a wider one spreads around that.
    def("canvasOval", 17, 17, [](VM& vm, int, Value* args) -> Value {
        const Target t = targetOf(vm, args);
        const double x = num(vm, args[11]), y = num(vm, args[12]), w = num(vm, args[13]), h = num(vm, args[14]);
        text::Path p;
        if (args[15].truthy()) {
            const double l = t.originX + px(t, x), tp = t.originY + px(t, y), r = t.originX + px(t, x + w), b = t.originY + px(t, y + h);
            roundPath(p, l, tp, r, b, (r - l) / 2, (b - tp) / 2);
        } else {
            const double unit = thickness(t), line = strokePixels(t, num(vm, args[16]));
            const text::Spread spread = text::spreadOf(line, unit);
            const double l = t.originX + px(t, x) - spread.before, tp = t.originY + px(t, y) - spread.before;
            const double r = t.originX + px(t, x + w) + unit + spread.after, b = t.originY + px(t, y + h) + unit + spread.after;
            roundPath(p, l, tp, r, b, (r - l) / 2, (b - tp) / 2);
            if (r - l > 2 * line && b - tp > 2 * line) roundPath(p, l + line, tp + line, r - line, b - line, (r - l) / 2 - line, (b - tp) / 2 - line, true);
        }
        fillPath(t, p, engineColor(vm, args[10]));
        return Value::null();
    });
    // line(target..., colour, x1, y1, x2, y2, lineWidth)
    def("canvasLine", 16, 16, [](VM& vm, int, Value* args) -> Value {
        const Target t = targetOf(vm, args);
        text::Path p;
        linePath(t, p, num(vm, args[11]), num(vm, args[12]), num(vm, args[13]), num(vm, args[14]), strokePixels(t, num(vm, args[15])));
        fillPath(t, p, engineColor(vm, args[10]));
        return Value::null();
    });
    // polygon(target..., colour, xs, ys, n, fill, closed, lineWidth): filled, or its sides drawn
    // as lines.
    def("canvasPolygon", 17, 17, [](VM& vm, int, Value* args) -> Value {
        const Target t = targetOf(vm, args);
        if (!args[11].isObjType(ObjType::List) || !args[12].isObjType(ObjType::List)) throw vm.runtimeError(Thrown::ClassCastException, "a polygon's points are two lists");
        ObjList* xs = asList(args[11]);
        ObjList* ys = asList(args[12]);
        const int64_t n = std::min<int64_t>({vm.toInt(args[13], "count"), static_cast<int64_t>(xs->size()), static_cast<int64_t>(ys->size())});
        if (n < 2) return Value::null();
        text::Path p;
        auto at = [&](ObjList* list, int64_t i) { return num(vm, vm.listGet(list, static_cast<size_t>(i))); };
        if (args[14].truthy()) {
            for (int64_t i = 0; i < n; i++) {
                const float x = static_cast<float>(t.originX + px(t, at(xs, i))), y = static_cast<float>(t.originY + px(t, at(ys, i)));
                if (i == 0) p.moveTo(x, y);
                else p.lineTo(x, y);
            }
            p.close();
            // Each part filled once, however the sides cross: drawn one side at a time would
            // cover twice where they meet.
            fillPath(t, p, engineColor(vm, args[10]));
            return Value::null();
        }
        const double unit = thickness(t), width = strokePixels(t, num(vm, args[16]));
        if (width > unit) {
            // Wider than a unit: one shape of bands and round joins, so that where the lines
            // meet is covered once, through the middles of the points' pixels as a line is.
            const text::Spread spread = text::spreadOf(width, unit);
            const double shift = (spread.after - spread.before) / 2;
            std::vector<double> points;
            for (int64_t i = 0; i < n; i++) {
                points.push_back(t.originX + (at(xs, i) + 0.5) * t.scale + shift);
                points.push_back(t.originY + (at(ys, i) + 0.5) * t.scale + shift);
            }
            text::polylineStroke(p, points, args[15].truthy(), width);
            fillPath(t, p, engineColor(vm, args[10]));
            return Value::null();
        }
        const int64_t sides = args[15].truthy() ? n : n - 1;
        for (int64_t i = 0; i < sides; i++) {
            text::Path line;
            linePath(t, line, at(xs, i), at(ys, i), at(xs, (i + 1) % n), at(ys, (i + 1) % n));
            fillPath(t, line, engineColor(vm, args[10]));
        }
        return Value::null();
    });
    // arc(target..., colour, x, y, w, h, start, extent, kind, fill, lineWidth): an arc of the oval
    // in the rectangle (M8), filled as fillOval fills (a wedge for a pie, the arc and its chord
    // otherwise) or its outline drawn as drawOval draws, `lineWidth` units across.
    def("canvasArc", 20, 20, [](VM& vm, int, Value* args) -> Value {
        const Target t = targetOf(vm, args);
        const double x = num(vm, args[11]), y = num(vm, args[12]), w = num(vm, args[13]), h = num(vm, args[14]);
        const double start = num(vm, args[15]), extent = num(vm, args[16]);
        const auto kind = static_cast<text::ArcKind>(std::clamp<int64_t>(vm.toInt(args[17], "kind"), 0, 2));
        const double l = t.originX + px(t, x), tp = t.originY + px(t, y), r = t.originX + px(t, x + w), b = t.originY + px(t, y + h);
        text::Path p;
        if (args[18].truthy()) {
            text::arcFill(p, l, tp, r, b, start, extent, kind);
        } else {
            // On the line drawOval's outline runs along: half a unit in from its outer edge.
            const double unit = thickness(t), line = strokePixels(t, num(vm, args[19]));
            const text::Spread spread = text::spreadOf(line, unit);
            const double middle = unit / 2 + (spread.after - spread.before) / 2;
            text::arcStroke(p, l + middle, tp + middle, r + middle, b + middle, start, extent, kind, line);
        }
        fillPath(t, p, engineColor(vm, args[10]));
        return Value::null();
    });
    // text(target..., colour, font, text, x, baseline): the width it took, in pixels.
    def("canvasText", 15, 15, [](VM& vm, int, Value* args) -> Value {
        const Target t = targetOf(vm, args);
        auto font = fontOf(vm, args[11]);
        const std::string& s = vm.toStr(args[12], "text");
        const uint32_t color = engineColor(vm, args[10]);
        const int x = static_cast<int>(std::lround(t.originX + px(t, num(vm, args[13]))));
        const int baseline = static_cast<int>(std::lround(t.originY + px(t, num(vm, args[14]))));
        std::vector<text::PlacedGlyph> glyphs;
        const int total = font->place(s, glyphs);
        for (const text::PlacedGlyph& g : glyphs) {
            if (x + g.x > t.clipRight + 4096 || x + g.x + g.advance < t.clipLeft - 4096) continue;
            blendImage(t, g.font->image(g.glyph, color), x + g.x, baseline, color);
        }
        return Value::integer(total);
    });
    // picture(target..., from, fromWidth, fromHeight, sx, sy, sw, sh, x, y, w, h, alpha): a part
    // of another picture, its own pixels, put into a rectangle of units, smoothly when it is
    // made larger or smaller, see-through by `alpha` (0.0 to 1.0).
    def("canvasPicture", 22, 22, [](VM& vm, int, Value* args) -> Value {
        const Target t = targetOf(vm, args);
        const int64_t fw = vm.toInt(args[11], "width"), fh = vm.toInt(args[12], "height");
        const std::span<uint8_t> from = rgbaOf(vm, args[10], fw, fh);
        const double sx = num(vm, args[13]), sy = num(vm, args[14]), sw = num(vm, args[15]), sh = num(vm, args[16]);
        const double dl = t.originX + px(t, num(vm, args[17])), dt = t.originY + px(t, num(vm, args[18]));
        const double dw = px(t, num(vm, args[19])), dh = px(t, num(vm, args[20]));
        const double alpha = std::clamp(num(vm, args[21]), 0.0, 1.0);
        if (!(dw > 0) || !(dh > 0) || !(sw > 0) || !(sh > 0) || alpha == 0) return Value::null();
        const int l = std::max(t.clipLeft, static_cast<int>(std::floor(dl))), r = std::min(t.clipRight, static_cast<int>(std::ceil(dl + dw)));
        const int tp = std::max(t.clipTop, static_cast<int>(std::floor(dt))), b = std::min(t.clipBottom, static_cast<int>(std::ceil(dt + dh)));
        const bool same = std::abs(dw - sw) < 1e-9 && std::abs(dh - sh) < 1e-9 && dl == std::floor(dl) && dt == std::floor(dt) &&
                          sx == std::floor(sx) && sy == std::floor(sy);
        if (same) {
            // At its own size on whole pixels (2026-11-13): a row at a time, each pixel's bytes as
            // they are, laid over as below (blendPixel), its alpha scaled by `alpha` as below;
            // an opaque one copied. The same pixels as the way below gives, which took some 37 ns
            // a pixel through doubles, a channel at a time.
            const int64_t ox = static_cast<int64_t>(sx) - static_cast<int64_t>(dl), oy = static_cast<int64_t>(sy) - static_cast<int64_t>(dt);
            const int cl = static_cast<int>(std::max<int64_t>(l, -ox)), cr = static_cast<int>(std::min<int64_t>(r, fw - ox));
            for (int row = tp; row < b && cl < cr; row++) {
                const int64_t cy = row + oy;
                if (cy < 0 || cy >= fh) continue;
                const uint8_t* src = from.data() + (static_cast<size_t>(cy) * static_cast<size_t>(fw) + static_cast<size_t>(cl + ox)) * 4;
                uint8_t* dst = t.rgba + (static_cast<size_t>(row) * static_cast<size_t>(t.width) + static_cast<size_t>(cl)) * 4;
                for (int col = cl; col < cr; col++, src += 4, dst += 4) {
                    const uint32_t a = src[3];
                    if (alpha == 1.0) {
                        if (a == 255) std::memcpy(dst, src, 4);
                        else if (a != 0) blendPixel(dst, src[0], src[1], src[2], a);
                        continue;
                    }
                    const uint32_t sa = static_cast<uint32_t>(std::lround(a * alpha));
                    if (sa != 0) blendPixel(dst, src[0], src[1], src[2], std::min<uint32_t>(sa, 255));
                }
            }
            return Value::null();
        }
        auto sample = [&](int64_t cx, int64_t cy, int c) -> double {
            cx = std::clamp<int64_t>(cx, 0, fw - 1);
            cy = std::clamp<int64_t>(cy, 0, fh - 1);
            return from[(static_cast<size_t>(cy) * static_cast<size_t>(fw) + static_cast<size_t>(cx)) * 4 + static_cast<size_t>(c)];
        };
        for (int row = tp; row < b; row++) {
            for (int col = l; col < r; col++) {
                double rgbaOut[4];
                if (same) {
                    const int64_t cx = static_cast<int64_t>(sx) + (col - static_cast<int64_t>(dl)), cy = static_cast<int64_t>(sy) + (row - static_cast<int64_t>(dt));
                    if (cx < 0 || cy < 0 || cx >= fw || cy >= fh) continue;
                    for (int c = 0; c < 4; c++) rgbaOut[c] = sample(cx, cy, c);
                } else {
                    // Where the middle of this pixel falls in the picture, and the four pixels round
                    // it weighed by how near, their colours multiplied by their alpha.
                    const double u = sx + ((col + 0.5) - dl) / dw * sw - 0.5, v = sy + ((row + 0.5) - dt) / dh * sh - 0.5;
                    if (u < sx - 0.5 || v < sy - 0.5 || u > sx + sw - 0.5 || v > sy + sh - 0.5) continue;
                    const int64_t x0 = static_cast<int64_t>(std::floor(u)), y0 = static_cast<int64_t>(std::floor(v));
                    const double fx = u - x0, fy = v - y0;
                    double acc[4] = {0, 0, 0, 0};
                    const double weights[4] = {(1 - fx) * (1 - fy), fx * (1 - fy), (1 - fx) * fy, fx * fy};
                    const int64_t cxs[4] = {x0, x0 + 1, x0, x0 + 1}, cys[4] = {y0, y0, y0 + 1, y0 + 1};
                    for (int k = 0; k < 4; k++) {
                        const double a = sample(cxs[k], cys[k], 3) / 255.0;
                        for (int c = 0; c < 3; c++) acc[c] += sample(cxs[k], cys[k], c) * a * weights[k];
                        acc[3] += a * weights[k];
                    }
                    if (acc[3] <= 0) continue;
                    for (int c = 0; c < 3; c++) rgbaOut[c] = acc[c] / acc[3];
                    rgbaOut[3] = acc[3] * 255;
                }
                const uint32_t sa = static_cast<uint32_t>(std::lround(rgbaOut[3] * alpha));
                if (sa == 0) continue;
                blendPixel(t.rgba + (static_cast<size_t>(row) * static_cast<size_t>(t.width) + static_cast<size_t>(col)) * 4,
                           static_cast<uint32_t>(std::lround(std::clamp(rgbaOut[0], 0.0, 255.0))), static_cast<uint32_t>(std::lround(std::clamp(rgbaOut[1], 0.0, 255.0))),
                           static_cast<uint32_t>(std::lround(std::clamp(rgbaOut[2], 0.0, 255.0))), std::min<uint32_t>(sa, 255));
            }
        }
        return Value::null();
    });

    // ---- An Image's pixels as a whole ----

    // pixels(cells): a picture's pixels as new bytes, four to a pixel (red, green, blue, alpha).
    def("imagePixels", 1, 1, [](VM& vm, int, Value* args) -> Value {
        if (!args[0].isObjType(ObjType::List) || !asList(args[0])->packed()) throw vm.runtimeError(Thrown::ClassCastException, "expected a picture's cells");
        const ItemBlock& from = asList(args[0])->pack->block;
        vm.checkRoomFor(from.size());
        ObjBytes* made = vm.alloc<ObjBytes>();
        made->data.assign(from.begin(), from.end());
        vm.account(made->data.size());
        return Value::object(made);
    });
    // opaque(cells): whether every pixel of a picture has an alpha of 255 (Image.isOpaque). The
    // pixels ANDed together 32 bits at a time, which the compiler makes wide.
    def("imageOpaque", 1, 1, [](VM& vm, int, Value* args) -> Value {
        if (!args[0].isObjType(ObjType::List) || !asList(args[0])->packed()) throw vm.runtimeError(Thrown::ClassCastException, "expected a picture's cells");
        const ItemBlock& block = asList(args[0])->pack->block;
        const size_t pixels = block.size() / 4;
        const uint8_t* bytes = block.data();
        uint32_t all = 0xFFFFFFFFu;
        for (size_t start = 0; start < pixels && (all >> 24) == 0xFF; start += 65536) {  // a solid picture read whole, a see-through one left early
            const size_t end = std::min(pixels, start + 65536);
            uint32_t part = 0xFFFFFFFFu;
            for (size_t i = start; i < end; i++) {
                uint32_t px;
                std::memcpy(&px, bytes + i * 4, 4);
                part &= px;
            }
            all &= part;
        }
        return Value::boolean((all >> 24) == 0xFF);
    });
    // write(cells, width, height, to, at, order, rowStride, bottomUp): a picture's pixels written into
    // bytes from `at` (Image.writePixels): each pixel's channels in `order` ("BGR", "RGBA", any of
    // R, G, B and A in any order, 1 to 4 of them), a row every `rowStride` bytes, from the top row or
    // the bottom one first. What is between rows is left as it is.
    def("imageWrite", 8, 8, [](VM& vm, int, Value* args) -> Value {
        if (!args[0].isObjType(ObjType::List) || !asList(args[0])->packed() || !args[3].isObjType(ObjType::Bytes))
            throw vm.runtimeError(Thrown::ClassCastException, "expected a picture's cells and bytes");
        const ItemBlock& block = asList(args[0])->pack->block;
        const int64_t w = vm.toInt(args[1], "width"), h = vm.toInt(args[2], "height");
        std::vector<uint8_t>& to = asBytes(args[3])->data;
        const int64_t at = vm.toInt(args[4], "index");
        const std::string& order = vm.toStr(args[5], "order");
        const int64_t stride = vm.toInt(args[6], "rowStride");
        const bool bottomUp = args[7].truthy();
        const int64_t size = static_cast<int64_t>(order.size());
        if (size < 1 || size > 4) throw vm.runtimeError(Thrown::IllegalArgumentException, "an order is 1 to 4 of R, G, B and A, not \"" + order + "\"");
        int from[4] = {0, 0, 0, 0};
        for (int64_t c = 0; c < size; c++) {
            const char* found = std::strchr("RGBA", order[static_cast<size_t>(c)]);
            if (found == nullptr || order[static_cast<size_t>(c)] == 0)
                throw vm.runtimeError(Thrown::IllegalArgumentException, "an order is 1 to 4 of R, G, B and A, not \"" + order + "\"");
            from[c] = static_cast<int>(found - "RGBA");
        }
        if (w * h * 4 != static_cast<int64_t>(block.size())) throw vm.runtimeError(Thrown::IllegalArgumentException, "the size does not fit the picture");
        if (stride < w * size) throw vm.runtimeError(Thrown::IllegalArgumentException, "a row stride of " + std::to_string(stride) + " is less than a row's " + std::to_string(w * size) + " bytes");
        if (h > 0 && w > 0 && (at < 0 || at + (h - 1) * stride + w * size > static_cast<int64_t>(to.size())))
            throw vm.runtimeError(Thrown::IndexOutOfBoundsException, "the pixels would run past the end of the bytes");
        const uint8_t* source = block.data();
        for (int64_t row = 0; row < h; row++) {
            const uint8_t* p = source + row * w * 4;
            uint8_t* o = to.data() + at + (bottomUp ? h - 1 - row : row) * stride;
            if (order == "RGBA") {
                std::memcpy(o, p, static_cast<size_t>(w) * 4);
            } else if (order == "BGR") {
                for (int64_t x = 0; x < w; x++, o += 3, p += 4) {
                    o[0] = p[2];
                    o[1] = p[1];
                    o[2] = p[0];
                }
            } else if (order == "BGRA") {
                for (int64_t x = 0; x < w; x++, o += 4, p += 4) {
                    o[0] = p[2];
                    o[1] = p[1];
                    o[2] = p[0];
                    o[3] = p[3];
                }
            } else {
                for (int64_t x = 0; x < w; x++, o += size, p += 4)
                    for (int64_t c = 0; c < size; c++) o[c] = p[from[c]];
            }
        }
        return Value::null();
    });
    // setPixels(cells, bytes): a picture's pixels taken from bytes, four to a pixel.
    def("imageSetPixels", 2, 2, [](VM& vm, int, Value* args) -> Value {
        if (!args[0].isObjType(ObjType::List) || !asList(args[0])->packed() || !args[1].isObjType(ObjType::Bytes))
            throw vm.runtimeError(Thrown::ClassCastException, "expected a picture's cells and bytes");
        ItemBlock& to = asList(args[0])->pack->block;
        const std::vector<uint8_t>& from = asBytes(args[1])->data;
        if (to.size() != from.size()) throw vm.runtimeError(Thrown::IllegalArgumentException, "the bytes do not fit the picture");
        std::memcpy(to.data(), from.data(), to.size());
        return Value::null();
    });

    // lay(target..., cells, width, height, left, top, clear): a picture's pixels laid over the
    // target's at (left, top) in its pixels, inside the clip (N3: what a layer drew, a picture
    // drawn through a transform). With `clear`, what is there is cleared by each pixel's alpha
    // instead, as Java's Clear composite.
    def("canvasLay", 16, 16, [](VM& vm, int, Value* args) -> Value {
        const Target t = targetOf(vm, args);
        const int64_t w = vm.toInt(args[11], "width"), h = vm.toInt(args[12], "height");
        const std::span<uint8_t> from = rgbaOf(vm, args[10], w, h);
        const int64_t left = vm.toInt(args[13], "left"), top = vm.toInt(args[14], "top");
        const bool clear = args[15].truthy();
        const int64_t l = std::max<int64_t>(t.clipLeft, left), r = std::min<int64_t>(t.clipRight, left + w);
        const int64_t tp = std::max<int64_t>(t.clipTop, top), b = std::min<int64_t>(t.clipBottom, top + h);
        for (int64_t row = tp; row < b; row++) {
            const uint8_t* s = from.data() + (static_cast<size_t>(row - top) * static_cast<size_t>(w) + static_cast<size_t>(l - left)) * 4;
            uint8_t* d = t.rgba + (static_cast<size_t>(row) * static_cast<size_t>(t.width) + static_cast<size_t>(l)) * 4;
            for (int64_t col = l; col < r; col++, s += 4, d += 4) {
                if (s[3] == 0) continue;
                if (clear) d[3] = static_cast<uint8_t>((d[3] * (255u - s[3]) + 127) / 255);
                else blendPixel(d, s[0], s[1], s[2], s[3]);
            }
        }
        return Value::null();
    });
    // outline(font, text): the glyphs of a line of text as outlines, from the start of its
    // baseline in the font's pixels, y growing downwards (N3): a verb (0 move, 1 line, 2
    // quadratic, 3 cubic, 4 close) and then its points' x and y, one after another.
    def("textOutline", 2, 2, [](VM& vm, int, Value* args) -> Value {
        auto font = fontOf(vm, args[0]);
        const std::string& s = vm.toStr(args[1], "text");
        std::vector<text::PlacedGlyph> glyphs;
        font->place(s, glyphs);
        std::vector<Value> out;
        text::Path path;
        for (const text::PlacedGlyph& g : glyphs) {
            if (!g.font->outline(g.glyph, path)) continue;
            size_t p = 0;
            for (uint8_t verb : path.verbs) {
                out.push_back(Value::integer(verb));
                const int count = verb == text::Path::Move || verb == text::Path::Line ? 1 : verb == text::Path::Quad ? 2 : verb == text::Path::Cubic ? 3 : 0;
                for (int k = 0; k < count; k++, p += 2) {
                    out.push_back(Value::number(static_cast<double>(path.points[p]) + g.x));
                    out.push_back(Value::number(-static_cast<double>(path.points[p + 1])));
                }
            }
        }
        return listOf(vm, std::move(out));
    });
    // path(target..., colour, outline, evenOdd): a Path flattened into straight lines
    // (Path.flatten) and filled (N3), each contour closed. Not on whole pixels: a path is where
    // it says.
    def("canvasPath", 13, 13, [](VM& vm, int, Value* args) -> Value {
        const Target t = targetOf(vm, args);
        text::Path p;
        if (!text::pathOf(vm, args[11], t.originX, t.originY, t.scale, p)) return Value::null();
        fillPath(t, p, engineColor(vm, args[10]), args[12].truthy() ? text::FillRule::EvenOdd : text::FillRule::NonZero);
        return Value::null();
    });

    // takePixels(cells, bytes): a picture's pixels taken from new bytes, the bytes emptied. For
    // the standard library's decoders, whose bytes are nowhere else.
    def("imageTakePixels", 2, 2, [](VM& vm, int, Value* args) -> Value {
        if (!args[0].isObjType(ObjType::List) || !asList(args[0])->packed() || !args[1].isObjType(ObjType::Bytes))
            throw vm.runtimeError(Thrown::ClassCastException, "expected a picture's cells and bytes");
        ItemBlock& to = asList(args[0])->pack->block;
        std::vector<uint8_t>& from = asBytes(args[1])->data;
        if (to.size() != from.size()) throw vm.runtimeError(Thrown::IllegalArgumentException, "the bytes do not fit the picture");
        // Copied since 2026-11-15: a list's items are kept apart from bytes' (value.h, ItemBlock),
        // so the two cannot be swapped; the bytes are emptied, as nothing else holds them.
        if (!from.empty()) std::memcpy(to.data(), from.data(), from.size());
        std::vector<uint8_t>().swap(from);
        return Value::null();
    });

    vm.defineGlobal("__text", Value::object(lib));
    // What may be done in a parallel section: measuring, and finding a font to draw with, which
    // change nothing a program can see (the engine's tables have locks of their own, held for an
    // instant); and drawing on a canvas, which says itself what it may draw on (mayDraw).
    vm.markNotParallel("__text", {"width", "charWidth", "faceGlyph", "find", "faceInfo", "font", "fontInfo", "canvasFillRect", "canvasDrawRect",
                                  "canvasClear", "canvasRoundRect", "canvasOval", "canvasLine", "canvasPolygon", "canvasArc", "canvasText",
                                  "canvasPicture", "canvasPath", "canvasLay", "textOutline"});
}

}  // namespace ww
