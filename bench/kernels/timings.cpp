// docs/timings.html's operations written in plain C++, for its "100% C++" columns (2026-11-13):
// the same work on the same 4000 by 3000 photo, as a C++ program would do it by itself.
//
//   timings.bat, then timings.exe <the photo as raw RGBA bytes, 4000 by 3000>
//
// Plain loops over the bytes, one thread, no intrinsics (Clang may vectorise what it can), built
// as ww.exe is (clang-cl /O2). Each operation the best of three, in milliseconds, its result
// made new where WinterWolf's is (the allocation counted, as WinterWolf's is), on the performance cores.
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

using Clock = std::chrono::steady_clock;
using Bytes = std::vector<uint8_t>;
static volatile uint64_t sink;

template <typename F>
static void best(const char* group, const char* name, F&& work) {
    double b = 1e300;
    for (int r = 0; r < 3; r++) {
        const auto t = Clock::now();
        sink = sink + static_cast<uint64_t>(work());
        b = std::min(b, std::chrono::duration<double, std::milli>(Clock::now() - t).count());
    }
    std::printf("%s\t%s\t%.1f\n", group, name, b);
}

struct Image {
    int w = 0, h = 0;
    Bytes px;  // RGBA, four bytes a pixel, row after row
    Image() = default;
    Image(int width, int height) : w(width), h(height), px(static_cast<size_t>(width) * height * 4) {}
    uint8_t* at(int x, int y) { return &px[(static_cast<size_t>(y) * w + x) * 4]; }
    const uint8_t* at(int x, int y) const { return &px[(static_cast<size_t>(y) * w + x) * 4]; }
};

static uint8_t clampByte(double v) { return static_cast<uint8_t>(std::min(255.0, std::max(0.0, std::round(v)))); }

// ---- Pixel loops ----

static uint64_t readRed(const Image& im) {
    uint64_t t = 0;
    for (int y = 0; y < im.h; y++)
        for (int x = 0; x < im.w; x++) t += im.at(x, y)[0];
    return t;
}
static uint64_t readFour(const Image& im) {
    uint64_t t = 0;
    for (int y = 0; y < im.h; y++)
        for (int x = 0; x < im.w; x++) {
            const uint8_t* c = im.at(x, y);
            t += c[0] + c[1] + c[2] + c[3];
        }
    return t;
}
static uint64_t copyThenRead(const Image& im) {
    const Bytes list = im.px;
    uint64_t t = 0;
    for (size_t i = 0; i < list.size(); i += 4) t += list[i];
    return t;
}
static uint64_t mapRed(const Image& im) {
    Bytes reds(static_cast<size_t>(im.w) * im.h);
    for (size_t i = 0; i < reds.size(); i++) reds[i] = im.px[i * 4];
    return reds[reds.size() / 2];
}
static uint64_t writeAll(Image& im, const uint8_t c[4]) {
    for (int y = 0; y < im.h; y++)
        for (int x = 0; x < im.w; x++) std::memcpy(im.at(x, y), c, 4);
    return im.px[4];
}
static uint8_t grey(const uint8_t* c) { return static_cast<uint8_t>((c[0] * 54 + c[1] * 183 + c[2] * 19 + 128) >> 8); }
static uint64_t greyInPlace(Image& im) {
    for (int y = 0; y < im.h; y++)
        for (int x = 0; x < im.w; x++) {
            uint8_t* c = im.at(x, y);
            const uint8_t v = grey(c);
            c[0] = c[1] = c[2] = v;
        }
    return im.px[0];
}
static uint64_t greyMapped(const Image& im) {
    Image out(im.w, im.h);
    for (size_t i = 0; i < im.px.size(); i += 4) {
        const uint8_t v = grey(&im.px[i]);
        out.px[i] = out.px[i + 1] = out.px[i + 2] = v;
        out.px[i + 3] = im.px[i + 3];
    }
    return out.px[0];
}
static Image mapChannels(const Image& im, const uint8_t* r, const uint8_t* g, const uint8_t* b) {
    Image out(im.w, im.h);
    for (size_t i = 0; i < im.px.size(); i += 4) {
        out.px[i] = r[im.px[i]];
        out.px[i + 1] = g[im.px[i + 1]];
        out.px[i + 2] = b[im.px[i + 2]];
        out.px[i + 3] = im.px[i + 3];
    }
    return out;
}

