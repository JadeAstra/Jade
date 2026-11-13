// Compression and checks that codecs share: see codec.h.

#include "codec.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cstring>

namespace ww::codec {

uint32_t crc32(const uint8_t* data, size_t size, uint32_t crc) {
    // Eight tables, so that eight bytes are taken at a time ("slicing by 8").
    static const auto tables = [] {
        std::array<std::array<uint32_t, 256>, 8> t{};
        for (uint32_t n = 0; n < 256; n++) {
            uint32_t c = n;
            for (int k = 0; k < 8; k++) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            t[0][n] = c;
        }
        for (uint32_t n = 0; n < 256; n++)
            for (int k = 1; k < 8; k++) t[k][n] = (t[k - 1][n] >> 8) ^ t[0][t[k - 1][n] & 0xFF];
        return t;
    }();
    crc = ~crc;
    while (size >= 8) {
        uint32_t lo, hi;
        std::memcpy(&lo, data, 4);
        std::memcpy(&hi, data + 4, 4);
        lo ^= crc;
        crc = tables[7][lo & 0xFF] ^ tables[6][(lo >> 8) & 0xFF] ^ tables[5][(lo >> 16) & 0xFF] ^ tables[4][lo >> 24] ^ tables[3][hi & 0xFF] ^
              tables[2][(hi >> 8) & 0xFF] ^ tables[1][(hi >> 16) & 0xFF] ^ tables[0][hi >> 24];
        data += 8;
        size -= 8;
    }
    while (size-- > 0) crc = tables[0][(crc ^ *data++) & 0xFF] ^ (crc >> 8);
    return ~crc;
}

uint32_t adler32(const uint8_t* data, size_t size, uint32_t adler) {
    uint32_t a = adler & 0xFFFF, b = adler >> 16;
    while (size > 0) {
        const size_t run = std::min<size_t>(size, 5552);  // the most before b can overflow
        for (size_t i = 0; i < run; i++) {
            a += data[i];
            b += a;
        }
        a %= 65521;
        b %= 65521;
        data += run;
        size -= run;
    }
    return (b << 16) | a;
}

namespace {

// The lengths and distances of the format: each a code, then some extra bits.
constexpr uint16_t kLengthBase[29] = {3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
constexpr uint8_t kLengthExtra[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
constexpr uint16_t kDistBase[30] = {1,   2,   3,   4,   5,   7,    9,    13,   17,   25,   33,   49,   65,    97,    129,
                                    193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
constexpr uint8_t kDistExtra[30] = {0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};
// The order the code length code's lengths are written in.
constexpr uint8_t kLengthOrder[19] = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};

uint32_t reverseBits(uint32_t code, int length) {
    uint32_t reversed = 0;
    for (int i = 0; i < length; i++) reversed |= ((code >> i) & 1) << (length - 1 - i);
    return reversed;
}

// ---- Expanding ----

// The compressed bits, the first in a byte its lowest, read through 64 bits kept at a time.
struct Bits {
    const uint8_t* p;
    const uint8_t* end;
    uint64_t buffer = 0;
    int count = 0;      // bits in the buffer
    size_t beyond = 0;  // bytes of zeros put in past the end, which no compressed data may need

    void refill() {
        while (count <= 56) {
            if (p < end) buffer |= static_cast<uint64_t>(*p++) << count;
            else beyond++;
            count += 8;
        }
    }
    uint32_t peek(int n) {
        if (count < n) refill();
        return static_cast<uint32_t>(buffer & ((uint64_t{1} << n) - 1));
    }
    void drop(int n) {
        buffer >>= n;
        count -= n;
    }
    uint32_t take(int n) {
        if (n == 0) return 0;
        const uint32_t value = peek(n);
        drop(n);
        return value;
    }
    // Whether bits past the end have been taken.
    bool overrun() const { return static_cast<int64_t>(beyond) * 8 > count; }
    void toByte() { drop(count & 7); }
    // Where the next whole byte is in the data, the buffer's bytes given back.
    const uint8_t* bytePlace() const { return p - (static_cast<size_t>(count / 8) - beyond); }
};

// A Huffman code by its lengths, decoded through a table of the next 10 bits: a code no longer
// is found at once; a longer one from there, a bit at a time.
struct Decoder {
    static constexpr int kFast = 10;
    uint16_t fast[1 << kFast];  // (length << 9) | symbol; 0 where the code is longer
    uint16_t counts[16];
    uint16_t firstCode[16];
    uint16_t firstIndex[16];
    uint16_t sorted[288];

    bool build(const uint8_t* lengths, int n) {
        std::memset(counts, 0, sizeof counts);
        std::memset(fast, 0, sizeof fast);
        for (int i = 0; i < n; i++) counts[lengths[i]]++;
        counts[0] = 0;
        int left = 1;
        for (int length = 1; length < 16; length++) {
            left = left * 2 - counts[length];
            if (left < 0) return false;  // more codes than the lengths allow
        }
        // Where each length's symbols start in `sorted`, and its first code, as RFC 1951
        // numbers them: the first of each length after the last of the length before, doubled.
        uint16_t index = 0;
        uint16_t next[16];
        for (int length = 1; length < 16; length++) {
            firstIndex[length] = index;
            next[length] = index;
            index = static_cast<uint16_t>(index + counts[length]);
        }
        uint16_t code = 0;
        uint16_t nextCode[16];
        for (int length = 1; length < 16; length++) {
            code = static_cast<uint16_t>((code + counts[length - 1]) << 1);
            nextCode[length] = code;
            firstCode[length] = code;
        }
        for (int symbol = 0; symbol < n; symbol++) {
            const int length = lengths[symbol];
            if (length == 0) continue;
            sorted[next[length]++] = static_cast<uint16_t>(symbol);
            const uint32_t c = nextCode[length]++;
            if (length <= kFast) {
                const uint32_t reversed = reverseBits(c, length);
                for (uint32_t k = reversed; k < (1u << kFast); k += 1u << length) fast[k] = static_cast<uint16_t>((length << 9) | symbol);
            }
        }
        return true;
    }

    // The next symbol, or -1 for bits that are no code.
    int decode(Bits& b) const {
        const uint32_t bits = b.peek(16);
        const uint16_t entry = fast[bits & ((1u << kFast) - 1)];
        if (entry != 0) {
            b.drop(entry >> 9);
            return entry & 511;
        }
        uint32_t code = reverseBits(bits & ((1u << kFast) - 1), kFast);
        for (int length = kFast + 1; length < 16; length++) {
            code = (code << 1) | ((bits >> (length - 1)) & 1);
            const uint32_t offset = code - firstCode[length];
            if (offset < counts[length]) {
                b.drop(length);
                return sorted[firstIndex[length] + offset];
            }
        }
        return -1;
    }
};

}  // namespace

bool inflate(const uint8_t* data, size_t size, std::vector<uint8_t>& out, size_t limit, std::string& error, bool raw, size_t* used) {
    auto fail = [&](const char* why) {
        error = why;
        return false;
    };
    const uint8_t* start = data;
    if (!raw) {
        if (size < 2 || (data[0] & 0x0F) != 8 || (data[0] >> 4) > 7 || ((static_cast<unsigned>(data[0]) << 8) | data[1]) % 31 != 0)
            return fail("not zlib data: its header is wrong");
        if ((data[1] & 0x20) != 0) return fail("zlib data that needs a preset dictionary, which is not supported");
        data += 2;
        size -= 2;
    }
    const size_t begin = out.size();
    out.reserve(begin + std::min<size_t>(limit, size * 4 + 64));
    Bits b{data, data + size};

    static const auto fixed = [] {
        std::array<Decoder, 2> d{};
        uint8_t lengths[288];
        for (int i = 0; i < 288; i++) lengths[i] = static_cast<uint8_t>(i < 144 ? 8 : i < 256 ? 9 : i < 280 ? 7 : 8);
        d[0].build(lengths, 288);
        uint8_t distances[30];
        std::memset(distances, 5, sizeof distances);
        d[1].build(distances, 30);
        return d;
    }();
    Decoder own[2];

    for (bool last = false; !last;) {
        last = b.take(1) != 0;
        const uint32_t kind = b.take(2);
        if (b.overrun()) return fail("the compressed data ends early");
        if (kind == 0) {  // stored as it is
            b.toByte();
            const uint32_t length = b.take(16), check = b.take(16);
            if (b.overrun()) return fail("the compressed data ends early");
            if ((length ^ check) != 0xFFFF) return fail("damaged compressed data: a stored block's length and its check disagree");
            if (out.size() - begin + length > limit) return fail("the compressed data expands past the limit");
            uint32_t left = length;
            while (left > 0 && b.count >= 8 && !(b.beyond > 0 && b.count <= static_cast<int>(b.beyond) * 8)) {
                out.push_back(static_cast<uint8_t>(b.take(8)));
                left--;
            }
            if (left > 0) {
                if (b.count != 0 || static_cast<size_t>(b.end - b.p) < left) return fail("the compressed data ends early");
                out.insert(out.end(), b.p, b.p + left);
                b.p += left;
            }
            continue;
        }
        if (kind == 3) return fail("damaged compressed data: a block of a kind there is none of");

        const Decoder* literals = &fixed[0];
        const Decoder* distances = &fixed[1];
        if (kind == 2) {
            // The block brings its own codes, themselves written with a third, small one.
            const int literalCount = static_cast<int>(b.take(5)) + 257, distanceCount = static_cast<int>(b.take(5)) + 1;
            const int lengthCount = static_cast<int>(b.take(4)) + 4;
            if (literalCount > 286 || distanceCount > 30) return fail("damaged compressed data: too many codes");
            uint8_t lengths[320] = {};
            for (int i = 0; i < lengthCount; i++) lengths[kLengthOrder[i]] = static_cast<uint8_t>(b.take(3));
            Decoder lengthCode;
            if (!lengthCode.build(lengths, 19)) return fail("damaged compressed data: a code of impossible lengths");
            std::memset(lengths, 0, sizeof lengths);
            for (int i = 0; i < literalCount + distanceCount;) {
                const int symbol = lengthCode.decode(b);
                if (symbol < 0 || b.overrun()) return fail("damaged compressed data: bits that are no code");
                if (symbol < 16) {
                    lengths[i++] = static_cast<uint8_t>(symbol);
                    continue;
                }
                uint8_t repeated = 0;
                int times;
                if (symbol == 16) {
                    if (i == 0) return fail("damaged compressed data: a length repeated before any");
                    repeated = lengths[i - 1];
                    times = 3 + static_cast<int>(b.take(2));
                } else if (symbol == 17) {
                    times = 3 + static_cast<int>(b.take(3));
                } else {
                    times = 11 + static_cast<int>(b.take(7));
                }
                if (i + times > literalCount + distanceCount) return fail("damaged compressed data: lengths past the codes");
                while (times-- > 0) lengths[i++] = repeated;
            }
            if (lengths[256] == 0) return fail("damaged compressed data: no code for the end of a block");
            if (!own[0].build(lengths, literalCount) || !own[1].build(lengths + literalCount, distanceCount))
                return fail("damaged compressed data: a code of impossible lengths");
            literals = &own[0];
            distances = &own[1];
        }

        for (;;) {
            const int symbol = literals->decode(b);
            if (symbol < 0 || b.overrun()) return fail(b.overrun() ? "the compressed data ends early" : "damaged compressed data: bits that are no code");
            if (symbol < 256) {
                if (out.size() - begin >= limit) return fail("the compressed data expands past the limit");
                out.push_back(static_cast<uint8_t>(symbol));
                continue;
            }
            if (symbol == 256) break;
            if (symbol > 285) return fail("damaged compressed data: a length there is none of");
            const size_t length = kLengthBase[symbol - 257] + b.take(kLengthExtra[symbol - 257]);
            const int code = distances->decode(b);
            if (code < 0 || code > 29) return fail("damaged compressed data: a distance there is none of");
            const size_t distance = kDistBase[code] + b.take(kDistExtra[code]);
            if (b.overrun()) return fail("the compressed data ends early");
            if (distance > out.size() - begin) return fail("damaged compressed data: a distance back past the start");
            if (out.size() - begin + length > limit) return fail("the compressed data expands past the limit");
            const size_t from = out.size() - distance;
            const size_t at = out.size();
            out.resize(at + length);
            uint8_t* o = out.data();
            if (distance >= length) {
                std::memcpy(o + at, o + from, length);
            } else {
                for (size_t k = 0; k < length; k++) o[at + k] = o[from + k];
            }
        }
    }

    b.toByte();
    const uint8_t* after = b.bytePlace();
    if (!raw) {
        if (b.end - after < 4) return fail("the compressed data ends before its check");
        const uint32_t check = (static_cast<uint32_t>(after[0]) << 24) | (static_cast<uint32_t>(after[1]) << 16) | (static_cast<uint32_t>(after[2]) << 8) | after[3];
        if (check != adler32(out.data() + begin, out.size() - begin)) return fail("damaged compressed data: its check does not match what it expands to");
        after += 4;
    }
    if (used != nullptr) *used = static_cast<size_t>(after - start);
    return true;
}

// ---- Compressing ----

namespace {

struct BitsOut {
    std::vector<uint8_t>& out;
    uint64_t buffer = 0;
    int count = 0;

    void put(uint32_t value, int length) {
        buffer |= static_cast<uint64_t>(value) << count;
        count += length;
        while (count >= 8) {
            out.push_back(static_cast<uint8_t>(buffer));
            buffer >>= 8;
            count -= 8;
        }
    }
    void toByte() {
        if (count > 0) out.push_back(static_cast<uint8_t>(buffer));
        buffer = 0;
        count = 0;
    }
};

// Lengths of a Huffman code for these counts, none longer than `most`: the code Huffman's
// way, then, where it is too deep, the deepest shortened and others lengthened to make room
// (as miniz does), the rarest symbols given the longest.
void codeLengths(const uint32_t* counts, int n, int most, uint8_t* lengths) {
    std::memset(lengths, 0, static_cast<size_t>(n));
    std::vector<std::pair<uint32_t, int>> used;
    for (int i = 0; i < n; i++)
        if (counts[i] != 0) used.push_back({counts[i], i});
    if (used.empty()) return;
    if (used.size() == 1) {
        lengths[used[0].second] = 1;
        return;
    }
    std::sort(used.begin(), used.end());
    const size_t m = used.size();
    // Two queues: the leaves, rarest first, and the nodes made, which come out in order.
    std::vector<uint64_t> weight(2 * m);
    std::vector<int32_t> parent(2 * m, -1);
    for (size_t i = 0; i < m; i++) weight[i] = used[i].first;
    size_t leaf = 0, node = m, made = m;
    auto next = [&]() -> size_t {
        if (leaf < m && (node >= made || weight[leaf] <= weight[node])) return leaf++;
        return node++;
    };
    while (made < 2 * m - 1) {
        const size_t a = next(), b = next();
        weight[made] = weight[a] + weight[b];
        parent[a] = static_cast<int32_t>(made);
        parent[b] = static_cast<int32_t>(made);
        made++;
    }
    std::vector<int> depth(2 * m - 1, 0);
    for (size_t i = 2 * m - 2; i-- > 0;) depth[i] = depth[static_cast<size_t>(parent[i])] + 1;
    std::vector<int> perLength(64, 0);
    for (size_t i = 0; i < m; i++) perLength[static_cast<size_t>(std::min(depth[i], 63))]++;
    // Too deep: everything below `most` counted at `most`, then room made.
    for (int length = most + 1; length < 64; length++) {
        perLength[static_cast<size_t>(most)] += perLength[static_cast<size_t>(length)];
        perLength[static_cast<size_t>(length)] = 0;
    }
    uint64_t total = 0;
    for (int length = 1; length <= most; length++) total += static_cast<uint64_t>(perLength[static_cast<size_t>(length)]) << (most - length);
    while (total > (uint64_t{1} << most)) {
        perLength[static_cast<size_t>(most)]--;
        for (int length = most - 1; length > 0; length--) {
            if (perLength[static_cast<size_t>(length)] != 0) {
                perLength[static_cast<size_t>(length)]--;
                perLength[static_cast<size_t>(length + 1)] += 2;
                break;
            }
        }
        total--;
    }
    // The rarest (first in `used`) given the longest.
    size_t i = 0;
    for (int length = most; length > 0; length--)
        for (int k = 0; k < perLength[static_cast<size_t>(length)]; k++) lengths[used[i++].second] = static_cast<uint8_t>(length);
}

// The codes for lengths, as RFC 1951 numbers them, each reversed, as they go out first bit first.
void codesOf(const uint8_t* lengths, int n, uint16_t* codes) {
    uint16_t perLength[16] = {}, next[16] = {};
    for (int i = 0; i < n; i++) perLength[lengths[i]]++;
    perLength[0] = 0;
    uint16_t code = 0;
    for (int length = 1; length < 16; length++) {
        code = static_cast<uint16_t>((code + perLength[length - 1]) << 1);
        next[length] = code;
    }
    for (int i = 0; i < n; i++)
        codes[i] = lengths[i] == 0 ? 0 : static_cast<uint16_t>(reverseBits(next[lengths[i]]++, lengths[i]));
}

int lengthCode(int length) {
    int code = 28;
    while (kLengthBase[code] > length) code--;
    return code;
}

int distanceCode(int distance) {
    if (distance <= 4) return distance - 1;
    int code = 29;
    while (kDistBase[code] > distance) code--;
    return code;
}

// A repeat or a byte, as the block keeps them until it is written.
struct Symbol {
    uint16_t value;     // the byte, or the repeat's length
    uint16_t distance;  // 0 for a byte
};

// How hard each level looks for repeats, as zlib's: a repeat this long is good enough to look
// less after, this long not looked past for a longer one starting a byte on, this long taken at
// once, and how many earlier places with the same three bytes are tried.
struct Effort {
    int good, lazy, nice, chain;
};
constexpr Effort kEffort[10] = {{0, 0, 0, 0},       {4, 0, 8, 4},       {4, 0, 16, 8},      {4, 0, 32, 32},     {4, 4, 16, 16},
                                {8, 16, 32, 32},    {8, 16, 128, 128},  {8, 32, 128, 256},  {32, 128, 258, 1024}, {32, 258, 258, 4096}};

class Compressor {
public:
    Compressor(const uint8_t* data, size_t size, int level, std::vector<uint8_t>& out)
        : data_(data), size_(size), effort_(kEffort[level]), bits_{out} {}

    void run() {
        if (effort_.chain == 0) {  // level 0: stored blocks
            size_t at = 0;
            do {
                const size_t length = std::min<size_t>(65535, size_ - at);
                writeStored(at, length, at + length == size_);
                at += length;
            } while (at < size_);
            return;
        }
        head_.assign(size_t{1} << kHashBits, -1);
        previous_.assign(kWindow, -1);
        size_t blockStart = 0;
        size_t i = 0;
        bool haveNext = false;
        Match next{};
        while (i < size_) {
            Match m = haveNext ? next : find(i);
            haveNext = false;
            insert(i);
            if (m.length >= 3 && effort_.lazy > 0 && m.length < effort_.lazy && i + 1 < size_) {
                next = find(i + 1, m.length);
                haveNext = true;
                if (next.length > m.length) {
                    literal(i);
                    i++;
                    flushIfFull(blockStart, i);
                    continue;
                }
                haveNext = false;
            }
            if (m.length >= 3) {
                symbols_.push_back(Symbol{static_cast<uint16_t>(m.length), static_cast<uint16_t>(m.distance)});
                literalCounts_[257 + lengthCode(m.length)]++;
                distanceCounts_[distanceCode(m.distance)]++;
                for (int k = 1; k < m.length; k++) insert(i + static_cast<size_t>(k));
                i += static_cast<size_t>(m.length);
            } else {
                literal(i);
                i++;
            }
            flushIfFull(blockStart, i);
        }
        writeBlock(blockStart, size_, true);
        bits_.toByte();  // the last bits, in a byte of their own
    }

private:
    static constexpr int kHashBits = 16;
    static constexpr size_t kWindow = 32768;
    static constexpr size_t kBlockSymbols = 32768;

    struct Match {
        int length = 0, distance = 0;
    };

    const uint8_t* data_;
    size_t size_;
    Effort effort_;
    BitsOut bits_;
    std::vector<int32_t> head_, previous_;
    std::vector<Symbol> symbols_;
    uint32_t literalCounts_[286] = {};
    uint32_t distanceCounts_[30] = {};

    uint32_t hashAt(size_t i) const {
        uint32_t v;
        std::memcpy(&v, data_ + i, 4);
        return (v * 2654435761u) >> (32 - kHashBits);
    }
    void insert(size_t i) {
        if (i + 3 >= size_) return;
        const uint32_t h = hashAt(i);
        previous_[i & (kWindow - 1)] = head_[h];
        head_[h] = static_cast<int32_t>(i);
    }
    // How many bytes at `a` and `b` are the same, up to `limit`: eight compared at a time.
    int sameLength(const uint8_t* a, const uint8_t* b, int limit) const {
        int length = 0;
        while (length + 8 <= limit) {
            uint64_t x, y;
            std::memcpy(&x, a + length, 8);
            std::memcpy(&y, b + length, 8);
            if (x != y) return length + (std::countr_zero(x ^ y) >> 3);
            length += 8;
        }
        while (length < limit && a[length] == b[length]) length++;
        return length;
    }
    // The longest earlier repeat of the bytes at `i`; after a repeat of `previous` bytes that is
    // already good, a quarter of the search, as zlib does.
    Match find(size_t i, int previous = 0) const {
        Match best;
        if (i + 3 >= size_) return best;
        const int limit = static_cast<int>(std::min<size_t>(258, size_ - i));
        int chain = effort_.chain;
        if (previous >= effort_.good) chain >>= 2;
        int bestLength = std::max(2, previous);
        if (bestLength >= limit) return best;
        const uint8_t* here = data_ + i;
        for (int32_t at = head_[hashAt(i)]; at >= 0 && chain-- > 0; at = previous_[static_cast<size_t>(at) & (kWindow - 1)]) {
            const size_t from = static_cast<size_t>(at);
            if (from >= i || i - from > kWindow - 262) break;  // out of the window, or a place written over
            const uint8_t* there = data_ + from;
            // The two bytes that would end a longer repeat, and the first, before the rest.
            if (there[bestLength] != here[bestLength] || there[bestLength - 1] != here[bestLength - 1] || there[0] != here[0]) continue;
            const int length = sameLength(there, here, limit);
            if (length > bestLength) {
                bestLength = length;
                best.length = length;
                best.distance = static_cast<int>(i - from);
                if (length >= effort_.nice || length >= limit) break;
                if (length >= effort_.good) chain >>= 2;
            }
        }
        if (best.length == 3 && best.distance > 4096) best.length = 0;  // too far to pay, as zlib finds
        return best;
    }
    void literal(size_t i) {
        symbols_.push_back(Symbol{data_[i], 0});
        literalCounts_[data_[i]]++;
    }
    void flushIfFull(size_t& blockStart, size_t i) {
        if (symbols_.size() < kBlockSymbols) return;
        writeBlock(blockStart, i, false);
        blockStart = i;
    }

    void writeStored(size_t at, size_t length, bool last) {
        bits_.put(last ? 1 : 0, 1);
        bits_.put(0, 2);
        bits_.toByte();
        bits_.put(static_cast<uint32_t>(length), 16);
        bits_.put(static_cast<uint32_t>(length ^ 0xFFFF), 16);
        bits_.out.insert(bits_.out.end(), data_ + at, data_ + at + length);
    }

    // The block of symbols, written the way that takes fewest bits: with codes of its own, with
    // the format's ready-made ones, or stored.
    void writeBlock(size_t from, size_t to, bool last) {
        literalCounts_[256]++;
        uint8_t literalLengths[286], distanceLengths[30];
        codeLengths(literalCounts_, 286, 15, literalLengths);
        codeLengths(distanceCounts_, 30, 15, distanceLengths);
        int literalCount = 286, distanceCount = 30;
        while (literalCount > 257 && literalLengths[literalCount - 1] == 0) literalCount--;
        while (distanceCount > 1 && distanceLengths[distanceCount - 1] == 0) distanceCount--;
        if (distanceLengths[0] == 0 && distanceCount == 1) distanceLengths[0] = 1;  // one distance code at least

        // The lengths, run-length coded with 16, 17 and 18.
        uint8_t all[316];
        std::memcpy(all, literalLengths, static_cast<size_t>(literalCount));
        std::memcpy(all + literalCount, distanceLengths, static_cast<size_t>(distanceCount));
        const int total = literalCount + distanceCount;
        std::vector<std::pair<uint8_t, uint8_t>> runs;  // a symbol and its extra bits
        uint32_t lengthCounts[19] = {};
        for (int i = 0; i < total;) {
            const uint8_t value = all[i];
            int run = 1;
            while (i + run < total && all[i + run] == value) run++;
            i += run;
            if (value == 0) {
                while (run >= 11) {
                    const int take = std::min(run, 138);
                    runs.push_back({18, static_cast<uint8_t>(take - 11)});
                    lengthCounts[18]++;
                    run -= take;
                }
                if (run >= 3) {
                    runs.push_back({17, static_cast<uint8_t>(run - 3)});
                    lengthCounts[17]++;
                    run = 0;
                }
            } else {
                runs.push_back({value, 0});
                lengthCounts[value]++;
                run--;
                while (run >= 3) {
                    const int take = std::min(run, 6);
                    runs.push_back({16, static_cast<uint8_t>(take - 3)});
                    lengthCounts[16]++;
                    run -= take;
                }
            }
            while (run-- > 0) {
                runs.push_back({value, 0});
                lengthCounts[value]++;
            }
        }
        uint8_t lengthLengths[19];
        codeLengths(lengthCounts, 19, 7, lengthLengths);
        int lengthCount = 19;
        while (lengthCount > 4 && lengthLengths[kLengthOrder[lengthCount - 1]] == 0) lengthCount--;

        // What each way costs, in bits.
        static const auto fixedLengths = [] {
            std::array<uint8_t, 318> l{};
            for (int i = 0; i < 288; i++) l[static_cast<size_t>(i)] = static_cast<uint8_t>(i < 144 ? 8 : i < 256 ? 9 : i < 280 ? 7 : 8);
            for (int i = 288; i < 318; i++) l[static_cast<size_t>(i)] = 5;
            return l;
        }();
        uint64_t own = 3 + 14 + 3 * static_cast<uint64_t>(lengthCount), fixed = 3;
        for (const auto& [symbol, extra] : runs) own += lengthLengths[symbol] + (symbol == 16 ? 2 : symbol == 17 ? 3 : symbol == 18 ? 7 : 0);
        for (int s = 0; s < 286; s++) {
            const uint64_t extra = s >= 257 ? kLengthExtra[s - 257] : 0;
            own += literalCounts_[s] * (literalLengths[s] + extra);
            fixed += literalCounts_[s] * (fixedLengths[static_cast<size_t>(s)] + extra);
        }
        for (int d = 0; d < 30; d++) {
            own += distanceCounts_[d] * (static_cast<uint64_t>(distanceLengths[d]) + kDistExtra[d]);
            fixed += distanceCounts_[d] * (5 + static_cast<uint64_t>(kDistExtra[d]));
        }
        const uint64_t stored = (to - from) * 8 + 40 * ((to - from) / 65535 + 1);

        if (stored < own && stored < fixed) {
            for (size_t at = from; at < to || at == from;) {
                const size_t length = std::min<size_t>(65535, to - at);
                writeStored(at, length, last && at + length == to);
                at += length;
                if (length == 0) break;
            }
        } else if (fixed <= own) {
            static const auto fixedCodes = [] {
                std::array<uint16_t, 288> c{};
                codesOf(fixedLengths.data(), 288, c.data());
                return c;
            }();
            static const auto fixedDistanceCodes = [] {
                std::array<uint16_t, 30> c{};
                codesOf(fixedLengths.data() + 288, 30, c.data());
                return c;
            }();
            bits_.put(last ? 1 : 0, 1);
            bits_.put(1, 2);
            writeSymbols(fixedLengths.data(), fixedCodes.data(), fixedLengths.data() + 288, fixedDistanceCodes.data());
        } else {
            uint16_t literalCodes[286], distanceCodes[30], lengthCodes[19];
            codesOf(literalLengths, 286, literalCodes);
            codesOf(distanceLengths, 30, distanceCodes);
            codesOf(lengthLengths, 19, lengthCodes);
            bits_.put(last ? 1 : 0, 1);
            bits_.put(2, 2);
            bits_.put(static_cast<uint32_t>(literalCount - 257), 5);
            bits_.put(static_cast<uint32_t>(distanceCount - 1), 5);
            bits_.put(static_cast<uint32_t>(lengthCount - 4), 4);
            for (int i = 0; i < lengthCount; i++) bits_.put(lengthLengths[kLengthOrder[i]], 3);
            for (const auto& [symbol, extra] : runs) {
                bits_.put(lengthCodes[symbol], lengthLengths[symbol]);
                if (symbol == 16) bits_.put(extra, 2);
                else if (symbol == 17) bits_.put(extra, 3);
                else if (symbol == 18) bits_.put(extra, 7);
            }
            writeSymbols(literalLengths, literalCodes, distanceLengths, distanceCodes);
        }
        symbols_.clear();
        std::memset(literalCounts_, 0, sizeof literalCounts_);
        std::memset(distanceCounts_, 0, sizeof distanceCounts_);
    }

    void writeSymbols(const uint8_t* literalLengths, const uint16_t* literalCodes, const uint8_t* distanceLengths, const uint16_t* distanceCodes) {
        for (const Symbol& s : symbols_) {
            if (s.distance == 0) {
                bits_.put(literalCodes[s.value], literalLengths[s.value]);
                continue;
            }
            const int lc = lengthCode(s.value);
            bits_.put(literalCodes[257 + lc], literalLengths[257 + lc]);
            bits_.put(static_cast<uint32_t>(s.value - kLengthBase[lc]), kLengthExtra[lc]);
            const int dc = distanceCode(s.distance);
            bits_.put(distanceCodes[dc], distanceLengths[dc]);
            bits_.put(static_cast<uint32_t>(s.distance - kDistBase[dc]), kDistExtra[dc]);
        }
        bits_.put(literalCodes[256], literalLengths[256]);
    }
};

}  // namespace

std::vector<uint8_t> deflate(const uint8_t* data, size_t size, int level, bool raw) {
    level = std::clamp(level, 0, 9);
    std::vector<uint8_t> out;
    out.reserve(size / 2 + 64);
    if (!raw) {
        out.push_back(0x78);
        const uint8_t levelBits = level <= 1 ? 0 : level <= 5 ? 1 : level == 6 ? 2 : 3;
        uint8_t flags = static_cast<uint8_t>(levelBits << 6);
        flags = static_cast<uint8_t>(flags + (31 - ((0x78u << 8) + flags) % 31) % 31);
        out.push_back(flags);
    }
    Compressor(data, size, level, out).run();
    if (!raw) {
        const uint32_t check = adler32(data, size);
        for (int shift = 24; shift >= 0; shift -= 8) out.push_back(static_cast<uint8_t>(check >> shift));
    }
    return out;
}

}  // namespace ww::codec
