// Turning an outline into a picture: see raster.h.

#include "raster.h"

#include <algorithm>
#include <cmath>

#include "../section_window.h"  // last: this file's own code goes in its section (D161)

namespace astra::text {

namespace {

// Accumulates exact area coverage: each edge adds, along its length, how much of every pixel
// lies to its right. A running sum along each row then gives the coverage of every pixel.
// (The method of font-rs. Nothing here depends on which way a contour winds.)
//
// Only the rows from `from` up to `to` are kept: a shape drawn inside a clip is worked out for
// the clip's rows alone. An edge is still followed from its first row, so every row kept comes
// out exactly as it would with all of them. Each row has a cell past its last pixel for what
// lands to the right of the picture, and its sum starts from nothing.
class Accumulator {
public:
    Accumulator(int width, int height, int from, int to)
        : w_(width), h_(height), from_(from), to_(to), a_(static_cast<size_t>(width + 1) * static_cast<size_t>(to - from), 0.0f) {}

    void line(float x0, float y0, float x1, float y1) {
        if (y0 == y1) return;
        float dir = 1.0f;
        if (y0 > y1) {
            std::swap(x0, x1);
            std::swap(y0, y1);
            dir = -1.0f;
        }
        const float dxdy = (x1 - x0) / (y1 - y0);
        float x = x0;
        if (y0 < 0) x -= y0 * dxdy;

        const int firstRow = std::max(0, static_cast<int>(std::floor(y0)));
        const int lastRow = std::min({h_, to_, static_cast<int>(std::ceil(y1))});
        for (int row = firstRow; row < lastRow; row++) {
            const float dy = std::min(static_cast<float>(row + 1), y1) - std::max(static_cast<float>(row), y0);
            const float xNext = x + dxdy * dy;
            if (row < from_) {
                x = xNext;  // above the rows kept: only where the edge has got to
                continue;
            }
            const size_t start = static_cast<size_t>(row - from_) * static_cast<size_t>(w_ + 1);
            const float d = dy * dir;
            const float left = std::min(x, xNext), right = std::max(x, xNext);
            const float leftFloor = std::floor(left);
            const int leftCell = static_cast<int>(leftFloor);
            const float rightCeil = std::ceil(right);
            const int rightCell = static_cast<int>(rightCeil);

            if (rightCell <= leftCell + 1) {
                const float mid = 0.5f * (x + xNext) - leftFloor;
                add(start, leftCell, d - d * mid);
                add(start, leftCell + 1, d * mid);
            } else {
                const float s = 1.0f / (right - left);
                const float leftPart = left - leftFloor;
                const float a0 = 0.5f * s * (1.0f - leftPart) * (1.0f - leftPart);
                const float rightPart = right - rightCeil + 1.0f;
                const float am = 0.5f * s * rightPart * rightPart;
                add(start, leftCell, d * a0);
                if (rightCell == leftCell + 2) {
                    add(start, leftCell + 1, d * (1.0f - a0 - am));
                } else {
                    const float a1 = s * (1.5f - leftPart);
                    add(start, leftCell + 1, d * (a1 - a0));
                    for (int cell = leftCell + 2; cell < rightCell - 1; cell++) add(start, cell, d * s);
                    const float a2 = a1 + static_cast<float>(rightCell - leftCell - 3) * s;
                    add(start, rightCell - 1, d * (1.0f - a2 - am));
                }
                add(start, rightCell, d * am);
            }
            x = xNext;
        }
    }

    // The kept rows' coverage, a byte a pixel, row after row.
    void finish(std::vector<uint8_t>& out) const {
        const size_t w = static_cast<size_t>(w_);
        out.resize(w * static_cast<size_t>(to_ - from_));
        for (size_t row = 0; row < static_cast<size_t>(to_ - from_); row++) {
            const float* in = a_.data() + row * (w + 1);
            uint8_t* to = out.data() + row * w;
            float sum = 0.0f;
            for (size_t i = 0; i < w; i++) {
                sum += in[i];
                const float coverage = std::min(std::fabs(sum), 1.0f);
                to[i] = static_cast<uint8_t>(coverage * 255.0f + 0.5f);
            }
        }
    }

private:
    int w_, h_, from_, to_;
    std::vector<float> a_;

