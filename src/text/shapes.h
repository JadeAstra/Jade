// Outlines of the shapes a canvas draws, in pixels with y growing downwards, for both canvases
// that draw them (text/api.cpp's on an image in memory, topaz/api.cpp's on the graphics card):
// arcs, wedges and chords of an ellipse, and lines and outlines of any width (M8).
//
// A shape is made of pieces that may overlap (the bands of a wide line, the discs at its
// joins); each is turned the same way round, so that the rasteriser, which adds up how much of
// each pixel the pieces cover and stops at full, covers each pixel once.
#pragma once

#include <vector>

#include "face.h"

namespace jade::text {

// How an arc is closed, as Java's Arc2D: OPEN (the curve alone), CHORD (a straight line from its
// end back to its start), PIE (lines from both ends to the middle of the ellipse).
enum ArcKind { kArcOpen = 0, kArcChord = 1, kArcPie = 2 };

// How a line `widthPx` wide lies around the one-unit line (`unitPx` wide) that a coordinate
// names: the extra width, in whole pixels, before it (left or up) and after it (right or down);
// the larger half after, as Java's pen hangs to the right and below.
struct Spread {
    double before, after;
};
Spread spreadOf(double widthPx, double unitPx);

// The whole of an arc of the ellipse in the box (l, t)-(r, b): a wedge for PIE, the arc and its
// chord otherwise (an OPEN arc fills as a CHORD, as in Java). Angles in degrees, 0 at three
// o'clock and counter-clockwise; on an ellipse 45 always points at the box's top right corner.
void arcFill(Path& p, double l, double t, double r, double b, double start, double extent, ArcKind kind);

// The outline of that arc, `width` pixels across, centred on the ellipse: the curve, and for a
// CHORD the straight line, for a PIE the two lines to the middle.
void arcStroke(Path& p, double l, double t, double r, double b, double start, double extent, ArcKind kind, double width);

// Lines through the points (x, y pairs), `width` pixels across with square ends and round
// joins, closed back to the first point or not: one shape, each pixel covered once.
void polylineStroke(Path& p, const std::vector<double>& xy, bool closed, double width);

}  // namespace jade::text

namespace jade {
class VM;
struct Value;
namespace text {

// A Path a canvas fills (N3), flattened by the standard library into straight lines (Path.flatten):
// for each contour, how many points it has and then their x and y, in units. Put into `out` in
// pixels, each contour closed, at (originX, originY) plus `scale` pixels a unit; false if there
// is nothing to fill. Throws for a list that is not that.
bool pathOf(VM& vm, Value outline, double originX, double originY, double scale, Path& out);

}  // namespace text
}  // namespace jade
