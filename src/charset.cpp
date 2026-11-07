// Character sets (D127): text turned into bytes and back in UTF-8, Latin-1, ASCII and UTF-16.
// Jade's text is UTF-8 throughout; the other sets are for what is read from and written to
// files, processes and sockets that use them, and for the conversions between text and bytes.

#include "charset.h"

#include <cstring>

#include "vm.h"

namespace jade {

namespace {

// UTF-16 units, a byte at a time in the order the set says.
void putUnit16(std::string& out, uint32_t unit, bool bigEndian) {
    const char low = static_cast<char>(unit & 0xFF), high = static_cast<char>(unit >> 8);
    if (bigEndian) {
        out += high;
        out += low;
    } else {
        out += low;
        out += high;
    }
}

uint32_t unit16(std::string_view bytes, size_t at, bool bigEndian) {
    const auto b0 = static_cast<unsigned char>(bytes[at]), b1 = static_cast<unsigned char>(bytes[at + 1]);
    return bigEndian ? (b0 << 8) | b1 : (b1 << 8) | b0;
}

// The characters 0x80 to 0x9F are in Windows-1252 (old mail's, and Windows' "ANSI"), 0 where it
// has none; the rest of it is Latin-1 (G53).
const uint16_t kWindows1252High[32] = {
    0x20AC, 0,      0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021, 0x02C6, 0x2030, 0x0160, 0x2039, 0x0152, 0,      0x017D, 0,
    0,      0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014, 0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0,      0x017E, 0x0178};

// ISO-8859-15, Latin-9: Latin-1 with eight places given other characters, the euro among them.
struct Latin9Place {
    uint8_t first;    // the byte
    uint16_t second;  // the character it is
};
const Latin9Place kLatin9[8] = {{0xA4, 0x20AC}, {0xA6, 0x0160}, {0xA8, 0x0161}, {0xB4, 0x017D},
                                {0xB8, 0x017E}, {0xBC, 0x0152}, {0xBD, 0x0153}, {0xBE, 0x0178}};

// The character a byte is in a set of one byte a character; 0xFFFD for one it has none for.
uint32_t singleByte(uint8_t b, Charset cs) {
    switch (cs) {
        case Charset::Ascii: return b < 0x80 ? b : 0xFFFD;
        case Charset::Windows1252:
            if (b >= 0x80 && b < 0xA0) return kWindows1252High[b - 0x80] != 0 ? kWindows1252High[b - 0x80] : 0xFFFD;
            return b;
        case Charset::Latin9:
            for (const auto& [place, c] : kLatin9)
                if (place == b) return c;
            return b;
        default: return b;  // Latin-1
    }
}

// The byte a character is in such a set; '?' for one it has no place for, as in Java.
char singleByteOf(uint32_t c, Charset cs) {
    if (c < 0x80) return static_cast<char>(c);
    switch (cs) {
        case Charset::Ascii: return '?';
        case Charset::Windows1252:
            for (int i = 0; i < 32; i++)
                if (kWindows1252High[i] == c) return static_cast<char>(0x80 + i);
            return c >= 0xA0 && c < 0x100 ? static_cast<char>(c) : '?';
        case Charset::Latin9:
            for (const auto& [place, ch] : kLatin9) {
                if (ch == c) return static_cast<char>(place);
                if (place == c) return '?';  // a Latin-1 character Latin-9 gave its place away
            }
            return c < 0x100 ? static_cast<char>(c) : '?';
        default: return c < 0x100 ? static_cast<char>(c) : '?';  // Latin-1
    }
}

}  // namespace

std::string encodeText(std::string_view utf8, Charset cs) {
    if (cs == Charset::Utf8) return std::string(utf8);
    const Characters text = Characters::fromUtf8(std::string(utf8));
    std::string out;
    out.reserve(text.size() * (isUtf16(cs) ? 2 : 1) + 2);
    if (cs == Charset::Utf16) putUnit16(out, 0xFEFF, true);  // a byte order mark, then big-endian
    for (size_t i = 0; i < text.size(); i++) {
        const uint32_t c = text[i];
        if (!isUtf16(cs)) {
            out += singleByteOf(c, cs);
            continue;
        }
        const bool big = bigEndian(cs);
        if (c < 0x10000) {
            putUnit16(out, c, big);
        } else {
            putUnit16(out, 0xD800 + ((c - 0x10000) >> 10), big);
            putUnit16(out, 0xDC00 + ((c - 0x10000) & 0x3FF), big);
        }
    }
    return out;
}

Charset utf16Order(std::string_view bytes, Charset cs, size_t& markSize) {
    markSize = 0;
    if (bytes.size() < 2) return cs;
    const auto b0 = static_cast<unsigned char>(bytes[0]), b1 = static_cast<unsigned char>(bytes[1]);
    if (b0 == 0xFE && b1 == 0xFF) {
        markSize = 2;
        return Charset::Utf16Be;
    }
    if (b0 == 0xFF && b1 == 0xFE) {
        markSize = 2;
        return Charset::Utf16Le;
    }
    return cs;
}

std::string decodeText(std::string_view bytes, Charset cs) {
    if (cs == Charset::Utf8) return std::string(bytes);
    std::string out;
    out.reserve(bytes.size());
    if (!isUtf16(cs)) {
        for (const char b : bytes) appendUtf8(out, singleByte(static_cast<uint8_t>(b), cs));
        return out;
    }
    size_t at = 0;
    if (cs == Charset::Utf16) cs = utf16Order(bytes, Charset::Utf16Be, at);  // the mark, if any, says the order
    const bool big = bigEndian(cs);
    while (at + 1 < bytes.size()) {
        const uint32_t unit = unit16(bytes, at, big);
        at += 2;
        if (unit >= 0xD800 && unit <= 0xDBFF && at + 1 < bytes.size()) {
            const uint32_t next = unit16(bytes, at, big);
            if (next >= 0xDC00 && next <= 0xDFFF) {
                appendUtf8(out, 0x10000 + ((unit - 0xD800) << 10) + (next - 0xDC00));
                at += 2;
                continue;
            }
        }
        // A surrogate on its own is no character.
        appendUtf8(out, unit >= 0xD800 && unit <= 0xDFFF ? 0xFFFD : unit);
    }
    if (at < bytes.size()) appendUtf8(out, 0xFFFD);  // half a unit at the end
    return out;
}

size_t lineEnd(std::string_view bytes, size_t from, Charset cs, size_t& endSize) {
    if (!isUtf16(cs)) {
        endSize = 1;
        return bytes.find('\n', from);
    }
    // UTF-16: a whole unit that is a line end, the units counted from `from`.
    endSize = 2;
    const bool big = bigEndian(cs);
    for (size_t at = from; at + 1 < bytes.size(); at += 2)
        if (unit16(bytes, at, big) == '\n') return at;
    return std::string_view::npos;
}

std::string withoutCarriageReturn(std::string line, Charset cs) {
    if (isUtf16(cs)) {
        if (line.size() >= 2 && unit16(line, line.size() - 2, bigEndian(cs)) == '\r') line.resize(line.size() - 2);
    } else if (!line.empty() && line.back() == '\r') {
        line.pop_back();
    }
    return line;
}

Charset charsetOf(VM& vm, Value v) {
    if (v.isNull()) return Charset::Utf8;
    if (v.isObjType(ObjType::Instance)) {
        const ObjInstance* entry = asInstance(v);
        if (entry->cls->isEnum && entry->cls->name->utf8() == "Charset")
            if (const Value* ordinal = entry->field(vm.names.entryOrdinal); ordinal != nullptr && ordinal->isInt() && ordinal->as.i >= 0 && ordinal->as.i <= 7)
                return static_cast<Charset>(ordinal->as.i);
    }
    throw vm.runtimeError(Thrown::ClassCastException, "expected a Charset (Charset.utf8, latin1, ascii, utf16le, utf16be, utf16, windows1252 or latin9), got a value of type " + vm.typeName(v));
}

}  // namespace jade
