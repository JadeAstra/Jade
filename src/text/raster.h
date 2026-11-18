// The text engine: turning an outline into a picture.
#pragma once

#include <cstdint>
#include <functional>
#include <vector>

#include "face.h"

namespace ww::text {

// A small picture of one glyph and where it sits. With the pen on the baseline at (x, y),
// y growing downwards, its top-left pixel goes at (x + left, y - top).
//
// An ordinary glyph is coverage: one byte for each pixel, saying how much of the pixel the
// glyph covers, to be painted in whatever colour the text has. A colour glyph (an emoji) has
// colours of its own: four bytes for each pixel, red, green, blue and alpha, with the colours
// already multiplied by the alpha.
struct GlyphImage {
    int width = 0, height = 0;
    int left = 0, top = 0;
    bool color = false;
    std::vector<uint8_t> pixels;

    bool empty() const { return width == 0 || height == 0; }
};

// How finely curves are cut into straight pieces (raster.cpp's flatten): a glyph's, small, to
// within a sixth of a pixel at most (a twelfth for TrueType's quadratic curves: a piece strays
// at most a quarter of the curve's bend over the pieces squared, a cubic's at most three
// quarters); a shape's, which may be large, nine times as closely, so that an oval's pixels add
// up to its area.
constexpr float kGlyphFineness = 1.0f;
constexpr float kShapeFineness = 9.0f;

// Which parts of an outline whose contours cross or nest are inside (N3): with NonZero, a part
// that the contours go round more often one way than the other, so a contour inside another
// going the same way fills too; with EvenOdd, a part inside an odd number of them, so a contour
// inside another makes a hole whichever way it goes.
enum class FillRule : uint8_t { NonZero, EvenOdd };

// Fills an outline that is already in pixels (y still growing upwards), with exact-area
// antialiasing: each pixel's value is the share of it that lies inside the outline.
GlyphImage rasterize(const Path& path, float fineness = kGlyphFineness, FillRule rule = FillRule::NonZero);

// The same with only the rows that land from `fromRow` up to `toRow` of a picture (y growing
// downwards, the image placed with its top at -top, as blendImage places a shape): a clip's rows.
// Each comes out exactly as rasterize gives it, so a shape drawn in parts matches one drawn
// whole, and a big shape inside a small clip costs only the clip's rows. A shape taller than
// 8192 pixels is drawn, its rows in the clip; one wider than that, or with more rows in the clip,
// is not.
GlyphImage rasterize(const Path& path, float fineness, int fromRow, int toRow, FillRule rule = FillRule::NonZero);

// The same, a row at a time, as each row's coverage is worked out: span(row, left, count,
// coverage) for each row that has any, `row` and `left` the picture's (y growing downwards, as
// blendImage places the image), `coverage` the bytes of its `count` pixels from `left`; the
// pixels of the row outside them covered by nothing. Every byte as rasterize(path, fineness,
// fromRow, toRow, rule) gives it, but only the cells the outline's edges reach in each row are
// worked on: a slanted line across a big picture costs its length, not its box.
using RowSpan = std::function<void(int row, int left, int count, const uint8_t* coverage)>;
void rasterizeRows(const Path& path, float fineness, int fromRow, int toRow, FillRule rule, const RowSpan& span);
// Whether rasterizeRows takes the outline as it is (2026-11-13): not wider than 8192 pixels, nor
// with more rows than that from `fromRow` to `toRow`, nor reaching 2^24 pixels out. One with a
// point that is no number (NaN, infinite) is taken, and drawn as nothing.
bool rasterizesRows(const Path& path, int fromRow, int toRow);
// The outline cut to a box, its curves made straight pieces as `fineness` cuts them, each piece
// cut where it crosses a side of the box and what is outside the box pulled onto it: beside the
// box, a piece runs up or down the box's side, which adds to the pixels right of it what the
// piece did; above or below, it runs along the top or bottom, which adds nothing. Every pixel
// inside the box is covered as it was; the outline's points all lie in the box. For a shape too
// large or too far off to be rasterized whole (2026-11-13).
Path clipped(const Path& path, float fineness, float minX, float minY, float maxX, float maxY);

// The same into a box given beforehand, which a colour glyph's layers share: `left` and
// `top` say where the box sits, as in GlyphImage. Adds nothing if the path is empty.
void rasterizeInto(const Path& path, int left, int top, int width, int height, std::vector<uint8_t>& coverage,
                   float fineness = kGlyphFineness, FillRule rule = FillRule::NonZero);

}  // namespace ww::text
