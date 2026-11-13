// PNG pictures: see png.h.

#include "png.h"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <cstring>

#include "../codec.h"
#include "../section_window.h"  // last: this file's own code goes in its section (D161)

namespace ww::image {

// ---- PNG ----

namespace {

const uint8_t signature[8] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};

void putU32(std::vector<uint8_t>& out, uint32_t v) {
    for (int shift = 24; shift >= 0; shift -= 8) out.push_back(static_cast<uint8_t>(v >> shift));
}

void chunk(std::vector<uint8_t>& out, const char* type, const std::vector<uint8_t>& body) {
    putU32(out, static_cast<uint32_t>(body.size()));
    const size_t start = out.size();
    out.insert(out.end(), type, type + 4);
    out.insert(out.end(), body.begin(), body.end());
    putU32(out, codec::crc32(out.data() + start, out.size() - start));
}

uint32_t getU32(const uint8_t* p) { return (static_cast<uint32_t>(p[0]) << 24) | (static_cast<uint32_t>(p[1]) << 16) | (static_cast<uint32_t>(p[2]) << 8) | p[3]; }

int paeth(int a, int b, int c) {
    const int p = a + b - c, pa = std::abs(p - a), pb = std::abs(p - b), pc = std::abs(p - c);
    return pa <= pb && pa <= pc ? a : pb <= pc ? b : c;
}

}  // namespace

std::vector<uint8_t> encodePng(const uint8_t* rgba, int width, int height) {
    std::vector<uint8_t> out(signature, signature + 8);
    std::vector<uint8_t> header;
    putU32(header, static_cast<uint32_t>(width));
    putU32(header, static_cast<uint32_t>(height));
    // A picture with nothing see-through is written without its alpha.
    const size_t pixels = static_cast<size_t>(width) * static_cast<size_t>(height);
    bool opaque = true;
    for (size_t i = 0; i < pixels && opaque; i++) opaque = rgba[i * 4 + 3] == 255;
    const size_t bpp = opaque ? 3 : 4;
    header.insert(header.end(), {8, static_cast<uint8_t>(opaque ? 2 : 6), 0, 0, 0});
    chunk(out, "IHDR", header);

    // Each row is written as its difference from something near it, whichever of the
    // format's five ways leaves the smallest numbers: that is what makes it compress.
    // The rows are kept with `bpp` zeros before them, so that the byte to the left is always there;
    // each filter is its own plain loop over the row, which the compiler makes into vector code.
    const size_t rowBytes = static_cast<size_t>(width) * bpp;
    std::vector<uint8_t> raw((rowBytes + 1) * static_cast<size_t>(height));
    std::vector<uint8_t> rowA(rowBytes + bpp, 0), rowB(rowBytes + bpp, 0);
    std::vector<uint8_t> tried[5];
    for (auto& t : tried) t.resize(rowBytes);
    uint8_t* above = rowA.data();
    uint8_t* current = rowB.data();
    auto score = [&](const uint8_t* t) {
        uint32_t sum = 0;
        for (size_t i = 0; i < rowBytes; i++) sum += static_cast<uint32_t>(std::abs(static_cast<int>(static_cast<int8_t>(t[i]))));
        return sum;
    };
    for (int y = 0; y < height; y++) {
        const uint8_t* row = rgba + static_cast<size_t>(y) * static_cast<size_t>(width) * 4;
        uint8_t* cur = current + bpp;
        if (bpp == 4) {
            std::memcpy(cur, row, rowBytes);
        } else {
            for (size_t x = 0; x < static_cast<size_t>(width); x++) {
                cur[x * 3] = row[x * 4];
                cur[x * 3 + 1] = row[x * 4 + 1];
                cur[x * 3 + 2] = row[x * 4 + 2];
            }
        }
        const uint8_t* up = above + bpp;
        const uint8_t* left = current;  // cur[i - bpp]
        const uint8_t* upLeft = above;  // up[i - bpp]
        uint8_t* t1 = tried[1].data();
        uint8_t* t2 = tried[2].data();
        uint8_t* t3 = tried[3].data();
        uint8_t* t4 = tried[4].data();
        std::memcpy(tried[0].data(), cur, rowBytes);
        for (size_t i = 0; i < rowBytes; i++) t1[i] = static_cast<uint8_t>(cur[i] - left[i]);
        for (size_t i = 0; i < rowBytes; i++) t2[i] = static_cast<uint8_t>(cur[i] - up[i]);
        for (size_t i = 0; i < rowBytes; i++) t3[i] = static_cast<uint8_t>(cur[i] - ((left[i] + up[i]) >> 1));
        for (size_t i = 0; i < rowBytes; i++) {
            const int a = left[i], b = up[i], c = upLeft[i];
            const int pa = std::abs(b - c), pb = std::abs(a - c), pc = std::abs(a + b - 2 * c);
            const int predicted = pa <= pb && pa <= pc ? a : pb <= pc ? b : c;
            t4[i] = static_cast<uint8_t>(cur[i] - predicted);
        }
        int bestFilter = 0;
        uint32_t bestSum = score(tried[0].data());
        for (int filter = 1; filter < 5; filter++) {
            const uint32_t sum = score(tried[filter].data());
            if (sum < bestSum) {
                bestSum = sum;
                bestFilter = filter;
            }
        }
        uint8_t* out = raw.data() + static_cast<size_t>(y) * (rowBytes + 1);
        out[0] = static_cast<uint8_t>(bestFilter);
        std::memcpy(out + 1, tried[bestFilter].data(), rowBytes);
        std::swap(above, current);
    }
    chunk(out, "IDAT", codec::deflate(raw.data(), raw.size(), 6));
    chunk(out, "IEND", {});
    return out;
}