// ---- Whole pictures ----

static Image crop(const Image& im, int x0, int y0, int w, int h) {
    Image out(w, h);
    for (int y = 0; y < h; y++) std::memcpy(out.at(0, y), im.at(x0, y0 + y), static_cast<size_t>(w) * 4);
    return out;
}
// Each pixel's colour times a 3 by 3 matrix, in floats, rounded; alpha kept (greyscale, saturation).
static Image mixed(const Image& im, const double m[9]) {
    Image out(im.w, im.h);
    for (size_t i = 0; i < im.px.size(); i += 4) {
        const double r = im.px[i], g = im.px[i + 1], b = im.px[i + 2];
        out.px[i] = clampByte(m[0] * r + m[1] * g + m[2] * b);
        out.px[i + 1] = clampByte(m[3] * r + m[4] * g + m[5] * b);
        out.px[i + 2] = clampByte(m[6] * r + m[7] * g + m[8] * b);
        out.px[i + 3] = im.px[i + 3];
    }
    return out;
}
// One pass of a convolution along rows (or columns), the colours weighed by alpha, the edge
// pixels repeated beyond the edges, the answer rounded to bytes.
static Image convolve(const Image& im, const std::vector<double>& k, bool down) {
    Image out(im.w, im.h);
    const int reach = static_cast<int>(k.size() / 2);
    for (int y = 0; y < im.h; y++)
        for (int x = 0; x < im.w; x++) {
            double r = 0, g = 0, b = 0, a = 0;
            for (int t = -reach; t <= reach; t++) {
                const int sx = down ? x : std::clamp(x + t, 0, im.w - 1);
                const int sy = down ? std::clamp(y + t, 0, im.h - 1) : y;
                const uint8_t* c = im.at(sx, sy);
                const double wa = k[static_cast<size_t>(t + reach)] * c[3];
                r += wa * c[0];
                g += wa * c[1];
                b += wa * c[2];
                a += wa;
            }
            uint8_t* o = out.at(x, y);
            if (a > 0) {
                o[0] = clampByte(r / a);
                o[1] = clampByte(g / a);
                o[2] = clampByte(b / a);
            } else {
                o[0] = o[1] = o[2] = 0;
            }
            o[3] = clampByte(a);
        }
    return out;
}
static Image blurred(const Image& im, double s) {
    const int reach = static_cast<int>(std::ceil(3 * s));
    std::vector<double> k(static_cast<size_t>(2 * reach + 1));
    double total = 0;
    for (int t = -reach; t <= reach; t++) total += k[static_cast<size_t>(t + reach)] = std::exp(-(t * t) / (2 * s * s));
    for (double& v : k) v /= total;
    return convolve(convolve(im, k, false), k, true);
}
// Half the size each way: each new pixel the average of the four it covers, weighed by alpha.
static Image halved(const Image& im) {
    Image out(im.w / 2, im.h / 2);
    for (int y = 0; y < out.h; y++)
        for (int x = 0; x < out.w; x++) {
            double r = 0, g = 0, b = 0, a = 0;
            for (int dy = 0; dy < 2; dy++)
                for (int dx = 0; dx < 2; dx++) {
                    const uint8_t* c = im.at(2 * x + dx, 2 * y + dy);
                    r += c[0] * c[3];
                    g += c[1] * c[3];
                    b += c[2] * c[3];
                    a += c[3];
                }
            uint8_t* o = out.at(x, y);
            o[0] = a > 0 ? clampByte(r / a) : 0;
            o[1] = a > 0 ? clampByte(g / a) : 0;
            o[2] = a > 0 ? clampByte(b / a) : 0;
            o[3] = clampByte(a / 4);
        }
    return out;
}
// A colour read between pixels, bilinear, weighed by alpha; clear beyond the picture where
// `clear`, else its edge pixels repeated.
static void sample(const Image& im, double fx, double fy, bool clear, uint8_t* o) {
    const int x0 = static_cast<int>(std::floor(fx)), y0 = static_cast<int>(std::floor(fy));
    const double ax = fx - x0, ay = fy - y0;
    double r = 0, g = 0, b = 0, a = 0;
    for (int dy = 0; dy < 2; dy++)
        for (int dx = 0; dx < 2; dx++) {
            int sx = x0 + dx, sy = y0 + dy;
            if (sx < 0 || sy < 0 || sx >= im.w || sy >= im.h) {
                if (clear) continue;
                sx = std::clamp(sx, 0, im.w - 1);
                sy = std::clamp(sy, 0, im.h - 1);
            }
            const double wgt = (dx ? ax : 1 - ax) * (dy ? ay : 1 - ay);
            const uint8_t* c = im.at(sx, sy);
            const double wa = wgt * c[3];
            r += wa * c[0];
            g += wa * c[1];
            b += wa * c[2];
            a += wa;
        }
    o[0] = a > 0 ? clampByte(r / a) : 0;
    o[1] = a > 0 ? clampByte(g / a) : 0;
    o[2] = a > 0 ? clampByte(b / a) : 0;
    o[3] = clampByte(a);
}
static Image scaledUp(const Image& im, int w, int h) {
    Image out(w, h);
    const double sx = static_cast<double>(im.w) / w, sy = static_cast<double>(im.h) / h;
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) sample(im, (x + 0.5) * sx - 0.5, (y + 0.5) * sy - 0.5, false, out.at(x, y));
    return out;
}
// Turned by `angle` about (cx, cy), in a new image just large enough to hold it.
static Image rotated(const Image& im, double angle, double cx, double cy) {
    const double c = std::cos(angle), s = std::sin(angle);
    double minX = 1e300, minY = 1e300, maxX = -1e300, maxY = -1e300;
    for (int k = 0; k < 4; k++) {
        const double px = (k % 2 == 0 ? 0 : im.w) - cx, py = (k < 2 ? 0 : im.h) - cy;
        const double x = c * px - s * py + cx, y = s * px + c * py + cy;
        minX = std::min(minX, x), minY = std::min(minY, y), maxX = std::max(maxX, x), maxY = std::max(maxY, y);
    }
    const double left = std::floor(minX), top = std::floor(minY);
    Image out(static_cast<int>(std::ceil(maxX) - left), static_cast<int>(std::ceil(maxY) - top));
    for (int y = 0; y < out.h; y++)
        for (int x = 0; x < out.w; x++) {
            const double px = left + x + 0.5 - cx, py = top + y + 0.5 - cy;
            sample(im, c * px + s * py + cx - 0.5, -s * px + c * py + cy - 0.5, true, out.at(x, y));
        }
    return out;
}
static uint64_t histogram(const Image& im) {
    std::vector<int64_t> levels(1024, 0);
    for (size_t i = 0; i < im.px.size(); i += 4) {
        levels[im.px[i]]++;
        levels[256 + im.px[i + 1]]++;
        levels[512 + im.px[i + 2]]++;
        levels[768 + im.px[i + 3]]++;
    }
    return static_cast<uint64_t>(levels[128]);
}
// The pixels joined to (x, y) through neighbours whose colour is within `tolerance` of its own in
// every part: a mask, filled from a stack.
static uint64_t flood(const Image& im, int x, int y, int tolerance) {
    std::vector<uint8_t> mask(static_cast<size_t>(im.w) * im.h, 0);
    const uint8_t* seed = im.at(x, y);
    auto within = [&](const uint8_t* c) {
        for (int k = 0; k < 4; k++)
            if (std::abs(c[k] - seed[k]) > tolerance) return false;
        return true;
    };
    std::vector<int> stack{y * im.w + x};
    mask[static_cast<size_t>(stack[0])] = 1;
    uint64_t count = 0;
    while (!stack.empty()) {
        const int p = stack.back();
        stack.pop_back();
        count++;
        const int px = p % im.w, py = p / im.w;
        const int next[4][2] = {{px - 1, py}, {px + 1, py}, {px, py - 1}, {px, py + 1}};
        for (const auto& n : next) {
            if (n[0] < 0 || n[1] < 0 || n[0] >= im.w || n[1] >= im.h) continue;
            const int q = n[1] * im.w + n[0];
            if (mask[static_cast<size_t>(q)] || !within(im.at(n[0], n[1]))) continue;
            mask[static_cast<size_t>(q)] = 1;
            stack.push_back(q);
        }
    }
    return count;
}
// `over` laid on `im` at (x, y) by its alpha times `opacity`, source over.
static uint64_t blend(Image& im, const Image& over, int x0, int y0, double opacity) {
    for (int y = 0; y < over.h && y0 + y < im.h; y++)
        for (int x = 0; x < over.w && x0 + x < im.w; x++) {
            const uint8_t* s = over.at(x, y);
            uint8_t* d = im.at(x0 + x, y0 + y);
            const double a = s[3] / 255.0 * opacity, da = d[3] / 255.0;
            const double outA = a + da * (1 - a);
            for (int k = 0; k < 3; k++) d[k] = outA > 0 ? clampByte((s[k] * a + d[k] * da * (1 - a)) / outA) : 0;
            d[3] = clampByte(outA * 255);
        }
    return im.px[0];
}