    void add(size_t rowStart, int cell, float amount) {
        if (cell < 0) cell = 0;
        if (cell > w_) cell = w_;
        a_[rowStart + static_cast<size_t>(cell)] += amount;
    }
};

// Walks an outline as straight pieces, curves cut into as many as their bend needs, times
// `fineness` squared. Every contour is closed, whether or not the outline says so.
template <class Line>
void flatten(const Path& path, float fineness, Line&& line) {
    const float most = std::sqrt(fineness);
    float startX = 0, startY = 0, x = 0, y = 0;
    bool open = false;
    size_t p = 0;
    auto closeContour = [&] {
        if (open && (x != startX || y != startY)) line(x, y, startX, startY);
        open = false;
    };
    for (uint8_t verb : path.verbs) {
        const float* pt = path.points.data() + p;
        switch (verb) {
            case Path::Move:
                closeContour();
                startX = x = pt[0];
                startY = y = pt[1];
                open = true;
                p += 2;
                break;
            case Path::Line:
                line(x, y, pt[0], pt[1]);
                x = pt[0];
                y = pt[1];
                p += 2;
                break;
            case Path::Quad: {
                const float cx = pt[0], cy = pt[1], ex = pt[2], ey = pt[3];
                const float bend = std::hypot(x - 2 * cx + ex, y - 2 * cy + ey);
                const int pieces = std::clamp(static_cast<int>(std::ceil(std::sqrt(bend * 3.0f * fineness))), 1, static_cast<int>(32 * most));
                float px = x, py = y;
                for (int i = 1; i <= pieces; i++) {
                    const float t = static_cast<float>(i) / static_cast<float>(pieces), u = 1.0f - t;
                    const float nx = u * u * x + 2 * u * t * cx + t * t * ex;
                    const float ny = u * u * y + 2 * u * t * cy + t * t * ey;
                    line(px, py, nx, ny);
                    px = nx;
                    py = ny;
                }
                x = ex;
                y = ey;
                p += 4;
                break;
            }
            case Path::Cubic: {
                const float ax = pt[0], ay = pt[1], bx = pt[2], by = pt[3], ex = pt[4], ey = pt[5];
                const float bend = std::max(std::hypot(x - 2 * ax + bx, y - 2 * ay + by), std::hypot(ax - 2 * bx + ex, ay - 2 * by + ey));
                const int pieces = std::clamp(static_cast<int>(std::ceil(std::sqrt(bend * 4.5f * fineness))), 1, static_cast<int>(48 * most));
                float px = x, py = y;
                for (int i = 1; i <= pieces; i++) {
                    const float t = static_cast<float>(i) / static_cast<float>(pieces), u = 1.0f - t;
                    const float nx = u * u * u * x + 3 * u * u * t * ax + 3 * u * t * t * bx + t * t * t * ex;
                    const float ny = u * u * u * y + 3 * u * u * t * ay + 3 * u * t * t * by + t * t * t * ey;
                    line(px, py, nx, ny);
                    px = nx;
                    py = ny;
                }
                x = ex;
                y = ey;
                p += 6;
                break;
            }
            case Path::Close:
                closeContour();
                x = startX;
                y = startY;
                break;
        }
    }
    closeContour();
}

}  // namespace

void rasterizeInto(const Path& path, int left, int top, int width, int height, std::vector<uint8_t>& coverage, float fineness) {
    coverage.assign(static_cast<size_t>(std::max(width, 0)) * static_cast<size_t>(std::max(height, 0)), 0);
    if (path.empty() || width <= 0 || height <= 0) return;
    Accumulator acc(width, height, 0, height);
    const float originX = static_cast<float>(left), originY = static_cast<float>(top);
    flatten(path, fineness, [&](float x0, float y0, float x1, float y1) { acc.line(x0 - originX, originY - y0, x1 - originX, originY - y1); });
    acc.finish(coverage);
}

GlyphImage rasterize(const Path& path, float fineness) {
    GlyphImage image;
    float minX, minY, maxX, maxY;
    if (!path.bounds(minX, minY, maxX, maxY)) return image;
    image.left = static_cast<int>(std::floor(minX));
    image.top = static_cast<int>(std::ceil(maxY));
    image.width = static_cast<int>(std::ceil(maxX)) - image.left + 1;
    image.height = image.top - static_cast<int>(std::floor(minY)) + 1;
    if (image.width <= 0 || image.height <= 0 || image.width > 8192 || image.height > 8192) return GlyphImage{};
    rasterizeInto(path, image.left, image.top, image.width, image.height, image.pixels, fineness);
    return image;
}

GlyphImage rasterize(const Path& path, float fineness, int fromRow, int toRow) {
    GlyphImage image;
    float minX, minY, maxX, maxY;
    if (!path.bounds(minX, minY, maxX, maxY)) return image;
    // Far beyond any picture: nothing to draw, and nothing to overflow an int.
    const float far = 1 << 24;
    if (!(minX > -far && maxX < far && minY > -far && maxY < far)) return image;
    image.left = static_cast<int>(std::floor(minX));
    image.top = static_cast<int>(std::ceil(maxY));
    image.width = static_cast<int>(std::ceil(maxX)) - image.left + 1;
    const int height = image.top - static_cast<int>(std::floor(minY)) + 1;
    // The rows kept, counted from the image's top: row r lands on the picture's row r - top.
    const int from = std::max(0, fromRow + image.top), to = std::min(height, toRow + image.top);
    if (image.width <= 0 || image.width > 8192 || from >= to || to - from > 8192) return GlyphImage{};
    Accumulator acc(image.width, height, from, to);
    const float originX = static_cast<float>(image.left), originY = static_cast<float>(image.top);
    flatten(path, fineness, [&](float x0, float y0, float x1, float y1) { acc.line(x0 - originX, originY - y0, x1 - originX, originY - y1); });
    acc.finish(image.pixels);
    image.top -= from;
    image.height = to - from;
    return image;
}

}  // namespace astra::text
