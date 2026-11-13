// PNG pictures, read and written by code of our own.
//
// A picture here is `width` by `height` pixels of four bytes each: red, green, blue and
// alpha, top row first, the colours not multiplied by the alpha. That is what a PNG file
// holds, so nothing is lost going either way.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace ww::image {

// Writes a picture as the bytes of a PNG file, compressed.
std::vector<uint8_t> encodePng(const uint8_t* rgba, int width, int height);

// Reads the bytes of a PNG file: every kind the format has (grey, palette, colour, with or
// without alpha, 1 to 16 bits, interlaced or not). False, with `error`, if it is not one or
// is damaged.
bool decodePng(const uint8_t* data, size_t size, std::vector<uint8_t>& rgba, int& width, int& height, std::string& error);

// The compression and checks are codec.h's.

}  // namespace ww::image