// ---- Conversions and files ----

static uint64_t toPlanes(const Image& im) {
    const size_t n = static_cast<size_t>(im.w) * im.h;
    std::vector<Bytes> planes(4, Bytes(n));
    for (size_t i = 0; i < n; i++)
        for (int k = 0; k < 4; k++) planes[static_cast<size_t>(k)][i] = im.px[i * 4 + static_cast<size_t>(k)];
    return planes[0][n / 2];
}
static uint64_t fromPlanes(const std::vector<Bytes>& planes, int w, int h) {
    Image out(w, h);
    const size_t n = static_cast<size_t>(w) * h;
    for (size_t i = 0; i < n; i++)
        for (int k = 0; k < 4; k++) out.px[i * 4 + static_cast<size_t>(k)] = planes[static_cast<size_t>(k)][i];
    return out.px[4];
}
static uint64_t encodeBmp(const Image& im) {
    bool seeThrough = false;
    for (size_t i = 3; i < im.px.size(); i += 4) seeThrough = seeThrough || im.px[i] != 255;
    const int bits = seeThrough ? 32 : 24, size = bits / 8;
    const size_t stride = (static_cast<size_t>(im.w) * bits + 31) / 32 * 4, at = 14 + (seeThrough ? 108 : 40);
    Bytes out(at + stride * im.h);
    out[0] = 'B', out[1] = 'M';  // the rest of the header as WinterWolf's; its cost is nothing beside the pixels
    for (int row = 0; row < im.h; row++) {
        uint8_t* o = &out[at + static_cast<size_t>(im.h - 1 - row) * stride];
        const uint8_t* p = im.at(0, row);
        for (int x = 0; x < im.w; x++, o += size, p += 4) {
            o[0] = p[2], o[1] = p[1], o[2] = p[0];
            if (seeThrough) o[3] = p[3];
        }
    }
    return out[at];
}