bool decodePng(const uint8_t* data, size_t size, std::vector<uint8_t>& rgba, int& width, int& height, std::string& error) {
    auto fail = [&](const char* why) {
        error = why;
        return false;
    };
    if (size < 8 || std::memcmp(data, signature, 8) != 0) return fail("not a PNG picture");

    uint32_t w = 0, h = 0;
    int depth = 0, colorType = -1, interlace = 0;
    std::vector<uint8_t> palette, paletteAlpha, compressed;
    int clearGrey = -1, clearRed = -1, clearGreen = -1, clearBlue = -1;  // the one colour that is see-through, if any
    for (size_t at = 8; at + 12 <= size;) {
        const uint32_t length = getU32(data + at);
        if (length > size - at - 12) return fail("damaged PNG picture");
        const uint8_t* body = data + at + 8;
        const std::string type(reinterpret_cast<const char*>(data + at + 4), 4);
        if (type == "IHDR" && length >= 13) {
            w = getU32(body);
            h = getU32(body + 4);
            depth = body[8];
            colorType = body[9];
            interlace = body[12];
            if (body[10] != 0 || body[11] != 0 || interlace > 1) return fail("a PNG picture of a kind that is not read");
        } else if (type == "PLTE") {
            palette.assign(body, body + length);
        } else if (type == "tRNS") {
            if (colorType == 3) paletteAlpha.assign(body, body + length);
            else if (colorType == 0 && length >= 2) clearGrey = (body[0] << 8) | body[1];
            else if (colorType == 2 && length >= 6) {
                clearRed = (body[0] << 8) | body[1];
                clearGreen = (body[2] << 8) | body[3];
                clearBlue = (body[4] << 8) | body[5];
            }
        } else if (type == "IDAT") {
            compressed.insert(compressed.end(), body, body + length);
        } else if (type == "IEND") {
            break;
        }
        at += static_cast<size_t>(length) + 12;
    }

    int channels = 0;
    switch (colorType) {
        case 0: channels = 1; break;
        case 2: channels = 3; break;
        case 3: channels = 1; break;
        case 4: channels = 2; break;
        case 6: channels = 4; break;
        default: return fail("damaged PNG picture");
    }
    const bool depthFits = depth == 8 || (depth == 16 && colorType != 3) || ((depth == 1 || depth == 2 || depth == 4) && (colorType == 0 || colorType == 3));
    if (w == 0 || h == 0 || w > 32768 || h > 32768 || !depthFits) return fail("damaged PNG picture");
    if (static_cast<uint64_t>(w) * h > (uint64_t{1} << 28)) return fail("the PNG picture is too large");
    if (colorType == 3 && palette.size() < 3) return fail("damaged PNG picture");

    const size_t bitsPerPixel = static_cast<size_t>(channels) * static_cast<size_t>(depth);
    const size_t bpp = std::max<size_t>(1, bitsPerPixel / 8);  // what "the pixel to the left" means for the filters
    auto rowBytesOf = [&](uint32_t pixels) { return (static_cast<size_t>(pixels) * bitsPerPixel + 7) / 8; };

    // An interlaced picture comes as seven smaller pictures, coarse to fine.
    static const int startX[7] = {0, 4, 0, 2, 0, 1, 0}, startY[7] = {0, 0, 4, 0, 2, 0, 1}, stepX[7] = {8, 8, 4, 4, 2, 2, 1}, stepY[7] = {8, 8, 8, 4, 4, 2, 2};
    struct Pass {
        uint32_t w, h;
        int x0, y0, dx, dy;
    };
    std::vector<Pass> passes;
    size_t expected = 0;
    if (interlace == 0) {
        passes.push_back({w, h, 0, 0, 1, 1});
    } else {
        for (int p = 0; p < 7; p++) {
            const uint32_t pw = (w + static_cast<uint32_t>(stepX[p] - startX[p] - 1)) / static_cast<uint32_t>(stepX[p]);
            const uint32_t ph = (h + static_cast<uint32_t>(stepY[p] - startY[p] - 1)) / static_cast<uint32_t>(stepY[p]);
            if (pw == 0 || ph == 0) continue;
            passes.push_back({pw, ph, startX[p], startY[p], stepX[p], stepY[p]});
        }
    }
    for (const Pass& pass : passes) expected += (rowBytesOf(pass.w) + 1) * pass.h;

    std::vector<uint8_t> raw;
    raw.reserve(expected);
    if (!codec::inflate(compressed.data(), compressed.size(), raw, expected, error)) return false;
    if (raw.size() < expected) return fail("the PNG picture ends early");

    width = static_cast<int>(w);
    height = static_cast<int>(h);
    rgba.assign(static_cast<size_t>(w) * h * 4, 0);

    size_t at = 0;
    std::vector<uint8_t> above;
    for (const Pass& pass : passes) {
        const size_t rowBytes = rowBytesOf(pass.w);
        above.assign(rowBytes, 0);
        for (uint32_t row = 0; row < pass.h; row++) {
            const int filter = raw[at++];
            uint8_t* line = &raw[at];
            at += rowBytes;
            for (size_t i = 0; i < rowBytes; i++) {
                const int left = i >= bpp ? line[i - bpp] : 0, up = above[i], upLeft = i >= bpp ? above[i - bpp] : 0;
                int predicted = 0;
                switch (filter) {
                    case 0: break;
                    case 1: predicted = left; break;
                    case 2: predicted = up; break;
                    case 3: predicted = (left + up) / 2; break;
                    case 4: predicted = paeth(left, up, upLeft); break;
                    default: return fail("damaged PNG picture");
                }
                line[i] = static_cast<uint8_t>(line[i] + predicted);
            }
            above.assign(line, line + rowBytes);

            // One value of a pixel, as it is stored: `depth` bits.
            auto sample = [&](size_t index) -> int {
                if (depth == 8) return line[index];
                if (depth == 16) return (line[index * 2] << 8) | line[index * 2 + 1];
                const size_t bit = index * static_cast<size_t>(depth);
                return (line[bit / 8] >> (8 - depth - static_cast<int>(bit % 8))) & ((1 << depth) - 1);
            };
            // The same as a byte: 0 to 255 whatever the depth.
            auto byteOf = [&](int value) -> uint8_t {
                if (depth == 8) return static_cast<uint8_t>(value);
                if (depth == 16) return static_cast<uint8_t>((static_cast<uint32_t>(value) * 255 + 32767) / 65535);
                return static_cast<uint8_t>(value * 255 / ((1 << depth) - 1));
            };
            const size_t y = static_cast<size_t>(pass.y0) + static_cast<size_t>(row) * static_cast<size_t>(pass.dy);
            for (uint32_t col = 0; col < pass.w; col++) {
                const size_t x = static_cast<size_t>(pass.x0) + static_cast<size_t>(col) * static_cast<size_t>(pass.dx);
                uint8_t* out = &rgba[(y * w + x) * 4];
                const size_t first = static_cast<size_t>(col) * static_cast<size_t>(channels);
                switch (colorType) {
                    case 0: {
                        const int grey = sample(first);
                        out[0] = out[1] = out[2] = byteOf(grey);
                        out[3] = grey == clearGrey ? 0 : 255;
                        break;
                    }
                    case 2: {
                        const int red = sample(first), green = sample(first + 1), blue = sample(first + 2);
                        out[0] = byteOf(red);
                        out[1] = byteOf(green);
                        out[2] = byteOf(blue);
                        out[3] = red == clearRed && green == clearGreen && blue == clearBlue ? 0 : 255;
                        break;
                    }
                    case 3: {
                        const size_t entry = static_cast<size_t>(sample(first));
                        if (entry * 3 + 2 < palette.size()) {
                            out[0] = palette[entry * 3];
                            out[1] = palette[entry * 3 + 1];
                            out[2] = palette[entry * 3 + 2];
                        }
                        out[3] = entry < paletteAlpha.size() ? paletteAlpha[entry] : 255;
                        break;
                    }
                    case 4:
                        out[0] = out[1] = out[2] = byteOf(sample(first));
                        out[3] = byteOf(sample(first + 1));
                        break;
                    default:
                        out[0] = byteOf(sample(first));
                        out[1] = byteOf(sample(first + 1));
                        out[2] = byteOf(sample(first + 2));
                        out[3] = byteOf(sample(first + 3));
                        break;
                }
            }
        }
    }
    return true;
}

}  // namespace ww::image
