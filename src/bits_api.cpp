// What BitReader, HuffmanTable and Bytes.inflate and deflate (src/std/bits.jade) are built on:
// bits read from any place in bytes, either order; a Huffman code decoded at a place; and
// codec.cpp's deflate both ways.

#include <cstring>
#include <string>
#include <vector>

#include "builtins.h"
#include "codec.h"
#include "vm.h"

namespace jade {

namespace {

// `n` bits (0 to 32) from bit `position` of the bytes: the first bit in a byte its lowest
// (lsb first, as deflate, GIF and WebP's lossless form have them) or its highest (msb first,
// as JPEG has them), the first bit read the value's lowest or highest the same way. Bits past
// the end are zeros.
uint64_t bitsAt(const std::vector<uint8_t>& data, int64_t position, int n, bool msbFirst) {
    if (n == 0) return 0;
    const size_t size = data.size();
    size_t byte = static_cast<size_t>(position >> 3);
    const int shift = static_cast<int>(position & 7);
    // Eight bytes from `byte`, zeros past the end.
    uint64_t window = 0;
    if (byte + 8 <= size) {
        std::memcpy(&window, data.data() + byte, 8);
    } else {
        for (int k = 0; k < 8 && byte + static_cast<size_t>(k) < size; k++) window |= static_cast<uint64_t>(data[byte + static_cast<size_t>(k)]) << (8 * k);
    }
    if (!msbFirst) return (window >> shift) & ((uint64_t{1} << n) - 1);
    // Most significant first: the bytes in the other order, as one big-endian number.
    uint64_t big = 0;
    for (int k = 0; k < 8; k++) big = (big << 8) | ((window >> (8 * k)) & 0xFF);
    return (big << shift) >> (64 - n);
}

// A Huffman code: its symbols in the order of their codes, by length, and a table of the next
// 10 bits each way for the codes no longer than that.
struct HuffmanCode {
    static constexpr int kFast = 10;
    static constexpr int kLongest = 16;
    uint16_t counts[kLongest + 1] = {};
    uint32_t firstCode[kLongest + 1] = {};
    uint16_t firstIndex[kLongest + 1] = {};
    std::vector<int32_t> symbols;          // in the order of their codes
    std::vector<uint32_t> fastMsb, fastLsb;  // (symbol << 5) | length, 0 where none
};

void releaseHuffman(int64_t id) { delete reinterpret_cast<HuffmanCode*>(id); }

const HuffmanCode& huffmanOf(VM& vm, const Value& v) {
    if (!v.isObjType(ObjType::Resource) || std::strcmp(static_cast<ObjResource*>(v.as.obj)->kind, "huffman") != 0)
        throw vm.runtimeError(Thrown::IllegalArgumentException, "not a Huffman table");
    return *reinterpret_cast<const HuffmanCode*>(static_cast<ObjResource*>(v.as.obj)->id);
}

uint32_t reversed(uint32_t code, int length) {
    uint32_t r = 0;
    for (int i = 0; i < length; i++) r |= ((code >> i) & 1) << (length - 1 - i);
    return r;
}

const std::vector<uint8_t>& bytesOf(VM& vm, const Value& v) {
    if (!v.isObjType(ObjType::Bytes)) throw vm.runtimeError(Thrown::IllegalArgumentException, "bits are read from Bytes, not a value of type " + vm.typeName(v));
    return asBytes(v)->data;
}

Value bytesValue(VM& vm, std::vector<uint8_t>&& data) {
    vm.checkRoomFor(data.size());
    ObjBytes* made = vm.alloc<ObjBytes>();
    made->data = std::move(data);
    vm.account(made->data.size());
    return Value::object(made);
}

}  // namespace

void registerBits(VM& vm) {
    ObjClass* holder = vm.alloc<ObjClass>(vm.intern("Bits"));
    auto def = [&](const char* name, int minArgs, int maxArgs, NativeFn fn) {
        holder->statics.set(vm.intern(name), Value::object(vm.alloc<ObjNative>(std::string("Bits.") + name, minArgs, maxArgs, fn)));
    };

    // n bits at a bit position: peek(bytes, position, n, msbFirst).
    def("peek", 4, 4, [](VM& vm, int, Value* args) -> Value {
        const std::vector<uint8_t>& data = bytesOf(vm, args[0]);
        const int64_t position = vm.toInt(args[1], "position");
        const int64_t n = vm.toInt(args[2], "count");
        if (n < 0 || n > 32) throw vm.runtimeError(Thrown::IllegalArgumentException, "bits are read 0 to 32 at a time, not " + std::to_string(n));
        if (position < 0) throw vm.runtimeError(Thrown::IndexOutOfBoundsException, "bit position " + std::to_string(position) + " is before the start");
        return Value::integer(static_cast<int64_t>(bitsAt(data, position, static_cast<int>(n), args[3].truthy())));
    });

    // A Huffman table from each symbol's code length, the codes as deflate numbers them; or, with
    // `symbols`, from the lengths in the order the codes go, each code's symbol given, as JPEG
    // has them. A string, what is wrong, where the lengths make no code.
    def("huffman", 2, 2, [](VM& vm, int, Value* args) -> Value {
        if (!args[0].isObjType(ObjType::List)) throw vm.runtimeError(Thrown::IllegalArgumentException, "a Huffman table is made from a list of lengths");
        ObjList* lengthList = asList(args[0]);
        ObjList* symbolList = args[1].isObjType(ObjType::List) ? asList(args[1]) : nullptr;
        const size_t n = lengthList->size();
        if (symbolList != nullptr && symbolList->size() != n)
            return vm.str("there are " + std::to_string(n) + " lengths but " + std::to_string(symbolList->size()) + " symbols");
        auto code = std::make_unique<HuffmanCode>();
        std::vector<int> lengths(n);
        std::vector<int32_t> symbolOf(n);
        for (size_t i = 0; i < n; i++) {
            const Value l = vm.listGet(lengthList, i);
            if (!l.isInt() || l.as.i < 0 || l.as.i > HuffmanCode::kLongest) return vm.str("a code length is 0 to 16, not " + vm.stringify(l));
            lengths[i] = static_cast<int>(l.as.i);
            if (symbolList != nullptr) {
                const Value s = vm.listGet(symbolList, i);
                if (!s.isInt() || s.as.i < 0 || s.as.i >= (int64_t{1} << 26)) return vm.str("a symbol is an int from 0 to 67108863, not " + vm.stringify(s));
                symbolOf[i] = static_cast<int32_t>(s.as.i);
            } else {
                symbolOf[i] = static_cast<int32_t>(i);
            }
            code->counts[lengths[i]]++;
        }
        code->counts[0] = 0;
        int64_t left = 1;
        for (int length = 1; length <= HuffmanCode::kLongest; length++) {
            left = left * 2 - code->counts[length];
            if (left < 0) return vm.str("more codes than the lengths have room for");
        }
        // Symbols by length, then in the order given (by symbol, for lengths alone).
        uint16_t next[HuffmanCode::kLongest + 1] = {};
        uint16_t index = 0;
        uint32_t first = 0;
        for (int length = 1; length <= HuffmanCode::kLongest; length++) {
            first = (first + code->counts[length - 1]) << 1;
            code->firstCode[length] = first;
            code->firstIndex[length] = index;
            next[length] = index;
            index = static_cast<uint16_t>(index + code->counts[length]);
        }
        code->symbols.assign(index, 0);
        code->fastMsb.assign(size_t{1} << HuffmanCode::kFast, 0);
        code->fastLsb.assign(size_t{1} << HuffmanCode::kFast, 0);
        for (size_t i = 0; i < n; i++) {
            const int length = lengths[i];
            if (length == 0) continue;
            const uint16_t at = next[length]++;
            code->symbols[at] = symbolOf[i];
            if (length > HuffmanCode::kFast) continue;
            const uint32_t c = code->firstCode[length] + (at - code->firstIndex[length]);
            const uint32_t entry = (static_cast<uint32_t>(symbolOf[i]) << 5) | static_cast<uint32_t>(length);
            const int spare = HuffmanCode::kFast - length;
            for (uint32_t k = 0; k < (1u << spare); k++) {
                code->fastMsb[(c << spare) | k] = entry;
                code->fastLsb[reversed(c, length) | (k << length)] = entry;
            }
        }
        HuffmanCode* kept = code.release();
        return Value::object(vm.alloc<ObjResource>("huffman", reinterpret_cast<int64_t>(kept), &releaseHuffman));
    });

    // The next code at a bit position: (symbol << 5) | its length, or -1 where the bits are none.
    def("decode", 4, 4, [](VM& vm, int, Value* args) -> Value {
        const HuffmanCode& code = huffmanOf(vm, args[0]);
        const std::vector<uint8_t>& data = bytesOf(vm, args[1]);
        const int64_t position = vm.toInt(args[2], "position");
        if (position < 0) throw vm.runtimeError(Thrown::IndexOutOfBoundsException, "bit position " + std::to_string(position) + " is before the start");
        const bool msbFirst = args[3].truthy();
        const uint32_t bits = static_cast<uint32_t>(bitsAt(data, position, HuffmanCode::kLongest, msbFirst));
        // The first 10 bits, in the order the code's bits go.
        const uint32_t head = msbFirst ? bits >> (HuffmanCode::kLongest - HuffmanCode::kFast) : bits & ((1u << HuffmanCode::kFast) - 1);
        const uint32_t entry = msbFirst ? code.fastMsb[head] : code.fastLsb[head];
        if (entry != 0) return Value::integer(static_cast<int64_t>(entry));
        uint32_t c = msbFirst ? head : reversed(head, HuffmanCode::kFast);
        for (int length = HuffmanCode::kFast + 1; length <= HuffmanCode::kLongest; length++) {
            const uint32_t bit = msbFirst ? (bits >> (HuffmanCode::kLongest - length)) & 1 : (bits >> (length - 1)) & 1;
            c = (c << 1) | bit;
            const uint32_t offset = c - code.firstCode[length];
            if (offset < code.counts[length])
                return Value::integer((static_cast<int64_t>(code.symbols[code.firstIndex[length] + offset]) << 5) | length);
        }
        return Value::integer(-1);
    });

    // Deflate both ways: inflate(bytes, raw, limit) gives Bytes, or a string saying what is wrong.
    def("inflate", 3, 3, [](VM& vm, int, Value* args) -> Value {
        const std::vector<uint8_t>& data = bytesOf(vm, args[0]);
        int64_t limit = vm.toInt(args[2], "limit");  // -1: none
        if (limit < -1) throw vm.runtimeError(Thrown::IllegalArgumentException, "the limit of inflate() is 0 or more, not " + std::to_string(limit));
        if (limit == -1) limit = INT64_MAX;
        std::vector<uint8_t> out;
        std::string error;
        if (!codec::inflate(data.data(), data.size(), out, static_cast<size_t>(limit), error, args[1].truthy())) return vm.str(error);
        return bytesValue(vm, std::move(out));
    });
    def("deflate", 3, 3, [](VM& vm, int, Value* args) -> Value {
        const std::vector<uint8_t>& data = bytesOf(vm, args[0]);
        const int64_t level = vm.toInt(args[1], "level");
        if (level < 0 || level > 9) throw vm.runtimeError(Thrown::IllegalArgumentException, "deflate() takes a level from 0 to 9, not " + std::to_string(level));
        return bytesValue(vm, codec::deflate(data.data(), data.size(), static_cast<int>(level), args[2].truthy()));
    });
    def("crc32", 2, 2, [](VM& vm, int, Value* args) -> Value {
        const std::vector<uint8_t>& data = bytesOf(vm, args[0]);
        return Value::integer(codec::crc32(data.data(), data.size(), static_cast<uint32_t>(vm.toInt(args[1], "crc"))));
    });
    def("adler32", 2, 2, [](VM& vm, int, Value* args) -> Value {
        const std::vector<uint8_t>& data = bytesOf(vm, args[0]);
        return Value::integer(codec::adler32(data.data(), data.size(), static_cast<uint32_t>(vm.toInt(args[1], "adler"))));
    });

    vm.defineGlobal("__bits", Value::object(holder));
}

}  // namespace jade