// ---- Drawing ----

static uint64_t fillRects(Image& im, int n, int w, int h, int step) {
    const uint8_t black[4] = {0, 0, 0, 255};
    for (int i = 0; i < n; i++) {
        const int x0 = step ? i % 3900 : 50 + i, y0 = step ? (i * 7) % 2900 : 50 + i;
        for (int y = y0; y < y0 + h && y < im.h; y++)
            for (int x = x0; x < x0 + w && x < im.w; x++) std::memcpy(im.at(x, y), black, 4);
    }
    return im.px[0];
}
// A picture laid on the image at its own size, on whole pixels: an opaque one copied a row at a
// time, a see-through one blended source over, in whole numbers.
static void drawPicture(Image& im, const Image& p, int x0, int y0) {
    bool opaque = true;
    for (size_t i = 3; i < p.px.size() && opaque; i += 4) opaque = p.px[i] == 255;
    const int w = std::min(p.w, im.w - x0), h = std::min(p.h, im.h - y0);
    for (int y = 0; y < h; y++) {
        if (opaque) {
            std::memcpy(im.at(x0, y0 + y), p.at(0, y), static_cast<size_t>(w) * 4);
            continue;
        }
        for (int x = 0; x < w; x++) {
            const uint8_t* s = p.at(x, y);
            uint8_t* d = im.at(x0 + x, y0 + y);
            const int a = s[3], da = d[3] * (255 - a) / 255, outA = a + da;
            for (int k = 0; k < 3; k++) d[k] = static_cast<uint8_t>(outA > 0 ? (s[k] * a + d[k] * da + outA / 2) / outA : 0);
            d[3] = static_cast<uint8_t>(outA);
        }
    }
}

