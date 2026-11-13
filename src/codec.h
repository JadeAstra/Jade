// Compression and checks that codecs share (M9/M10 part one, step 3): zlib's deflate both
// ways, CRC-32 and Adler-32. PNG uses them, and WinterWolf has them as Bytes.inflate, Bytes.deflate
// and the bit readers of src/bits_api.cpp.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace ww::codec {

// CRC-32 as PNG, ZIP and gzip have it, carried on from `crc` (0 to start).
uint32_t crc32(const uint8_t* data, size_t size, uint32_t crc = 0);
// Adler-32 as zlib has it, carried on from `adler` (1 to start).
uint32_t adler32(const uint8_t* data, size_t size, uint32_t adler = 1);

// Deflate-compressed data expanded into `out`: with zlib's header and check (RFC 1950), or
// `raw` (RFC 1951 alone, as ZIP and gzip hold it). Never more than `limit` bytes. False, with
// `error`, for data that is damaged, ends early or would be larger than the limit. `used`, if
// given, is how many bytes of `data` the compressed data took.
bool inflate(const uint8_t* data, size_t size, std::vector<uint8_t>& out, size_t limit, std::string& error, bool raw = false,
             size_t* used = nullptr);

// `data` compressed, at `level` 0 (stored, as fast as copying) to 9 (smallest, slowest), as
// zlib's levels are; 6 is zlib's own default. With zlib's header and check unless `raw`.
std::vector<uint8_t> deflate(const uint8_t* data, size_t size, int level = 6, bool raw = false);

}  // namespace ww::codec