int main(int argc, char** argv) {
    SetProcessAffinityMask(GetCurrentProcess(), 0xFFFF);  // the performance cores, as bench/pinned.ps1
    if (argc < 2) {
        std::fprintf(stderr, "give the photo as raw RGBA bytes, 4000 by 3000\n");
        return 1;
    }
    Image photo(4000, 3000);
    FILE* f = std::fopen(argv[1], "rb");
    if (f == nullptr || std::fread(photo.px.data(), 1, photo.px.size(), f) != photo.px.size()) {
        std::fprintf(stderr, "cannot read %s\n", argv[1]);
        return 1;
    }
    std::fclose(f);

    Bytes bytes = photo.px;
    best("yardstick", "bytes: read red", [&] {
        uint64_t t = 0;
        for (size_t i = 0; i < bytes.size(); i += 4) t += bytes[i];
        return t;
    });
    best("yardstick", "bytes: greyscale in place", [&] {
        for (size_t i = 0; i < bytes.size(); i += 4) bytes[i] = bytes[i + 1] = bytes[i + 2] = grey(&bytes[i]);
        return bytes[0];
    });

    best("read", "image[x, y].red", [&] { return readRed(photo); });
    best("read", "image[x, y], four parts", [&] { return readFour(photo); });
    best("read", "pixels.toList() then for", [&] { return copyThenRead(photo); });
    best("read", "pixels.map { it.red }", [&] { return mapRed(photo); });

    Image canvasImage(4000, 3000);
    const uint8_t red[4] = {255, 0, 0, 255};
    best("write", "image[x, y] = c", [&] { return writeAll(canvasImage, red); });

    Image work = photo;
    best("change", "greyscale in place", [&] { return greyInPlace(work); });
    best("change", "greyscale into a new grid", [&] { return greyMapped(work); });
    uint8_t invert[256];
    for (int v = 0; v < 256; v++) invert[v] = static_cast<uint8_t>(255 - v);
    best("change", "mapChannels(t, t, t)", [&] { return mapChannels(photo, invert, invert, invert).px[0]; });

    best("whole", "copy()", [&] { return Image(photo).px[0]; });
    best("whole", "crop(1000, 1000, 2000, 1000)", [&] { return crop(photo, 1000, 1000, 2000, 1000).px[0]; });
    const double greyM[9] = {0.2126, 0.7152, 0.0722, 0.2126, 0.7152, 0.0722, 0.2126, 0.7152, 0.0722};
    best("whole", "greyscale()", [&] { return mixed(photo, greyM).px[0]; });
    best("whole", "inverted()", [&] { return mapChannels(photo, invert, invert, invert).px[0]; });
    best("whole", "adjusted(1.1, 1.2, 0.8)", [&] {
        uint8_t table[256];
        for (int v = 0; v < 256; v++) table[v] = clampByte((((v / 255.0) * 1.1 - 0.5) * 1.2 + 0.5) * 255.0);
        const double sat = 0.8;
        const double m[9] = {0.213 + 0.787 * sat, 0.715 - 0.715 * sat, 0.072 - 0.072 * sat, 0.213 - 0.213 * sat, 0.715 + 0.285 * sat,
                             0.072 - 0.072 * sat, 0.213 - 0.213 * sat, 0.715 - 0.715 * sat, 0.072 + 0.928 * sat};
        return mixed(mapChannels(photo, table, table, table), m).px[0];
    });
    best("whole", "blurred(3)", [&] { return blurred(photo, 3).px[0]; });
    best("whole", "scaled(2000, 1500)", [&] { return halved(photo).px[0]; });
    best("whole", "scaled(8000, 6000)", [&] { return scaledUp(photo, 8000, 6000).px[0]; });
    best("whole", "transformed(rotation(0.3))", [&] { return rotated(photo, 0.3, 2000, 1500).px[0]; });
    best("whole", "histogram()", [&] { return histogram(photo); });
    best("whole", "floodRegion(10, 10, 20)", [&] { return flood(photo, 10, 10, 20); });
    const Image overlay = crop(photo, 0, 0, 2000, 1500);
    best("whole", "blend 2000x1500 over, half", [&] { return blend(work, overlay, 500, 500, 0.5); });

    best("io", "rgbaBytes()", [&] { return Bytes(photo.px)[0]; });
    best("io", "Image.of(w, h, bytes)", [&] { return Image(photo).px[0]; });
    best("io", "toPlanes()", [&] { return toPlanes(photo); });
    std::vector<Bytes> planes(4, Bytes(static_cast<size_t>(photo.w) * photo.h));
    best("io", "fromPlanes()", [&] { return fromPlanes(planes, photo.w, photo.h); });
    best("io", "toTensor()", [&] { return Bytes(photo.px)[0]; });
    best("io", "encode BMP", [&] { return encodeBmp(photo); });

    Image board(4000, 3000);
    best("draw", "10,000 fillRect 100x100", [&] { return fillRects(board, 10000, 100, 100, 1); });
    const Image small = crop(photo, 0, 0, 300, 300);
    best("draw", "200 draw(picture 300x300)", [&] {
        for (int i = 0; i < 200; i++) drawPicture(board, small, i % 3700, (i * 7) % 2700);
        return board.px[0];
    });
    for (int s : {10, 100, 300, 1000}) {
        Image pic(s, s), see(s, s);
        for (size_t i = 0; i < pic.px.size(); i += 4) {
            pic.px[i] = see.px[i] = 10, pic.px[i + 1] = see.px[i + 1] = 20, pic.px[i + 2] = see.px[i + 2] = 30;
            pic.px[i + 3] = 255, see.px[i + 3] = 128;
        }
        char name[64];
        std::snprintf(name, sizeof name, "100 pictures %dx%d", s, s);
        best("draw-size", name, [&] {
            for (int i = 0; i < 100; i++) drawPicture(board, pic, 50 + i, 50 + i);
            return board.px[0];
        });
        std::snprintf(name, sizeof name, "100 pictures %dx%d, half see-through", s, s);
        best("draw-size", name, [&] {
            for (int i = 0; i < 100; i++) drawPicture(board, see, 50 + i, 50 + i);
            return board.px[0];
        });
    }
    best("draw-size", "100 fillRect 300x300", [&] { return fillRects(board, 100, 300, 300, 0); });
    return 0;
}
