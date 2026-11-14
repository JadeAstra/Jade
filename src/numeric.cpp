// Whole-list, grid and picture operations (M9/M10 part one, questions 15 and 17): numpy's kind of
// work on the lists, Bytes and grids WinterWolf has, each a loop in C++ over a list's packed block,
// and the general picture operations (a window reduced, a grid sampled, values scattered, a
// colour table, blend modes, distances, regions). The built-in __num; what each is to a program
// is in src/std/numeric.ww.
//
// Every operation gives what WinterWolf's own arithmetic gives: an int with an int stays an int, its
// overflow an ArithmeticException as + gives, unless the ...Wrapped form is asked for; a float
// with anything a float; a result written into a list of a narrow width checked against its
// range as storing it is.

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <numeric>
#include <vector>

#include "builtins.h"
#include "vm.h"

namespace ww {

namespace {

// A number rounded half away from zero, as std::lround, without its call (a library call on
// Windows, which a loop over every pixel pays for). For numbers an int64 holds.
inline int64_t rounded(double v) { return v >= 0 ? static_cast<int64_t>(v + 0.5) : -static_cast<int64_t>(-v + 0.5); }

[[noreturn]] void fail(VM& vm, Thrown kind, const std::string& message) { throw vm.runtimeError(kind, message); }

ObjList* listArg(VM& vm, Value v, const char* what) {
    if (!v.isObjType(ObjType::List)) fail(vm, Thrown::ClassCastException, std::string(what) + " is a list");
    return asList(v);
}

size_t sizeArg(VM& vm, Value v, const char* what) {
    const int64_t n = vm.toInt(v, what);
    if (n < 0) fail(vm, Thrown::IllegalArgumentException, std::string(what) + " is below 0");
    return static_cast<size_t>(n);
}

bool intNarrow(Narrow n) { return n == Narrow::Byte || n == Narrow::Int16 || n == Narrow::Int32 || n == Narrow::Uint16 || n == Narrow::Uint32; }

const char* narrowName(Narrow n) {
    switch (n) {
        case Narrow::Byte: return "byte";
        case Narrow::Int16: return "int16";
        case Narrow::Int32: return "int32";
        case Narrow::Uint16: return "uint16";
        case Narrow::Uint32: return "uint32";
        case Narrow::Float32: return "float32";
        default: return "char";
    }
}

// The range a narrow int keeps.
void numericRange(Narrow n, int64_t& low, int64_t& high) {
    switch (n) {
        case Narrow::Byte: low = 0, high = 255; break;
        case Narrow::Int16: low = -32768, high = 32767; break;
        case Narrow::Uint16: low = 0, high = 65535; break;
        case Narrow::Int32: low = INT32_MIN, high = INT32_MAX; break;
        default: low = 0, high = UINT32_MAX; break;
    }
}

// A list's items, or one number for every item, read as ints or as floats: a packed List<int> or
// List<float> is read where it is; any other list of numbers, Bytes and a single number are read
// into a block of their own first.
struct Numbers {
    bool isFloat = false;
    bool single = false;
    size_t count = 0;
    const int64_t* ints = nullptr;
    const double* floats = nullptr;
    std::vector<int64_t> ownInts;
    std::vector<double> ownFloats;

    int64_t i(size_t k) const { return ints[single ? 0 : k]; }
    double f(size_t k) const { return isFloat ? floats[single ? 0 : k] : static_cast<double>(ints[single ? 0 : k]); }
    void own() {
        ints = ownInts.data();
        floats = ownFloats.data();
    }
};

Numbers numbers(VM& vm, Value v, const char* what) {
    Numbers n;
    if (v.isInt() || v.isByte()) {
        n.single = true;
        n.count = 1;
        n.ownInts = {v.as.i};
        n.own();
        return n;
    }
    if (v.isFloat()) {
        n.single = n.isFloat = true;
        n.count = 1;
        n.ownFloats = {v.as.d};
        n.own();
        return n;
    }
    if (v.isObjType(ObjType::Bytes)) {
        const std::vector<uint8_t>& data = asBytes(v)->data;
        n.count = data.size();
        n.ownInts.assign(data.begin(), data.end());
        n.own();
        return n;
    }
    if (!v.isObjType(ObjType::List)) fail(vm, Thrown::ClassCastException, std::string(what) + " is a list of numbers or a number, not a " + vm.typeName(v));
    const ObjList* list = asList(v);
    n.count = list->size();
    if (!list->packed()) {
        const std::vector<Value>& items = list->values();
        for (const Value& item : items) {
            if (item.isNull()) fail(vm, Thrown::NullPointerException, std::string(what) + " has a null among its items");
            if (item.isFloat()) n.isFloat = true;
            else if (!item.isInt() && !item.isByte()) fail(vm, Thrown::ClassCastException, std::string(what) + " has a " + vm.typeName(item) + " among its numbers");
        }
        if (n.isFloat) {
            n.ownFloats.reserve(items.size());
            for (const Value& item : items) n.ownFloats.push_back(item.isFloat() ? item.as.d : static_cast<double>(item.as.i));
        } else {
            n.ownInts.reserve(items.size());
            for (const Value& item : items) n.ownInts.push_back(item.as.i);
        }
        n.own();
        return n;
    }
    const PackedItems& p = *list->pack;
    if (p.nullable)
        for (size_t k = 0; k < p.count; k++)
            if ((p.nulls[k / 64] >> (k % 64) & 1) != 0) fail(vm, Thrown::NullPointerException, std::string(what) + " has a null among its items");
    switch (p.store) {
        case ListStore::Ints: n.ints = reinterpret_cast<const int64_t*>(p.block.data()); return n;
        case ListStore::Floats:
            n.isFloat = true;
            n.floats = reinterpret_cast<const double*>(p.block.data());
            return n;
        case ListStore::Narrow:
            if (p.narrow == Narrow::Float32) {
                n.isFloat = true;
                n.ownFloats.resize(p.count);
                for (size_t k = 0; k < p.count; k++) n.ownFloats[k] = readNarrow(p.narrow, p.block.data() + k * p.width).as.d;
                n.own();
                return n;
            }
            if (intNarrow(p.narrow)) {
                n.ownInts.resize(p.count);
                for (size_t k = 0; k < p.count; k++) n.ownInts[k] = readNarrow(p.narrow, p.block.data() + k * p.width).as.i;
                n.own();
                return n;
            }
            break;
        default: break;
    }
    fail(vm, Thrown::ClassCastException, std::string(what) + " is a list of numbers, not a " + vm.typeName(v));
}

// A list's type as a program writes it: List<int>.
std::string listText(VM& vm, const ObjList* list) { return list->type != nullptr ? vm.typeInfoText(list->type) : "List"; }

// Whether a list keeps floats (List<float>, List<float32>, or a List<Number> of them).
bool keepsFloats(const ObjList* list) {
    if (!list->packed()) return true;  // a list of any numbers takes a float
    return list->pack->store == ListStore::Floats || (list->pack->store == ListStore::Narrow && list->pack->narrow == Narrow::Float32);
}

// A new list of `count` zeros: a List<float>, a List<int>, or of the type `like` was made with.
ObjList* newNumbers(VM& vm, bool isFloat, size_t count, const ObjList* like = nullptr) {
    ObjList* list = newList(vm);
    TempRoot keep(vm, Value::object(list));
    if (like != nullptr && like->type != nullptr) vm.typeList(list, like->type);
    else vm.typeList(list, vm.typeOf(kTypeList, false, nullptr, {vm.typeOf(isFloat ? kTypeFloat : kTypeInt, false, nullptr, {})}));
    if (list->packed()) {
        PackedItems& p = *list->pack;
        p.block.assign(count * p.width, 0);
        p.count = count;
        if (p.nullable) p.nulls.assign((count + 63) / 64, 0);
        vm.account(count * p.width);
    } else {
        list->values().assign(count, isFloat ? Value::number(0) : Value::integer(0));
        vm.account(count * sizeof(Value));
    }
    return list;
}

std::string notKept(int64_t v, Narrow n) {
    int64_t low, high;
    numericRange(n, low, high);
    const char* name = narrowName(n);
    return std::to_string(v) + " does not fit in " + (name[0] == 'i' || name[0] == 'u' ? "an " : "a ") + name + ", which keeps " + std::to_string(low) + " to " + std::to_string(high);
}

// Ints written into a list: kept as they are, or as floats in a list of floats, each checked
// against a narrow list's range.
void store(VM& vm, ObjList* out, const std::vector<int64_t>& values) {
    if (out->size() != values.size()) fail(vm, Thrown::IllegalArgumentException, "the list written into has " + std::to_string(out->size()) + " items, not " + std::to_string(values.size()));
    if (!out->packed()) {
        for (size_t k = 0; k < values.size(); k++) out->values()[k] = Value::integer(values[k]);
        return;
    }
    PackedItems& p = *out->pack;
    if (p.nullable) std::fill(p.nulls.begin(), p.nulls.end(), 0);
    uint8_t* block = p.block.data();
    switch (p.store) {
        case ListStore::Ints: std::memcpy(block, values.data(), values.size() * 8); return;
        case ListStore::Floats:
            for (size_t k = 0; k < values.size(); k++) {
                const double d = static_cast<double>(values[k]);
                std::memcpy(block + k * 8, &d, 8);
            }
            return;
        case ListStore::Narrow: {
            if (p.narrow == Narrow::Float32) {
                for (size_t k = 0; k < values.size(); k++) {
                    const float f = static_cast<float>(values[k]);
                    std::memcpy(block + k * 4, &f, 4);
                }
                return;
            }
            if (!intNarrow(p.narrow)) break;
            int64_t low, high;
            numericRange(p.narrow, low, high);
            for (size_t k = 0; k < values.size(); k++) {
                const int64_t v = values[k];
                if (v < low || v > high) fail(vm, Thrown::ClassCastException, notKept(v, p.narrow));
                switch (p.width) {
                    case 1: block[k] = static_cast<uint8_t>(v); break;
                    case 2: {
                        const uint16_t w = static_cast<uint16_t>(v);
                        std::memcpy(block + k * 2, &w, 2);
                        break;
                    }
                    default: {
                        const uint32_t w = static_cast<uint32_t>(v);
                        std::memcpy(block + k * 4, &w, 4);
                        break;
                    }
                }
            }
            return;
        }
        default: break;
    }
    fail(vm, Thrown::ClassCastException, "a " + listText(vm, out) + " cannot keep numbers");
}

// Floats written into a list of floats; a list of ints cannot keep them.
void store(VM& vm, ObjList* out, const std::vector<double>& values) {
    if (out->size() != values.size()) fail(vm, Thrown::IllegalArgumentException, "the list written into has " + std::to_string(out->size()) + " items, not " + std::to_string(values.size()));
    if (!out->packed()) {
        for (size_t k = 0; k < values.size(); k++) out->values()[k] = Value::number(values[k]);
        return;
    }
    PackedItems& p = *out->pack;
    if (p.nullable) std::fill(p.nulls.begin(), p.nulls.end(), 0);
    if (p.store == ListStore::Floats) {
        std::memcpy(p.block.data(), values.data(), values.size() * 8);
        return;
    }
    if (p.store == ListStore::Narrow && p.narrow == Narrow::Float32) {
        for (size_t k = 0; k < values.size(); k++) {
            const float f = static_cast<float>(values[k]);
            std::memcpy(p.block.data() + k * 4, &f, 4);
        }
        return;
    }
    fail(vm, Thrown::ClassCastException, "a " + listText(vm, out) + " cannot keep the floats this gives: make it a list of floats");
}

// Floats written into a list of any numbers, rounded and held to the range of one of ints: what
// a grid resampled or reduced is written back as.
void storeRounded(VM& vm, ObjList* out, const std::vector<double>& values) {
    if (keepsFloats(out)) {
        store(vm, out, values);
        return;
    }
    int64_t low = INT64_MIN, high = INT64_MAX;
    if (out->packed() && out->pack->store == ListStore::Narrow) numericRange(out->pack->narrow, low, high);
    std::vector<int64_t> ints(values.size());
    for (size_t k = 0; k < values.size(); k++) {
        const double r = std::round(values[k]);
        ints[k] = !(r > static_cast<double>(low)) ? low : r >= static_cast<double>(high) ? high : static_cast<int64_t>(r);
    }
    store(vm, out, ints);
}

// Where an operation's result goes: `into` when given, else a new list made for it, of floats
// or ints as the receiver keeps, or of the receiver's own type for a narrow one.
ObjList* resultFor(VM& vm, Value receiver, Value into, size_t count, bool& isFloat, bool computesFloat) {
    if (!into.isNull()) {
        ObjList* out = listArg(vm, into, "into");
        isFloat = keepsFloats(out);
        if (out->size() != count) fail(vm, Thrown::IllegalArgumentException, "into has " + std::to_string(out->size()) + " items, not " + std::to_string(count));
        if (computesFloat && !isFloat) fail(vm, Thrown::ClassCastException, "a " + listText(vm, out) + " cannot keep the floats this gives: make it a list of floats");
        return out;
    }
    const ObjList* r = receiver.isObjType(ObjType::List) ? asList(receiver) : nullptr;
    if (r != nullptr && !r->packed()) {
        isFloat = true;
        return newNumbers(vm, true, count, r);
    }
    if (r == nullptr || r->pack->store == ListStore::Ints || r->pack->store == ListStore::Floats ||
        (r->pack->store == ListStore::Narrow && r->pack->narrow == Narrow::Byte)) {
        isFloat = computesFloat;
        return newNumbers(vm, computesFloat, count);
    }
    isFloat = keepsFloats(r);
    if (computesFloat && !isFloat) fail(vm, Thrown::ClassCastException, "a " + listText(vm, r) + " cannot keep the floats this gives");
    return newNumbers(vm, isFloat, count, r);
}

size_t matched(VM& vm, const Numbers& a, const Numbers& b) {
    if (b.single) return a.count;
    if (a.single) return b.count;
    if (a.count != b.count) fail(vm, Thrown::IllegalArgumentException, "the lists' sizes differ: " + std::to_string(a.count) + " and " + std::to_string(b.count));
    return a.count;
}

enum EachOp { kPlus, kMinus, kTimes, kDiv, kPlusWrap, kMinusWrap, kTimesWrap };

// Whether a value is a list kept as values (a List<Number> of ints and floats together), which
// is worked on an item at a time, each as WinterWolf's arithmetic takes it.
bool anyNumbers(Value v) { return v.isObjType(ObjType::List) && !asList(v)->packed(); }

// Item `k` of a list, or the one number standing for every item: an int or a float.
Value numberAt(VM& vm, Value v, size_t k, const char* what) {
    Value item = v.isObjType(ObjType::List) ? vm.listGet(asList(v), k) : v;
    if (item.isByte()) item = Value::integer(item.as.i);
    if (item.isNull()) fail(vm, Thrown::NullPointerException, std::string(what) + " has a null among its items");
    if (!item.isInt() && !item.isFloat()) fail(vm, Thrown::ClassCastException, std::string(what) + " has a " + vm.typeName(item) + " among its numbers");
    return item;
}

// One step of arithmetic on two numbers, as WinterWolf's operators give it.
Value arith(VM& vm, EachOp op, Value a, Value b) {
    if (a.isInt() && b.isInt()) {
        const int64_t x = a.as.i, y = b.as.i;
        int64_t z = 0;
        switch (op) {
            case kPlus: if (addOverflows(x, y, z)) throw vm.overflowError(Op::Add, x, y); break;
            case kMinus: if (subOverflows(x, y, z)) throw vm.overflowError(Op::Sub, x, y); break;
            case kTimes: if (mulOverflows(x, y, z)) throw vm.overflowError(Op::Mul, x, y); break;
            case kDiv:
                if (y == 0) fail(vm, Thrown::ArithmeticException, "division by zero");
                if (y == -1 && x == INT64_MIN) throw vm.overflowError(Op::Div, x, y);
                z = x / y;
                break;
            case kPlusWrap: z = static_cast<int64_t>(static_cast<uint64_t>(x) + static_cast<uint64_t>(y)); break;
            case kMinusWrap: z = static_cast<int64_t>(static_cast<uint64_t>(x) - static_cast<uint64_t>(y)); break;
            default: z = static_cast<int64_t>(static_cast<uint64_t>(x) * static_cast<uint64_t>(y)); break;
        }
        return Value::integer(z);
    }
    const double x = a.toDouble(), y = b.toDouble();
    return Value::number(op == kPlus ? x + y : op == kMinus ? x - y : op == kTimes ? x * y : x / y);
}

size_t sizeOfArg(VM& vm, Value v, const char* what) {
    if (v.isObjType(ObjType::List)) return asList(v)->size();
    if (v.isObjType(ObjType::Bytes)) return asBytes(v)->data.size();
    numberAt(vm, v, 0, what);
    return SIZE_MAX;  // one number, for every item
}

// ---- Grids of colours: four bytes a cell, red, green, blue, alpha ----

uint8_t* colorCells(VM& vm, Value v, size_t count, const char* what) {
    if (!v.isObjType(ObjType::List) || !asList(v)->packed() || asList(v)->pack->store != ListStore::Items || asList(v)->pack->width != 4)
        fail(vm, Thrown::ClassCastException, std::string(what) + " is a grid of colours");
    if (asList(v)->size() != count) fail(vm, Thrown::IllegalArgumentException, std::string(what) + " has " + std::to_string(asList(v)->size()) + " cells, not " + std::to_string(count));
    return asList(v)->pack->block.data();
}

// A grid's cells as planes of floats: one for a grid of numbers; four for colours, red, green
// and blue multiplied by alpha where `premultiply` says.
std::vector<std::vector<double>> planesOf(VM& vm, Value cells, size_t count, bool colour, bool premultiply) {
    std::vector<std::vector<double>> planes;
    if (!colour) {
        Numbers n = numbers(vm, cells, "the grid");
        if (n.count != count) fail(vm, Thrown::IllegalArgumentException, "the grid has " + std::to_string(n.count) + " cells, not " + std::to_string(count));
        planes.emplace_back(count);
        for (size_t k = 0; k < count; k++) planes[0][k] = n.f(k);
        return planes;
    }
    const uint8_t* c = colorCells(vm, cells, count, "the grid");
    planes.assign(4, std::vector<double>(count));
    for (size_t k = 0; k < count; k++) {
        const double a = c[k * 4 + 3];
        const double m = premultiply ? a / 255.0 : 1.0;
        planes[0][k] = c[k * 4] * m;
        planes[1][k] = c[k * 4 + 1] * m;
        planes[2][k] = c[k * 4 + 2] * m;
        planes[3][k] = a;
    }
    return planes;
}

// Planes written back into a grid's cells: colours rounded and held from 0 to 255, divided by
// alpha again where they were multiplied by it; numbers as storeRounded writes them, or as
// floats where `floats` says.
void writePlanes(VM& vm, Value out, const std::vector<std::vector<double>>& planes, size_t count, bool colour, bool premultiplied) {
    if (!colour) {
        storeRounded(vm, listArg(vm, out, "the grid written into"), planes[0]);
        return;
    }
    uint8_t* c = colorCells(vm, out, count, "the grid written into");
    auto byte = [](double v) { return static_cast<uint8_t>(v <= 0 ? 0 : v >= 255 ? 255 : rounded(v)); };
    for (size_t k = 0; k < count; k++) {
        const double a = std::clamp(planes[3][k], 0.0, 255.0);
        const uint8_t alpha = byte(a);
        c[k * 4 + 3] = alpha;
        if (premultiplied) {
            const double m = alpha == 0 ? 0.0 : 255.0 / a;
            c[k * 4] = byte(planes[0][k] * m);
            c[k * 4 + 1] = byte(planes[1][k] * m);
            c[k * 4 + 2] = byte(planes[2][k] * m);
        } else {
            c[k * 4] = byte(planes[0][k]);
            c[k * 4 + 1] = byte(planes[1][k]);
            c[k * 4 + 2] = byte(planes[2][k]);
        }
    }
}

// A row of colours as floats, red, green and blue multiplied by alpha (out of 255), and alpha.
void premultipliedRow(const uint8_t* src, size_t n, float* out) {
    for (size_t x = 0; x < n; x++, src += 4, out += 4) {
        const float m = src[3] / 255.0f;
        out[0] = src[0] * m;
        out[1] = src[1] * m;
        out[2] = src[2] * m;
        out[3] = src[3];
    }
}

// Such a row back as colours: alpha rounded and held from 0 to 255, the rest divided by it.
void colorRow(const float* in, size_t n, uint8_t* dst) {
    auto byte = [](float v) { return static_cast<uint8_t>(v <= 0 ? 0 : v >= 255 ? 255 : static_cast<int>(v + 0.5f)); };
    for (size_t x = 0; x < n; x++, in += 4, dst += 4) {
        const float a = in[3] < 0 ? 0 : in[3] > 255 ? 255 : in[3];
        const uint8_t alpha = byte(a);
        dst[3] = alpha;
        if (alpha == 0) {
            dst[0] = dst[1] = dst[2] = 0;
            continue;
        }
        const float m = 255.0f / a;
        dst[0] = byte(in[0] * m);
        dst[1] = byte(in[1] * m);
        dst[2] = byte(in[2] * m);
    }
}

// Catmull-Rom's weights for the four cells round a place t of the way between the middle two.
void cubicWeights(double t, double w[4]) {
    const double t2 = t * t, t3 = t2 * t;
    w[0] = (-t3 + 2 * t2 - t) / 2;
    w[1] = (3 * t3 - 5 * t2 + 2) / 2;
    w[2] = (-3 * t3 + 4 * t2 + t) / 2;
    w[3] = (t3 - t2) / 2;
}

// How each cell of a line `to` long takes from a line `from` long: area-averaged when it shrinks,
// read between cells by the interpolation when it grows (0 nearest, 1 bilinear, 2 bicubic).
struct Taps {
    std::vector<size_t> start;  // per output cell: where its taps begin in index/weight
    std::vector<size_t> index;
    std::vector<double> weight;
};

Taps tapsFor(size_t from, size_t to, int interpolation) {
    Taps t;
    const double ratio = static_cast<double>(from) / static_cast<double>(to);
    auto clampIndex = [&](int64_t i) { return static_cast<size_t>(std::clamp<int64_t>(i, 0, static_cast<int64_t>(from) - 1)); };
    for (size_t k = 0; k < to; k++) {
        t.start.push_back(t.index.size());
        if (to < from) {
            // The part of the old line the new cell covers, each old cell by its overlap.
            const double lo = k * ratio, hi = (k + 1) * ratio;
            for (size_t i = static_cast<size_t>(std::floor(lo)); i < from && static_cast<double>(i) < hi; i++) {
                const double overlap = std::min(hi, i + 1.0) - std::max(lo, static_cast<double>(i));
                if (overlap > 0) {
                    t.index.push_back(i);
                    t.weight.push_back(overlap / ratio);
                }
            }
            continue;
        }
        const double at = (k + 0.5) * ratio - 0.5;
        if (interpolation == 0) {
            t.index.push_back(clampIndex(static_cast<int64_t>(std::floor((k + 0.5) * ratio))));
            t.weight.push_back(1.0);
        } else if (interpolation == 1) {
            const double f = std::floor(at);
            const double u = at - f;
            t.index.push_back(clampIndex(static_cast<int64_t>(f)));
            t.weight.push_back(1 - u);
            t.index.push_back(clampIndex(static_cast<int64_t>(f) + 1));
            t.weight.push_back(u);
        } else {
            const double f = std::floor(at);
            double w[4];
            cubicWeights(at - f, w);
            for (int j = 0; j < 4; j++) {
                t.index.push_back(clampIndex(static_cast<int64_t>(f) - 1 + j));
                t.weight.push_back(w[j]);
            }
        }
    }
    t.start.push_back(t.index.size());
    return t;
}

// ---- Windows ----

// The least or greatest of each run of `width` items of `in` centred on it (van Herk and
// Gil-Werman: two passes, three comparisons an item whatever the width); outside the line
// counts as nothing.
void runningExtreme(const double* in, size_t n, size_t half, bool greatest, double* out, std::vector<double>& ahead, std::vector<double>& behind) {
    const size_t w = 2 * half + 1;
    const double none = greatest ? -std::numeric_limits<double>::infinity() : std::numeric_limits<double>::infinity();
    const size_t padded = n + 2 * half;
    const size_t blocks = (padded + w - 1) / w;
    const size_t total = blocks * w;
    ahead.assign(total, none);
    behind.assign(total, none);
    auto at = [&](size_t i) { return i >= half && i - half < n ? in[i - half] : none; };
    auto better = [&](double a, double b) { return greatest ? std::max(a, b) : std::min(a, b); };
    for (size_t b = 0; b < blocks; b++) {
        const size_t s = b * w;
        ahead[s] = at(s);
        for (size_t i = 1; i < w; i++) ahead[s + i] = better(ahead[s + i - 1], at(s + i));
        behind[s + w - 1] = at(s + w - 1);
        for (size_t i = w - 1; i-- > 0;) behind[s + i] = better(behind[s + i + 1], at(s + i));
    }
    // The window of padded items k to k + w - 1 is item k's.
    for (size_t k = 0; k < n; k++) out[k] = better(behind[k], ahead[k + w - 1]);
}

// How far a disc of `radius` reaches across at `dy` rows from its middle.
size_t discHalf(size_t radius, int64_t dy) {
    const double r = static_cast<double>(radius) + 0.5;
    const double across = std::sqrt(std::max(0.0, r * r - static_cast<double>(dy * dy)));
    return static_cast<size_t>(std::floor(across));
}

enum Reduce { kMin, kMax, kMedian, kMean, kSum };

// A square window's mean of colours weighed by alpha (a box blur), from their bytes: each row's
// runs of red, green and blue times alpha, and of alpha, summed in ints into a ring of the
// window's rows, and the sums of each column kept as rows come in and go out. As writePlanes
// gives it: alpha the mean rounded, each colour the weighted mean divided by alpha's.
void squareMeanColours(const uint8_t* src, size_t w, size_t h, size_t radius, uint8_t* dst) {
    const size_t span = 2 * radius + 1;
    const size_t ring = std::min(span, h);
    std::vector<int64_t> runs(ring * w * 4), columns(w * 4, 0), prefix((w + 1) * 4);
    auto rowRuns = [&](size_t y, int64_t* into) {
        const uint8_t* row = src + y * w * 4;
        for (size_t c = 0; c < 4; c++) prefix[c] = 0;
        for (size_t x = 0; x < w; x++) {
            const int64_t a = row[x * 4 + 3];
            prefix[(x + 1) * 4] = prefix[x * 4] + row[x * 4] * a;
            prefix[(x + 1) * 4 + 1] = prefix[x * 4 + 1] + row[x * 4 + 1] * a;
            prefix[(x + 1) * 4 + 2] = prefix[x * 4 + 2] + row[x * 4 + 2] * a;
            prefix[(x + 1) * 4 + 3] = prefix[x * 4 + 3] + a;
        }
        for (size_t x = 0; x < w; x++) {
            const size_t lo = x >= radius ? x - radius : 0, hi = std::min(w - 1, x + radius) + 1;
            for (size_t c = 0; c < 4; c++) into[x * 4 + c] = prefix[hi * 4 + c] - prefix[lo * 4 + c];
        }
    };
    auto slot = [&](size_t y) { return runs.data() + (y % ring) * w * 4; };
    for (size_t y = 0; y < std::min(h, radius + 1); y++) {
        rowRuns(y, slot(y));
        for (size_t k = 0; k < w * 4; k++) columns[k] += slot(y)[k];
    }
    auto byte = [](double v) { return static_cast<uint8_t>(v <= 0 ? 0 : v >= 255 ? 255 : rounded(v)); };
    for (size_t y = 0; y < h; y++) {
        const size_t lo = y >= radius ? y - radius : 0, hi = std::min(h - 1, y + radius);
        const size_t tall = hi - lo + 1;
        uint8_t* out = dst + y * w * 4;
        for (size_t x = 0; x < w; x++) {
            const size_t wide = std::min(w - 1, x + radius) - (x >= radius ? x - radius : 0) + 1;
            const double count = static_cast<double>(wide * tall);
            const int64_t* sum = &columns[x * 4];
            const double alpha = std::clamp(static_cast<double>(sum[3]) / count, 0.0, 255.0);
            const uint8_t a = byte(alpha);
            out[x * 4 + 3] = a;
            // The weighted mean, (sum / 255) / count, times 255 / alpha.
            const double m = a == 0 ? 0.0 : 1.0 / (count * alpha);
            out[x * 4] = byte(static_cast<double>(sum[0]) * m);
            out[x * 4 + 1] = byte(static_cast<double>(sum[1]) * m);
            out[x * 4 + 2] = byte(static_cast<double>(sum[2]) * m);
        }
        if (y + 1 < h) {
            // The row above the next window goes; the one below it comes, into the same slot.
            if (y >= radius) {
                const int64_t* gone = slot(y - radius);
                for (size_t k = 0; k < w * 4; k++) columns[k] -= gone[k];
            }
            if (y + radius + 1 < h) {
                int64_t* come = slot(y + radius + 1);
                rowRuns(y + radius + 1, come);
                for (size_t k = 0; k < w * 4; k++) columns[k] += come[k];
            }
        }
    }
}

// One plane reduced over windows: square ones by separable passes (running sums; van Herk for
// the least and greatest), discs a row of the disc at a time; the median by a running histogram
// of 256 bins where `bytes` says the plane holds 0 to 255, else by sorting each window.
std::vector<double> reducePlane(const std::vector<double>& in, size_t w, size_t h, size_t radius, Reduce reduce, bool disc, bool bytes) {
    std::vector<double> out(w * h);
    const int64_t r = static_cast<int64_t>(radius);
    if (reduce == kMedian) {
        if (bytes) {
            // Huang's running histogram along each row: a cell's window takes in a column and
            // lets one go as it moves right.
            std::vector<int> hist(256);
            for (size_t y = 0; y < h; y++) {
                std::fill(hist.begin(), hist.end(), 0);
                int count = 0;
                auto column = [&](int64_t x, int sign) {
                    if (x < 0 || x >= static_cast<int64_t>(w)) return;
                    for (int64_t dy = -r; dy <= r; dy++) {
                        const int64_t yy = static_cast<int64_t>(y) + dy;
                        if (yy < 0 || yy >= static_cast<int64_t>(h)) continue;
                        const int v = static_cast<int>(in[static_cast<size_t>(yy) * w + static_cast<size_t>(x)]);
                        hist[static_cast<size_t>(std::clamp(v, 0, 255))] += sign;
                        count += sign;
                    }
                };
                if (!disc) {
                    for (int64_t x = -r; x <= r; x++) column(x, 1);
                    for (size_t x = 0; x < w; x++) {
                        int seen = 0, v = 0;
                        while (v < 255 && seen + hist[static_cast<size_t>(v)] <= (count - 1) / 2) seen += hist[static_cast<size_t>(v++)];
                        out[y * w + x] = v;
                        column(static_cast<int64_t>(x) - r, -1);
                        column(static_cast<int64_t>(x) + r + 1, 1);
                    }
                    continue;
                }
                // A disc: its cells taken in whole for each cell (the edge of a disc moves
                // unevenly), still by the histogram, which keeps sorting away.
                for (size_t x = 0; x < w; x++) {
                    std::fill(hist.begin(), hist.end(), 0);
                    count = 0;
                    for (int64_t dy = -r; dy <= r; dy++) {
                        const int64_t yy = static_cast<int64_t>(y) + dy;
                        if (yy < 0 || yy >= static_cast<int64_t>(h)) continue;
                        const int64_t half = static_cast<int64_t>(discHalf(radius, dy));
                        for (int64_t xx = std::max<int64_t>(0, static_cast<int64_t>(x) - half); xx <= std::min<int64_t>(static_cast<int64_t>(w) - 1, static_cast<int64_t>(x) + half); xx++) {
                            hist[static_cast<size_t>(std::clamp(static_cast<int>(in[static_cast<size_t>(yy) * w + static_cast<size_t>(xx)]), 0, 255))]++;
                            count++;
                        }
                    }
                    int seen = 0, v = 0;
                    while (v < 255 && seen + hist[static_cast<size_t>(v)] <= (count - 1) / 2) seen += hist[static_cast<size_t>(v++)];
                    out[y * w + x] = v;
                }
            }
            return out;
        }
        std::vector<double> window;
        for (size_t y = 0; y < h; y++)
            for (size_t x = 0; x < w; x++) {
                window.clear();
                for (int64_t dy = -r; dy <= r; dy++) {
                    const int64_t yy = static_cast<int64_t>(y) + dy;
                    if (yy < 0 || yy >= static_cast<int64_t>(h)) continue;
                    const int64_t half = disc ? static_cast<int64_t>(discHalf(radius, dy)) : r;
                    for (int64_t xx = std::max<int64_t>(0, static_cast<int64_t>(x) - half); xx <= std::min<int64_t>(static_cast<int64_t>(w) - 1, static_cast<int64_t>(x) + half); xx++)
                        window.push_back(in[static_cast<size_t>(yy) * w + static_cast<size_t>(xx)]);
                }
                std::nth_element(window.begin(), window.begin() + static_cast<ptrdiff_t>((window.size() - 1) / 2), window.end());
                out[y * w + x] = window[(window.size() - 1) / 2];
            }
        return out;
    }
    if (reduce == kSum || reduce == kMean) {
        // Sums of each row's prefix: a row's run of any width in one subtraction.
        std::vector<double> prefix((w + 1) * h);
        for (size_t y = 0; y < h; y++)
            for (size_t x = 0; x < w; x++) prefix[y * (w + 1) + x + 1] = prefix[y * (w + 1) + x] + in[y * w + x];
        if (!disc) {
            // Square: each row's runs, then the runs of the window's rows summed for each column,
            // the sums kept as rows come in below and go out above: read along rows only.
            std::vector<double> rows(w * h), counts(w), columns(w, 0.0);
            for (size_t y = 0; y < h; y++)
                for (size_t x = 0; x < w; x++) {
                    const size_t lo = static_cast<size_t>(std::max<int64_t>(0, static_cast<int64_t>(x) - r));
                    const size_t hi = static_cast<size_t>(std::min<int64_t>(static_cast<int64_t>(w) - 1, static_cast<int64_t>(x) + r));
                    rows[y * w + x] = prefix[y * (w + 1) + hi + 1] - prefix[y * (w + 1) + lo];
                    if (y == 0) counts[x] = static_cast<double>(hi - lo + 1);
                }
            for (size_t y = 0; y < std::min(h, radius + 1); y++)
                for (size_t x = 0; x < w; x++) columns[x] += rows[y * w + x];
            for (size_t y = 0; y < h; y++) {
                const size_t lo = static_cast<size_t>(std::max<int64_t>(0, static_cast<int64_t>(y) - r));
                const size_t hi = static_cast<size_t>(std::min<int64_t>(static_cast<int64_t>(h) - 1, static_cast<int64_t>(y) + r));
                const double tall = static_cast<double>(hi - lo + 1);
                for (size_t x = 0; x < w; x++) out[y * w + x] = reduce == kSum ? columns[x] : columns[x] / (counts[x] * tall);
                if (y + 1 < h) {
                    if (static_cast<int64_t>(y) - r >= 0)
                        for (size_t x = 0; x < w; x++) columns[x] -= rows[(y - radius) * w + x];
                    if (y + radius + 1 < h)
                        for (size_t x = 0; x < w; x++) columns[x] += rows[(y + radius + 1) * w + x];
                }
            }
            return out;
        }
        for (size_t y = 0; y < h; y++)
            for (size_t x = 0; x < w; x++) {
                double sum = 0, n = 0;
                for (int64_t dy = -r; dy <= r; dy++) {
                    const int64_t yy = static_cast<int64_t>(y) + dy;
                    if (yy < 0 || yy >= static_cast<int64_t>(h)) continue;
                    const int64_t half = static_cast<int64_t>(discHalf(radius, dy));
                    const size_t lo = static_cast<size_t>(std::max<int64_t>(0, static_cast<int64_t>(x) - half));
                    const size_t hi = static_cast<size_t>(std::min<int64_t>(static_cast<int64_t>(w) - 1, static_cast<int64_t>(x) + half));
                    sum += prefix[static_cast<size_t>(yy) * (w + 1) + hi + 1] - prefix[static_cast<size_t>(yy) * (w + 1) + lo];
                    n += static_cast<double>(hi - lo + 1);
                }
                out[y * w + x] = reduce == kSum ? sum : sum / n;
            }
        return out;
    }
    const bool greatest = reduce == kMax;
    std::vector<double> ahead, behind, line(std::max(w, h)), across(std::max(w, h));
    if (!disc) {
        std::vector<double> rows(w * h);
        for (size_t y = 0; y < h; y++) runningExtreme(&in[y * w], w, radius, greatest, &rows[y * w], ahead, behind);
        for (size_t x = 0; x < w; x++) {
            for (size_t y = 0; y < h; y++) line[y] = rows[y * w + x];
            runningExtreme(line.data(), h, radius, greatest, across.data(), ahead, behind);
            for (size_t y = 0; y < h; y++) out[y * w + x] = across[y];
        }
        return out;
    }
    // A disc: each row of it, the row's runs of its width, combined.
    std::fill(out.begin(), out.end(), greatest ? -std::numeric_limits<double>::infinity() : std::numeric_limits<double>::infinity());
    for (int64_t dy = -r; dy <= r; dy++) {
        const size_t half = discHalf(radius, dy);
        for (size_t y = 0; y < h; y++) {
            const int64_t yy = static_cast<int64_t>(y) + dy;
            if (yy < 0 || yy >= static_cast<int64_t>(h)) continue;
            runningExtreme(&in[static_cast<size_t>(yy) * w], w, half, greatest, line.data(), ahead, behind);
            for (size_t x = 0; x < w; x++) out[y * w + x] = greatest ? std::max(out[y * w + x], line[x]) : std::min(out[y * w + x], line[x]);
        }
    }
    return out;
}

// The squared distance along a line to the nearest cell that is 0 in `f` (Felzenszwalb and
// Huttenlocher's lower envelope of parabolas).
void distanceLine(const double* f, size_t n, double* d, std::vector<size_t>& v, std::vector<double>& z) {
    const double inf = std::numeric_limits<double>::infinity();
    v.assign(n, 0);
    z.assign(n + 1, 0);
    size_t k = 0;
    // Cells that are infinitely far are left out of the envelope.
    size_t first = 0;
    while (first < n && f[first] == inf) first++;
    if (first == n) {
        for (size_t q = 0; q < n; q++) d[q] = inf;
        return;
    }
    v[0] = first;
    z[0] = -inf;
    z[1] = inf;
    for (size_t q = first + 1; q < n; q++) {
        if (f[q] == inf) continue;
        double s;
        for (;;) {
            const double p = static_cast<double>(v[k]);
            const double qq = static_cast<double>(q);
            s = ((f[q] + qq * qq) - (f[v[k]] + p * p)) / (2 * qq - 2 * p);
            if (s <= z[k] && k > 0) {
                k--;
                continue;
            }
            break;
        }
        if (s <= z[k]) {
            v[k] = q;  // k is 0 and the new parabola is lower everywhere
            z[k + 1] = inf;
            continue;
        }
        k++;
        v[k] = q;
        z[k] = s;
        z[k + 1] = inf;
    }
    k = 0;
    for (size_t q = 0; q < n; q++) {
        while (z[k + 1] < static_cast<double>(q)) k++;
        const double dq = static_cast<double>(q) - static_cast<double>(v[k]);
        d[q] = dq * dq + f[v[k]];
    }
}

// How different two cells are: for colours the most any part differs, alpha included.
double difference(const Numbers& n, const uint8_t* c, size_t a, size_t b) {
    if (c == nullptr) return std::fabs(n.f(a) - n.f(b));
    int most = 0;
    for (int k = 0; k < 4; k++) most = std::max(most, std::abs(static_cast<int>(c[a * 4 + k]) - static_cast<int>(c[b * 4 + k])));
    return most;
}

}  // namespace

void registerNumeric(VM& vm) {
    ObjClass* lib = vm.alloc<ObjClass>(vm.intern("__num"));
    auto def = [&](const char* name, int minArgs, int maxArgs, NativeFn fn) {
        lib->statics.set(vm.intern(name), Value::object(vm.alloc<ObjNative>(std::string("__num.") + name, minArgs, maxArgs, fn)));
    };

    // ---- Lists of numbers ----

    // fast(list): whether a list is a packed List<int> or List<float>, which sum, min and max
    // work on in C++.
    def("fast", 1, 1, [](VM&, int, Value* args) -> Value {
        if (!args[0].isObjType(ObjType::List)) return Value::boolean(false);
        const ObjList* list = asList(args[0]);
        return Value::boolean(list->packed() && !list->pack->nullable && (list->pack->store == ListStore::Ints || list->pack->store == ListStore::Floats));
    });
    // sum(list): the total of a List<int> (an overflow an ArithmeticException) or List<float>.
    def("sum", 1, 1, [](VM& vm, int, Value* args) -> Value {
        Numbers n = numbers(vm, args[0], "the list");
        if (n.isFloat) {
            double total = 0;
            for (size_t k = 0; k < n.count; k++) total += n.floats[k];
            return Value::number(total);
        }
        int64_t total = 0;
        for (size_t k = 0; k < n.count; k++) {
            int64_t next;
            if (addOverflows(total, n.ints[k], next)) throw vm.overflowError(Op::Add, total, n.ints[k]);
            total = next;
        }
        return Value::integer(total);
    });
    // extreme(list, greatest): the least or greatest item; null for an empty list, or one with
    // a NaN in it (left to the standard library's order of floats).
    def("extreme", 2, 2, [](VM& vm, int, Value* args) -> Value {
        Numbers n = numbers(vm, args[0], "the list");
        if (n.count == 0) return Value::null();
        const bool greatest = args[1].truthy();
        if (n.isFloat) {
            double best = n.floats[0];
            for (size_t k = 0; k < n.count; k++) {
                const double v = n.floats[k];
                if (std::isnan(v)) return Value::null();
                if (greatest ? v > best : v < best) best = v;
            }
            return Value::number(best);
        }
        int64_t best = n.ints[0];
        for (size_t k = 1; k < n.count; k++) best = greatest ? std::max(best, n.ints[k]) : std::min(best, n.ints[k]);
        return Value::integer(best);
    });
    // each(op, a, b, into): item by item, `b` a list of the same size or one number: plus,
    // minus, times, div, and the three that wrap round.
    def("each", 4, 4, [](VM& vm, int, Value* args) -> Value {
        const auto op = static_cast<EachOp>(vm.toInt(args[0], "op"));
        if (anyNumbers(args[1]) || anyNumbers(args[2]) || anyNumbers(args[3])) {
            // A list of ints and floats together: an item at a time, each as + takes it.
            const size_t na = sizeOfArg(vm, args[1], "the list"), nb = sizeOfArg(vm, args[2], "the other list");
            if (na != SIZE_MAX && nb != SIZE_MAX && na != nb) fail(vm, Thrown::IllegalArgumentException, "the lists' sizes differ: " + std::to_string(na) + " and " + std::to_string(nb));
            const size_t count = na != SIZE_MAX ? na : nb;
            ObjList* out;
            if (!args[3].isNull()) {
                out = listArg(vm, args[3], "into");
                if (out->size() != count) fail(vm, Thrown::IllegalArgumentException, "into has " + std::to_string(out->size()) + " items, not " + std::to_string(count));
            } else {
                out = newNumbers(vm, true, count, args[1].isObjType(ObjType::List) ? asList(args[1]) : nullptr);
            }
            TempRoot keep(vm, Value::object(out));
            for (size_t k = 0; k < count; k++) vm.listSet(out, k, arith(vm, op, numberAt(vm, args[1], k, "the list"), numberAt(vm, args[2], k, "the other list")));
            return Value::object(out);
        }
        const Numbers a = numbers(vm, args[1], "the list");
        const Numbers b = numbers(vm, args[2], "the other list");
        const size_t count = matched(vm, a, b);
        const bool wraps = op >= kPlusWrap;
        if (wraps && (a.isFloat || b.isFloat)) fail(vm, Thrown::ClassCastException, "the ...Wrapped forms are for ints");
        bool isFloat = false;
        ObjList* out = resultFor(vm, args[1], args[3], count, isFloat, a.isFloat || b.isFloat);
        TempRoot keep(vm, Value::object(out));
        if (isFloat) {
            std::vector<double> r(count);
            for (size_t k = 0; k < count; k++) {
                const double x = a.f(k), y = b.f(k);
                r[k] = op == kPlus ? x + y : op == kMinus ? x - y : op == kTimes ? x * y : x / y;
            }
            store(vm, out, r);
        } else {
            std::vector<int64_t> r(count);
            for (size_t k = 0; k < count; k++) {
                const int64_t x = a.i(k), y = b.i(k);
                int64_t z = 0;
                switch (op) {
                    case kPlus: if (addOverflows(x, y, z)) throw vm.overflowError(Op::Add, x, y); break;
                    case kMinus: if (subOverflows(x, y, z)) throw vm.overflowError(Op::Sub, x, y); break;
                    case kTimes: if (mulOverflows(x, y, z)) throw vm.overflowError(Op::Mul, x, y); break;
                    case kDiv:
                        if (y == 0) fail(vm, Thrown::ArithmeticException, "division by zero");
                        if (y == -1 && x == INT64_MIN) throw vm.overflowError(Op::Div, x, y);
                        z = x / y;
                        break;
                    case kPlusWrap: z = static_cast<int64_t>(static_cast<uint64_t>(x) + static_cast<uint64_t>(y)); break;
                    case kMinusWrap: z = static_cast<int64_t>(static_cast<uint64_t>(x) - static_cast<uint64_t>(y)); break;
                    default: z = static_cast<int64_t>(static_cast<uint64_t>(x) * static_cast<uint64_t>(y)); break;
                }
                r[k] = z;
            }
            store(vm, out, r);
        }
        return Value::object(out);
    });
    // clamp(a, low, high, into): each item held from `low` to `high`.
    def("clamp", 4, 4, [](VM& vm, int, Value* args) -> Value {
        if (anyNumbers(args[0])) {
            const size_t count = asList(args[0])->size();
            const Value lo = numberAt(vm, args[1], 0, "low"), hi = numberAt(vm, args[2], 0, "high");
            if (lo.toDouble() > hi.toDouble()) fail(vm, Thrown::IllegalArgumentException, "low is above high");
            ObjList* out = args[3].isNull() ? newNumbers(vm, true, count, asList(args[0])) : listArg(vm, args[3], "into");
            TempRoot keep(vm, Value::object(out));
            for (size_t k = 0; k < count; k++) {
                const Value v = numberAt(vm, args[0], k, "the list");
                vm.listSet(out, k, v.toDouble() < lo.toDouble() ? lo : v.toDouble() > hi.toDouble() ? hi : v);
            }
            return Value::object(out);
        }
        const Numbers a = numbers(vm, args[0], "the list");
        const Numbers lo = numbers(vm, args[1], "low"), hi = numbers(vm, args[2], "high");
        if (!lo.single || !hi.single) fail(vm, Thrown::IllegalArgumentException, "clampEach takes two numbers");
        if (lo.f(0) > hi.f(0)) fail(vm, Thrown::IllegalArgumentException, "low is above high");
        const bool floats = a.isFloat || lo.isFloat || hi.isFloat;
        bool isFloat = false;
        ObjList* out = resultFor(vm, args[0], args[3], a.count, isFloat, floats);
        TempRoot keep(vm, Value::object(out));
        if (isFloat) {
            std::vector<double> r(a.count);
            for (size_t k = 0; k < a.count; k++) r[k] = std::clamp(a.f(k), lo.f(0), hi.f(0));
            store(vm, out, r);
        } else {
            std::vector<int64_t> r(a.count);
            for (size_t k = 0; k < a.count; k++) r[k] = std::clamp(a.i(k), lo.i(0), hi.i(0));
            store(vm, out, r);
        }
        return Value::object(out);
    });
    // abs(a, into): each item made positive.
    def("abs", 2, 2, [](VM& vm, int, Value* args) -> Value {
        if (anyNumbers(args[0])) {
            const size_t count = asList(args[0])->size();
            ObjList* out = args[1].isNull() ? newNumbers(vm, true, count, asList(args[0])) : listArg(vm, args[1], "into");
            TempRoot keep(vm, Value::object(out));
            for (size_t k = 0; k < count; k++) {
                const Value v = numberAt(vm, args[0], k, "the list");
                vm.listSet(out, k, v.isInt() ? (v.as.i < 0 ? arith(vm, kMinus, Value::integer(0), v) : v) : Value::number(std::fabs(v.as.d)));
            }
            return Value::object(out);
        }
        const Numbers a = numbers(vm, args[0], "the list");
        bool isFloat = false;
        ObjList* out = resultFor(vm, args[0], args[1], a.count, isFloat, a.isFloat);
        TempRoot keep(vm, Value::object(out));
        if (isFloat) {
            std::vector<double> r(a.count);
            for (size_t k = 0; k < a.count; k++) r[k] = std::fabs(a.f(k));
            store(vm, out, r);
        } else {
            std::vector<int64_t> r(a.count);
            for (size_t k = 0; k < a.count; k++) {
                if (a.i(k) == INT64_MIN) throw vm.overflowError(Op::Sub, 0, a.i(k));
                r[k] = a.i(k) < 0 ? -a.i(k) : a.i(k);
            }
            store(vm, out, r);
        }
        return Value::object(out);
    });
    // dot(a, b): the sum of the products of the items in turn.
    def("dot", 2, 2, [](VM& vm, int, Value* args) -> Value {
        if (anyNumbers(args[0]) || anyNumbers(args[1])) {
            const size_t na = sizeOfArg(vm, args[0], "the list"), nb = sizeOfArg(vm, args[1], "the other list");
            if (na != nb || na == SIZE_MAX) fail(vm, Thrown::IllegalArgumentException, "dot takes two lists of one size, not " + std::to_string(na) + " and " + std::to_string(nb));
            Value total = Value::integer(0);
            for (size_t k = 0; k < na; k++) total = arith(vm, kPlus, total, arith(vm, kTimes, numberAt(vm, args[0], k, "the list"), numberAt(vm, args[1], k, "the other list")));
            return total;
        }
        const Numbers a = numbers(vm, args[0], "the list"), b = numbers(vm, args[1], "the other list");
        if (a.single || b.single || a.count != b.count) fail(vm, Thrown::IllegalArgumentException, "dot takes two lists of one size, not " + std::to_string(a.count) + " and " + std::to_string(b.count));
        if (a.isFloat || b.isFloat) {
            double total = 0;
            for (size_t k = 0; k < a.count; k++) total += a.f(k) * b.f(k);
            return Value::number(total);
        }
        int64_t total = 0;
        for (size_t k = 0; k < a.count; k++) {
            int64_t p, next;
            if (mulOverflows(a.i(k), b.i(k), p)) throw vm.overflowError(Op::Mul, a.i(k), b.i(k));
            if (addOverflows(total, p, next)) throw vm.overflowError(Op::Add, total, p);
            total = next;
        }
        return Value::integer(total);
    });
    // running(a, into): each item the sum of those up to it.
    def("running", 2, 2, [](VM& vm, int, Value* args) -> Value {
        if (anyNumbers(args[0])) {
            const size_t count = asList(args[0])->size();
            ObjList* out = args[1].isNull() ? newNumbers(vm, true, count, asList(args[0])) : listArg(vm, args[1], "into");
            TempRoot keep(vm, Value::object(out));
            Value total = Value::integer(0);
            for (size_t k = 0; k < count; k++) vm.listSet(out, k, total = arith(vm, kPlus, total, numberAt(vm, args[0], k, "the list")));
            return Value::object(out);
        }
        const Numbers a = numbers(vm, args[0], "the list");
        bool isFloat = false;
        ObjList* out = resultFor(vm, args[0], args[1], a.count, isFloat, a.isFloat);
        TempRoot keep(vm, Value::object(out));
        if (isFloat) {
            std::vector<double> r(a.count);
            double total = 0;
            for (size_t k = 0; k < a.count; k++) r[k] = total += a.f(k);
            store(vm, out, r);
        } else {
            std::vector<int64_t> r(a.count);
            int64_t total = 0;
            for (size_t k = 0; k < a.count; k++) {
                if (addOverflows(total, a.i(k), total)) throw vm.overflowError(Op::Add, total - a.i(k), a.i(k));
                r[k] = total;
            }
            store(vm, out, r);
        }
        return Value::object(out);
    });
    // copyInto(from, to, toOffset, start, end): a run of items copied into another list of the
    // same item type, or Bytes into Bytes, at once.
    def("copyInto", 5, 5, [](VM& vm, int, Value* args) -> Value {
        const size_t offset = sizeArg(vm, args[2], "destinationOffset"), start = sizeArg(vm, args[3], "startIndex"), end = sizeArg(vm, args[4], "endIndex");
        auto bounds = [&](size_t fromSize, size_t toSize) {
            if (start > end || end > fromSize)
                fail(vm, Thrown::IndexOutOfBoundsException, "the run " + std::to_string(start) + " to " + std::to_string(end) + " is not inside the " + std::to_string(fromSize) + " items");
            if (offset + (end - start) > toSize)
                fail(vm, Thrown::IndexOutOfBoundsException, std::to_string(end - start) + " items at " + std::to_string(offset) + " do not fit in the " + std::to_string(toSize) + " of the destination");
        };
        if (args[0].isObjType(ObjType::Bytes) && args[1].isObjType(ObjType::Bytes)) {
            const std::vector<uint8_t>& from = asBytes(args[0])->data;
            std::vector<uint8_t>& to = asBytes(args[1])->data;
            bounds(from.size(), to.size());
            std::memmove(to.data() + offset, from.data() + start, end - start);
            return args[1];
        }
        ObjList* from = listArg(vm, args[0], "the list");
        ObjList* to = listArg(vm, args[1], "the destination");
        bounds(from->size(), to->size());
        const bool same = from->packed() && to->packed() && from->pack->store == to->pack->store && from->pack->width == to->pack->width &&
                          from->pack->itemClass == to->pack->itemClass && from->pack->narrow == to->pack->narrow && !from->pack->nullable && !to->pack->nullable;
        if (same) {
            if (end > start) std::memmove(vm.itemAt(to, offset), vm.itemAt(from, start), (end - start) * to->pack->width);
        } else if (from == to && offset > start) {
            for (size_t k = end - start; k-- > 0;) vm.listSet(to, offset + k, vm.listGet(from, start + k));
        } else {
            for (size_t k = 0; k < end - start; k++) vm.listSet(to, offset + k, vm.listGet(from, start + k));
        }
        return args[1];
    });
    // addAt(list, indices, values): each value added at its index, an index given more than once
    // adding each time (numpy's add.at); `values` a list of the indices' size or one number.
    def("addAt", 3, 3, [](VM& vm, int, Value* args) -> Value {
        ObjList* list = listArg(vm, args[0], "the list");
        const Numbers idx = numbers(vm, args[1], "indices");
        if (idx.isFloat) fail(vm, Thrown::ClassCastException, "indices are ints");
        const Numbers values = numbers(vm, args[2], "values");
        const size_t count = idx.single ? values.count : matched(vm, idx, values);
        Numbers now = numbers(vm, args[0], "the list");
        const bool floats = now.isFloat || values.isFloat;
        const size_t size = list->size();
        auto at = [&](size_t k) {
            const int64_t i = idx.i(k);
            if (i < 0 || static_cast<size_t>(i) >= size) fail(vm, Thrown::IndexOutOfBoundsException, "index " + std::to_string(i) + " is outside the " + std::to_string(size) + " items");
            return static_cast<size_t>(i);
        };
        if (floats) {
            std::vector<double> r(size);
            for (size_t k = 0; k < size; k++) r[k] = now.f(k);
            for (size_t k = 0; k < count; k++) r[at(k)] += values.f(k);
            store(vm, list, r);
        } else {
            std::vector<int64_t> r(now.ints, now.ints + size);
            for (size_t k = 0; k < count; k++) {
                const size_t i = at(k);
                if (addOverflows(r[i], values.i(k), r[i])) throw vm.overflowError(Op::Add, r[i] - values.i(k), values.i(k));
            }
            store(vm, list, r);
        }
        return Value::null();
    });
    // histogram(cells, bins, from, to): how many items fall in each of `bins` equal parts of
    // `from` to `to` (the least and the most when null), the last part taking `to` itself. A
    // picture's is WinterWolf's (numeric.ww).
    def("histogram", 4, 4, [](VM& vm, int, Value* args) -> Value {
        const size_t bins = sizeArg(vm, args[1], "bins");
        if (bins == 0) fail(vm, Thrown::IllegalArgumentException, "a histogram has 1 bin or more");
        const Numbers n = numbers(vm, args[0], "the grid");
        std::vector<double> plane(n.count);
        double lo = std::numeric_limits<double>::infinity(), hi = -lo;
        for (size_t k = 0; k < n.count; k++) {
            plane[k] = n.f(k);
            if (!std::isnan(plane[k])) lo = std::min(lo, plane[k]), hi = std::max(hi, plane[k]);
        }
        if (!args[2].isNull()) lo = vm.toNumber(args[2], "from");
        if (!args[3].isNull()) hi = vm.toNumber(args[3], "to");
        if (!(lo <= hi)) lo = hi = 0;
        std::vector<int64_t> counts(bins, 0);
        const double span = hi - lo;
        for (double v : plane) {
            if (std::isnan(v) || v < lo || v > hi) continue;
            size_t b = span > 0 ? static_cast<size_t>((v - lo) / span * static_cast<double>(bins)) : 0;
            if (b >= bins) b = bins - 1;
            counts[b]++;
        }
        ObjList* made = newNumbers(vm, false, counts.size());
        TempRoot keep(vm, Value::object(made));
        store(vm, made, counts);
        return Value::object(made);
    });

    // ---- Grids ----

    // sums(cells, width, height, columns): a List of each row's total, or each column's.
    def("sums", 4, 4, [](VM& vm, int, Value* args) -> Value {
        const Numbers n = numbers(vm, args[0], "the grid");
        const size_t w = sizeArg(vm, args[1], "width"), h = sizeArg(vm, args[2], "height");
        const bool columns = args[3].truthy();
        const size_t count = columns ? w : h;
        if (n.isFloat) {
            std::vector<double> r(count);
            for (size_t y = 0; y < h; y++)
                for (size_t x = 0; x < w; x++) r[columns ? x : y] += n.f(y * w + x);
            ObjList* out = newNumbers(vm, true, count);
            TempRoot keep(vm, Value::object(out));
            store(vm, out, r);
            return Value::object(out);
        }
        std::vector<int64_t> r(count);
        for (size_t y = 0; y < h; y++)
            for (size_t x = 0; x < w; x++) {
                int64_t& t = r[columns ? x : y];
                if (addOverflows(t, n.i(y * w + x), t)) throw vm.overflowError(Op::Add, t - n.i(y * w + x), n.i(y * w + x));
            }
        ObjList* out = newNumbers(vm, false, count);
        TempRoot keep(vm, Value::object(out));
        store(vm, out, r);
        return Value::object(out);
    });
    // transposed(cells, width, height, out): the grid turned on its diagonal, into `out`'s
    // cells (height by width, of the same type).
    def("transposed", 4, 4, [](VM& vm, int, Value* args) -> Value {
        ObjList* from = listArg(vm, args[0], "the grid");
        ObjList* to = listArg(vm, args[3], "the grid written into");
        const size_t w = sizeArg(vm, args[1], "width"), h = sizeArg(vm, args[2], "height");
        if (from->size() != w * h || to->size() != w * h) fail(vm, Thrown::IllegalArgumentException, "the grids' sizes do not match");
        if (from->packed() && to->packed() && from->pack->width == to->pack->width && !from->pack->nullable && !to->pack->nullable) {
            const size_t width = from->pack->width;
            const uint8_t* a = from->pack->block.data();
            uint8_t* b = to->pack->block.data();
            for (size_t y = 0; y < h; y++)
                for (size_t x = 0; x < w; x++) std::memcpy(b + (x * h + y) * width, a + (y * w + x) * width, width);
            return Value::null();
        }
        for (size_t y = 0; y < h; y++)
            for (size_t x = 0; x < w; x++) vm.listSet(to, x * h + y, vm.listGet(from, y * w + x));
        return Value::null();
    });
    // convolve(cells, width, height, kernel, kw, kh, out, colour): each cell the sum of its
    // neighbours weighted by the kernel centred on it, the edge cells repeated past the edge;
    // colours each part on its own, multiplied by alpha on the way.
    def("convolve", 8, 8, [](VM& vm, int, Value* args) -> Value {
        const size_t w = sizeArg(vm, args[1], "width"), h = sizeArg(vm, args[2], "height");
        const size_t kw = sizeArg(vm, args[4], "the kernel's width"), kh = sizeArg(vm, args[5], "the kernel's height");
        const bool colour = args[7].truthy();
        const Numbers k = numbers(vm, args[3], "the kernel");
        if (k.count != kw * kh || kw == 0 || kh == 0) fail(vm, Thrown::IllegalArgumentException, "the kernel is " + std::to_string(kw) + " by " + std::to_string(kh));
        if (colour) {
            const uint8_t* in = colorCells(vm, args[0], w * h, "the grid");
            uint8_t* out = colorCells(vm, args[6], w * h, "the grid written into");
            if (w == 0 || h == 0) return Value::null();
            std::vector<float> weights(kw * kh);
            for (size_t i = 0; i < kw * kh; i++) weights[i] = static_cast<float>(k.f(i));
            // Rows multiplied by alpha as they are first wanted, into a ring of the kernel's height
            // (row r in place r % kh): the rows a pixel's window takes are kh in a run, so never
            // two in one place, and a row leaves only once no window takes it again. Each answer
            // row written straight into `out`, which may be `in`: a row is in the ring before
            // its answer is written over it (2026-11-13: the whole picture as floats, 192 MB for
            // 12 million pixels, and the answer apart, took 127 ms for a 1 by 1 kernel).
            const int64_t cx = static_cast<int64_t>(kw / 2), cy = static_cast<int64_t>(kh / 2);
            std::vector<float> ring(kh * w * 4), acc(w * 4);
            std::vector<int64_t> held(kh, -1);
            std::vector<const float*> rows(kh);
            // The columns whose whole window is inside the grid: no edge to repeat.
            const int64_t inFrom = cx, inTo = static_cast<int64_t>(w) - static_cast<int64_t>(kw) + cx;
            // A pixel's four parts side by side, worked out together: each part the same
            // multiplications and additions, in the same order, as one at a time.
            using F4 = float __attribute__((vector_size(16)));
            auto load4 = [](const float* p) {
                F4 v;
                std::memcpy(&v, p, sizeof v);
                return v;
            };
            for (size_t y = 0; y < h; y++) {
                for (size_t j = 0; j < kh; j++) {
                    const int64_t yy = std::clamp<int64_t>(static_cast<int64_t>(y + j) - cy, 0, static_cast<int64_t>(h) - 1);
                    const size_t place = static_cast<size_t>(yy) % kh;
                    if (held[place] != yy) {
                        premultipliedRow(in + static_cast<size_t>(yy) * w * 4, w, &ring[place * w * 4]);
                        held[place] = yy;
                    }
                    rows[j] = &ring[place * w * 4];
                }
                // Each pixel: its window's rows in order, each row's taps in order, as before
                // (an answer the sum over rows of each row's sum).
                for (int64_t x = 0; x < static_cast<int64_t>(w); x++) {
                    F4 total = {0, 0, 0, 0};
                    for (size_t j = 0; j < kh; j++) {
                        const float* row = rows[j];
                        const float* kr = &weights[j * kw];
                        F4 sum = {0, 0, 0, 0};
                        if (x >= inFrom && x <= inTo) {
                            const float* at = row + (x - cx) * 4;
                            for (size_t i = 0; i < kw; i++, at += 4) sum += load4(at) * kr[i];
                        } else {
                            for (size_t i = 0; i < kw; i++) {
                                const float* at = row + std::clamp<int64_t>(x + static_cast<int64_t>(i) - cx, 0, static_cast<int64_t>(w) - 1) * 4;
                                sum += load4(at) * kr[i];
                            }
                        }
                        total += sum;
                    }
                    std::memcpy(&acc[static_cast<size_t>(x) * 4], &total, sizeof total);
                }
                colorRow(acc.data(), w, out + y * w * 4);
            }
            return Value::null();
        }
        auto planes = planesOf(vm, args[0], w * h, colour, true);
        std::vector<std::vector<double>> result(planes.size(), std::vector<double>(w * h));
        const int64_t cx = static_cast<int64_t>(kw / 2), cy = static_cast<int64_t>(kh / 2);
        auto clampX = [&](int64_t x) { return static_cast<size_t>(std::clamp<int64_t>(x, 0, static_cast<int64_t>(w) - 1)); };
        auto clampY = [&](int64_t y) { return static_cast<size_t>(std::clamp<int64_t>(y, 0, static_cast<int64_t>(h) - 1)); };
        for (size_t p = 0; p < planes.size(); p++) {
            const std::vector<double>& in = planes[p];
            std::vector<double>& out = result[p];
            for (size_t y = 0; y < h; y++)
                for (size_t x = 0; x < w; x++) {
                    double sum = 0;
                    for (size_t j = 0; j < kh; j++) {
                        const size_t yy = clampY(static_cast<int64_t>(y + j) - cy);
                        const double* row = &in[yy * w];
                        for (size_t i = 0; i < kw; i++) sum += row[clampX(static_cast<int64_t>(x + i) - cx)] * k.f(j * kw + i);
                    }
                    out[y * w + x] = sum;
                }
        }
        if (colour) writePlanes(vm, args[6], result, w * h, true, true);
        else store(vm, listArg(vm, args[6], "the grid written into"), result[0]);
        return Value::null();
    });
    // scaled(cells, width, height, toWidth, toHeight, interpolation, out, colour): the grid
    // resampled to another size, area-averaged each way it shrinks, read between cells by the
    // interpolation each way it grows.
    def("scaled", 8, 8, [](VM& vm, int, Value* args) -> Value {
        const size_t w = sizeArg(vm, args[1], "width"), h = sizeArg(vm, args[2], "height");
        const size_t nw = sizeArg(vm, args[3], "width"), nh = sizeArg(vm, args[4], "height");
        const int interpolation = static_cast<int>(vm.toInt(args[5], "interpolation"));
        const bool colour = args[7].truthy();
        if (w == 0 || h == 0) {
            if (nw * nh != 0) fail(vm, Thrown::IllegalArgumentException, "an empty grid cannot be scaled to " + std::to_string(nw) + " by " + std::to_string(nh));
            return Value::null();
        }
        const Taps across = tapsFor(w, nw, interpolation), down = tapsFor(h, nh, interpolation);
        if (colour) {
            const uint8_t* in = colorCells(vm, args[0], w * h, "the grid");
            uint8_t* out = colorCells(vm, args[6], nw * nh, "the grid written into");
            // Source rows made the new width, kept for the next rows that read them too.
            const size_t kept = 8;
            std::vector<int64_t> keptRow(kept, -1);
            std::vector<float> keptData(kept * nw * 4), line(w * 4), acc(nw * 4);
            auto across4 = [&](size_t sy) -> const float* {
                const size_t slot = sy % kept;
                float* row = &keptData[slot * nw * 4];
                if (keptRow[slot] == static_cast<int64_t>(sy)) return row;
                premultipliedRow(in + sy * w * 4, w, line.data());
                for (size_t x = 0; x < nw; x++) {
                    float r = 0, g = 0, b = 0, a = 0;
                    for (size_t t = across.start[x]; t < across.start[x + 1]; t++) {
                        const float* c = &line[across.index[t] * 4];
                        const float wt = static_cast<float>(across.weight[t]);
                        r += c[0] * wt, g += c[1] * wt, b += c[2] * wt, a += c[3] * wt;
                    }
                    row[x * 4] = r, row[x * 4 + 1] = g, row[x * 4 + 2] = b, row[x * 4 + 3] = a;
                }
                keptRow[slot] = static_cast<int64_t>(sy);
                return row;
            };
            std::vector<uint8_t> done(nw * nh * 4);
            for (size_t y = 0; y < nh; y++) {
                std::fill(acc.begin(), acc.end(), 0.0f);
                for (size_t t = down.start[y]; t < down.start[y + 1]; t++) {
                    const float* row = across4(down.index[t]);
                    const float wt = static_cast<float>(down.weight[t]);
                    for (size_t i = 0; i < nw * 4; i++) acc[i] += row[i] * wt;
                }
                colorRow(acc.data(), nw, &done[y * nw * 4]);
            }
            std::memcpy(out, done.data(), done.size());
            return Value::null();
        }
        auto planes = planesOf(vm, args[0], w * h, colour, true);
        std::vector<std::vector<double>> result(planes.size(), std::vector<double>(nw * nh));
        std::vector<double> mid(nw * h);
        for (size_t p = 0; p < planes.size(); p++) {
            for (size_t y = 0; y < h; y++)
                for (size_t x = 0; x < nw; x++) {
                    double s = 0;
                    for (size_t t = across.start[x]; t < across.start[x + 1]; t++) s += planes[p][y * w + across.index[t]] * across.weight[t];
                    mid[y * nw + x] = s;
                }
            for (size_t y = 0; y < nh; y++)
                for (size_t x = 0; x < nw; x++) {
                    double s = 0;
                    for (size_t t = down.start[y]; t < down.start[y + 1]; t++) s += mid[down.index[t] * nw + x] * down.weight[t];
                    result[p][y * nw + x] = s;
                }
        }
        writePlanes(vm, args[6], result, nw * nh, colour, true);
        return Value::null();
    });
    // fillWhere(cells, mask, color): the cells where a grid of bools is true set to one colour.
    def("fillWhere", 3, 3, [](VM& vm, int, Value* args) -> Value {
        ObjList* cells = listArg(vm, args[0], "the grid");
        ObjList* mask = listArg(vm, args[1], "the mask");
        if (!mask->packed() || mask->pack->store != ListStore::Bools || mask->size() != cells->size())
            fail(vm, Thrown::IllegalArgumentException, "the mask is a grid of bools the grid's size");
        const uint8_t* m = mask->pack->block.data();
        for (size_t k = 0; k < cells->size(); k++)
            if (m[k] != 0) vm.listSet(cells, k, args[2]);
        return Value::null();
    });
    // countDifferent(a, b): how many cells of two grids of one size differ.
    def("countDifferent", 2, 2, [](VM& vm, int, Value* args) -> Value {
        ObjList* a = listArg(vm, args[0], "the grid");
        ObjList* b = listArg(vm, args[1], "the other grid");
        if (a->size() != b->size()) fail(vm, Thrown::IllegalArgumentException, "the grids' sizes differ: " + std::to_string(a->size()) + " and " + std::to_string(b->size()) + " cells");
        int64_t n = 0;
        if (a->packed() && b->packed() && a->pack->width == b->pack->width && a->pack->store == b->pack->store && !a->pack->nullable && !b->pack->nullable) {
            const size_t w = a->pack->width;
            const uint8_t* x = a->pack->block.data();
            const uint8_t* y = b->pack->block.data();
            for (size_t k = 0; k < a->size(); k++) n += std::memcmp(x + k * w, y + k * w, w) != 0;
            return Value::integer(n);
        }
        for (size_t k = 0; k < a->size(); k++) n += !vm.equals(vm.listGet(a, k), vm.listGet(b, k));
        return Value::integer(n);
    });
    // mapColors(cells, table, out): each colour put through a table of 33 by 33 by 33 colours
    // (ColorLookup's, Bytes of red, green, blue, alpha, red the slowest), between its colours by
    // trilinear interpolation; a colour's own alpha kept, times the table's.
    def("mapColors", 3, 3, [](VM& vm, int, Value* args) -> Value {
        const size_t n = listArg(vm, args[0], "the grid")->size();
        const uint8_t* in = colorCells(vm, args[0], n, "the grid");
        uint8_t* out = colorCells(vm, args[2], n, "the grid written into");
        if (!args[1].isObjType(ObjType::Bytes) || asBytes(args[1])->data.size() != 33 * 33 * 33 * 4) fail(vm, Thrown::IllegalArgumentException, "not a colour table");
        const uint8_t* t = asBytes(args[1])->data.data();
        for (size_t k = 0; k < n; k++) {
            const uint8_t* c = in + k * 4;
            double f[3];
            int lo[3];
            for (int i = 0; i < 3; i++) {
                const double at = c[i] * 32.0 / 255.0;
                lo[i] = std::min(31, static_cast<int>(at));
                f[i] = at - lo[i];
            }
            double mixed[4] = {0, 0, 0, 0};
            for (int corner = 0; corner < 8; corner++) {
                const int dr = corner >> 2 & 1, dg = corner >> 1 & 1, db = corner & 1;
                const double wgt = (dr ? f[0] : 1 - f[0]) * (dg ? f[1] : 1 - f[1]) * (db ? f[2] : 1 - f[2]);
                if (wgt == 0) continue;
                const uint8_t* e = t + ((static_cast<size_t>(lo[0] + dr) * 33 + static_cast<size_t>(lo[1] + dg)) * 33 + static_cast<size_t>(lo[2] + db)) * 4;
                for (int i = 0; i < 4; i++) mixed[i] += e[i] * wgt;
            }
            uint8_t* o = out + k * 4;
            for (int i = 0; i < 3; i++) o[i] = static_cast<uint8_t>(rounded(std::clamp(mixed[i], 0.0, 255.0)));
            o[3] = static_cast<uint8_t>(rounded(std::clamp(mixed[3], 0.0, 255.0) * c[3] / 255.0));
        }
        return Value::null();
    });
    // sampleAffine(cells, width, height, m0, m1, m2, m3, m4, m5, outWidth, outHeight,
    // interpolation, outside, out): a grid of colours `outWidth` by `outHeight`, each cell (x, y)
    // read as sample reads one from the place (m0 x + m2 y + m4, m1 x + m3 y + m5), which places
    // works out: the two in one pass, each place and each cell's colours (multiplied by alpha)
    // worked out as they are wanted (2026-11-13: an image turned took some 300 MB of places and
    // planes of doubles, and a second, through them). The same arithmetic in the same order as
    // places, planesOf, sample and writePlanes, so the same colours.
    def("sampleAffine", 14, 14, [](VM& vm, int, Value* args) -> Value {
        const size_t w = sizeArg(vm, args[1], "width"), h = sizeArg(vm, args[2], "height");
        double m[6];
        for (int i = 0; i < 6; i++) m[i] = vm.toNumber(args[3 + i], "a transform's number");
        const size_t ow = sizeArg(vm, args[9], "width"), oh = sizeArg(vm, args[10], "height");
        const int interpolation = static_cast<int>(vm.toInt(args[11], "interpolation"));
        if (w == 0 || h == 0) fail(vm, Thrown::IllegalArgumentException, "an empty grid has nothing to sample");
        const uint8_t* c = colorCells(vm, args[0], w * h, "the grid");
        double outside[4] = {0, 0, 0, 0};
        const bool hasOutside = !args[12].isNull();
        if (hasOutside) {
            const Numbers o = numbers(vm, args[12], "outside");
            if (o.count != 4) fail(vm, Thrown::IllegalArgumentException, "outside is a colour");
            const double a = o.f(3);
            outside[0] = o.f(0) * a / 255;
            outside[1] = o.f(1) * a / 255;
            outside[2] = o.f(2) * a / 255;
            outside[3] = a;
        }
        uint8_t* out = colorCells(vm, args[13], ow * oh, "the grid written into");
        const double W = static_cast<double>(w), H = static_cast<double>(h);
        // A cell's plane p as planesOf makes it: red, green, blue multiplied by alpha / 255; alpha.
        auto cell = [&](size_t p, int64_t cx, int64_t cy) -> double {
            cx = std::clamp<int64_t>(cx, 0, static_cast<int64_t>(w) - 1);
            cy = std::clamp<int64_t>(cy, 0, static_cast<int64_t>(h) - 1);
            const uint8_t* at = c + (static_cast<size_t>(cy) * w + static_cast<size_t>(cx)) * 4;
            const double a = at[3];
            if (p == 3) return a;
            const double mul = a / 255.0;
            return at[p] * mul;
        };
        auto byte = [](double v) { return static_cast<uint8_t>(v <= 0 ? 0 : v >= 255 ? 255 : rounded(v)); };
        for (size_t oy = 0; oy < oh; oy++)
            for (size_t ox = 0; ox < ow; ox++) {
                const double px = static_cast<double>(ox), py = static_cast<double>(oy);
                const double x = m[0] * px + m[2] * py + m[4];
                const double y = m[1] * px + m[3] * py + m[5];
                double v[4];
                if (hasOutside && !(x >= -0.5 && x < W - 0.5 && y >= -0.5 && y < H - 0.5)) {
                    for (int p = 0; p < 4; p++) v[p] = outside[p];
                } else if (interpolation == 1 && !std::isnan(x) && !std::isnan(y)) {
                    // Bilinear: the four cells round it fetched once, each part as cell() gives it.
                    const double fx = std::floor(x), fy = std::floor(y), u = x - fx, t = y - fy;
                    const int64_t ix = static_cast<int64_t>(fx), iy = static_cast<int64_t>(fy);
                    double q[4][4];
                    const int64_t qx[4] = {ix, ix + 1, ix, ix + 1}, qy[4] = {iy, iy, iy + 1, iy + 1};
                    for (int k = 0; k < 4; k++) {
                        const int64_t cx = std::clamp<int64_t>(qx[k], 0, static_cast<int64_t>(w) - 1);
                        const int64_t cy = std::clamp<int64_t>(qy[k], 0, static_cast<int64_t>(h) - 1);
                        const uint8_t* at = c + (static_cast<size_t>(cy) * w + static_cast<size_t>(cx)) * 4;
                        const double a = at[3];
                        const double mul = a / 255.0;
                        q[k][0] = at[0] * mul;
                        q[k][1] = at[1] * mul;
                        q[k][2] = at[2] * mul;
                        q[k][3] = a;
                    }
                    for (int p = 0; p < 4; p++) v[p] = (q[0][p] * (1 - u) + q[1][p] * u) * (1 - t) + (q[2][p] * (1 - u) + q[3][p] * u) * t;
                } else {
                    for (size_t p = 0; p < 4; p++) {
                        if (interpolation == 0 || std::isnan(x) || std::isnan(y)) {
                            v[p] = std::isnan(x) || std::isnan(y) ? 0 : cell(p, rounded(x), rounded(y));
                        } else if (interpolation == 1) {
                            const double fx = std::floor(x), fy = std::floor(y), u = x - fx, t = y - fy;
                            const int64_t ix = static_cast<int64_t>(fx), iy = static_cast<int64_t>(fy);
                            v[p] = (cell(p, ix, iy) * (1 - u) + cell(p, ix + 1, iy) * u) * (1 - t) +
                                   (cell(p, ix, iy + 1) * (1 - u) + cell(p, ix + 1, iy + 1) * u) * t;
                        } else {
                            const double fx = std::floor(x), fy = std::floor(y);
                            double wx[4], wy[4];
                            cubicWeights(x - fx, wx);
                            cubicWeights(y - fy, wy);
                            const int64_t ix = static_cast<int64_t>(fx), iy = static_cast<int64_t>(fy);
                            double s = 0;
                            for (int j = 0; j < 4; j++)
                                for (int i = 0; i < 4; i++) s += cell(p, ix - 1 + i, iy - 1 + j) * wx[i] * wy[j];
                            v[p] = s;
                        }
                    }
                }
                // As writePlanes writes a colour multiplied by alpha.
                uint8_t* d = out + (oy * ow + ox) * 4;
                const double a = std::clamp(v[3], 0.0, 255.0);
                const uint8_t alpha = byte(a);
                d[3] = alpha;
                const double mul = alpha == 0 ? 0.0 : 255.0 / a;
                d[0] = byte(v[0] * mul);
                d[1] = byte(v[1] * mul);
                d[2] = byte(v[2] * mul);
            }
        return Value::null();
    });
    // sample(cells, width, height, xs, ys, interpolation, outside, out, colour): each cell of
    // the result read from the place (xs[k], ys[k]) of the grid, whole numbers the middles of its
    // cells; outside the grid `outside`, or the nearest edge where it is null.
    def("sample", 9, 9, [](VM& vm, int, Value* args) -> Value {
        const size_t w = sizeArg(vm, args[1], "width"), h = sizeArg(vm, args[2], "height");
        const Numbers xs = numbers(vm, args[3], "xs"), ys = numbers(vm, args[4], "ys");
        const size_t n = matched(vm, xs, ys);
        const int interpolation = static_cast<int>(vm.toInt(args[5], "interpolation"));
        const bool colour = args[8].truthy();
        if (w == 0 || h == 0) fail(vm, Thrown::IllegalArgumentException, "an empty grid has nothing to sample");
        const auto planes = planesOf(vm, args[0], w * h, colour, true);
        // What lies outside, as planes do: multiplied by alpha for a colour.
        std::vector<double> outside;
        const bool hasOutside = !args[6].isNull();
        if (hasOutside) {
            // A colour comes as its parts, red, green, blue, alpha.
            const Numbers o = numbers(vm, args[6], "outside");
            if (colour) {
                if (o.count != 4) fail(vm, Thrown::IllegalArgumentException, "outside is a colour");
                const double a = o.f(3);
                outside = {o.f(0) * a / 255, o.f(1) * a / 255, o.f(2) * a / 255, a};
            } else {
                outside.push_back(o.f(0));
            }
        }
        std::vector<std::vector<double>> result(planes.size(), std::vector<double>(n));
        const double W = static_cast<double>(w), H = static_cast<double>(h);
        for (size_t k = 0; k < n; k++) {
            const double x = xs.f(k), y = ys.f(k);
            if (hasOutside && !(x >= -0.5 && x < W - 0.5 && y >= -0.5 && y < H - 0.5)) {
                for (size_t p = 0; p < planes.size(); p++) result[p][k] = outside[p];
                continue;
            }
            auto cell = [&](size_t p, int64_t cx, int64_t cy) {
                cx = std::clamp<int64_t>(cx, 0, static_cast<int64_t>(w) - 1);
                cy = std::clamp<int64_t>(cy, 0, static_cast<int64_t>(h) - 1);
                return planes[p][static_cast<size_t>(cy) * w + static_cast<size_t>(cx)];
            };
            for (size_t p = 0; p < planes.size(); p++) {
                double v;
                if (interpolation == 0 || std::isnan(x) || std::isnan(y)) {
                    v = std::isnan(x) || std::isnan(y) ? 0 : cell(p, rounded(x), rounded(y));
                } else if (interpolation == 1) {
                    const double fx = std::floor(x), fy = std::floor(y), u = x - fx, t = y - fy;
                    const int64_t ix = static_cast<int64_t>(fx), iy = static_cast<int64_t>(fy);
                    v = (cell(p, ix, iy) * (1 - u) + cell(p, ix + 1, iy) * u) * (1 - t) + (cell(p, ix, iy + 1) * (1 - u) + cell(p, ix + 1, iy + 1) * u) * t;
                } else {
                    const double fx = std::floor(x), fy = std::floor(y);
                    double wx[4], wy[4];
                    cubicWeights(x - fx, wx);
                    cubicWeights(y - fy, wy);
                    const int64_t ix = static_cast<int64_t>(fx), iy = static_cast<int64_t>(fy);
                    v = 0;
                    for (int j = 0; j < 4; j++)
                        for (int i = 0; i < 4; i++) v += cell(p, ix - 1 + i, iy - 1 + j) * wx[i] * wy[j];
                }
                result[p][k] = v;
            }
        }
        writePlanes(vm, args[7], result, n, colour, true);
        return Value::null();
    });
    // window(cells, width, height, radius, reduce, disc, out, colour): each cell its window
    // reduced (0 min, 1 max, 2 median, 3 mean, 4 sum), over a square or a disc, cells outside the
    // grid left out; colours each part on its own.
    def("window", 8, 8, [](VM& vm, int, Value* args) -> Value {
        const size_t w = sizeArg(vm, args[1], "width"), h = sizeArg(vm, args[2], "height");
        const size_t radius = sizeArg(vm, args[3], "radius");
        const auto reduce = static_cast<Reduce>(vm.toInt(args[4], "reduce"));
        const bool disc = args[5].truthy(), colour = args[7].truthy();
        // A mean of colours weighs them by alpha, as a blur does; the rest work on each part as it is.
        const bool premultiply = colour && reduce == kMean;
        if (premultiply && !disc) {
            const uint8_t* from = colorCells(vm, args[0], w * h, "the grid");
            uint8_t* to = colorCells(vm, args[6], w * h, "the grid written into");
            if (from == to) {
                const std::vector<uint8_t> copy(from, from + w * h * 4);
                squareMeanColours(copy.data(), w, h, radius, to);
            } else {
                squareMeanColours(from, w, h, radius, to);
            }
            return Value::null();
        }
        auto planes = planesOf(vm, args[0], w * h, colour, premultiply);
        bool bytes = colour;
        if (!colour && listArg(vm, args[0], "the grid")->packed()) {
            const PackedItems& p = *asList(args[0])->pack;
            bytes = p.store == ListStore::Narrow && p.narrow == Narrow::Byte;
        }
        std::vector<std::vector<double>> result;
        for (const auto& plane : planes) result.push_back(reducePlane(plane, w, h, radius, reduce, disc, bytes));
        if (colour) writePlanes(vm, args[6], result, w * h, true, premultiply);
        else storeRounded(vm, listArg(vm, args[6], "the grid written into"), result[0]);
        return Value::null();
    });
    // distances(mask, width, height, out): each cell's distance to the nearest true cell of a
    // grid of bools, exact, into a grid of floats; infinity where none is true.
    def("distances", 4, 4, [](VM& vm, int, Value* args) -> Value {
        ObjList* mask = listArg(vm, args[0], "the mask");
        const size_t w = sizeArg(vm, args[1], "width"), h = sizeArg(vm, args[2], "height");
        if (!mask->packed() || mask->pack->store != ListStore::Bools || mask->size() != w * h) fail(vm, Thrown::IllegalArgumentException, "distances are of a grid of bools");
        const uint8_t* m = mask->pack->block.data();
        const double inf = std::numeric_limits<double>::infinity();
        std::vector<double> f(w * h), line(std::max(w, h)), done(std::max(w, h)), z;
        std::vector<size_t> v;
        for (size_t k = 0; k < w * h; k++) f[k] = m[k] != 0 ? 0 : inf;
        for (size_t x = 0; x < w; x++) {
            for (size_t y = 0; y < h; y++) line[y] = f[y * w + x];
            distanceLine(line.data(), h, done.data(), v, z);
            for (size_t y = 0; y < h; y++) f[y * w + x] = done[y];
        }
        for (size_t y = 0; y < h; y++) {
            distanceLine(&f[y * w], w, done.data(), v, z);
            for (size_t x = 0; x < w; x++) f[y * w + x] = std::sqrt(done[x]);
        }
        store(vm, listArg(vm, args[3], "the grid written into"), f);
        return Value::null();
    });
    // regions(cells, width, height, tolerance, diagonal, out, colour): every connected area of
    // cells numbered from 0, in the order their first cells come, two neighbours in one area
    // where they differ by no more than `tolerance`; how many there are.
    def("regions", 7, 7, [](VM& vm, int, Value* args) -> Value {
        const size_t w = sizeArg(vm, args[1], "width"), h = sizeArg(vm, args[2], "height");
        const double tolerance = vm.toNumber(args[3], "tolerance");
        const bool diagonal = args[4].truthy(), colour = args[6].truthy();
        Numbers n;
        const uint8_t* c = nullptr;
        if (colour) c = colorCells(vm, args[0], w * h, "the grid");
        else n = numbers(vm, args[0], "the grid");
        std::vector<size_t> parent(w * h);
        std::iota(parent.begin(), parent.end(), size_t{0});
        auto root = [&](size_t a) {
            while (parent[a] != a) a = parent[a] = parent[parent[a]];
            return a;
        };
        auto join = [&](size_t a, size_t b) {
            if (difference(n, c, a, b) > tolerance) return;
            a = root(a), b = root(b);
            if (a != b) parent[std::max(a, b)] = std::min(a, b);
        };
        for (size_t y = 0; y < h; y++)
            for (size_t x = 0; x < w; x++) {
                const size_t k = y * w + x;
                if (x + 1 < w) join(k, k + 1);
                if (y + 1 < h) join(k, k + w);
                if (diagonal && y + 1 < h) {
                    if (x + 1 < w) join(k, k + w + 1);
                    if (x > 0) join(k, k + w - 1);
                }
            }
        std::vector<int64_t> number(w * h), label(w * h, -1);
        int64_t count = 0;
        for (size_t k = 0; k < w * h; k++) {
            const size_t r = root(k);
            if (label[r] < 0) label[r] = count++;
            number[k] = label[r];
        }
        store(vm, listArg(vm, args[5], "the grid written into"), number);
        return Value::integer(count);
    });
    // flood(cells, width, height, x, y, tolerance, diagonal, out, colour): the cells joined to
    // (x, y) through cells that differ from it by no more than `tolerance`, true in a grid of
    // bools; how many.
    def("flood", 9, 9, [](VM& vm, int, Value* args) -> Value {
        const size_t w = sizeArg(vm, args[1], "width"), h = sizeArg(vm, args[2], "height");
        const int64_t sx = vm.toInt(args[3], "x"), sy = vm.toInt(args[4], "y");
        if (sx < 0 || sy < 0 || static_cast<size_t>(sx) >= w || static_cast<size_t>(sy) >= h)
            fail(vm, Thrown::IndexOutOfBoundsException, "(" + std::to_string(sx) + ", " + std::to_string(sy) + ") is outside the grid, which is " + std::to_string(w) + " by " + std::to_string(h));
        const double tolerance = vm.toNumber(args[5], "tolerance");
        const bool diagonal = args[6].truthy(), colour = args[8].truthy();
        Numbers n;
        const uint8_t* c = nullptr;
        if (colour) c = colorCells(vm, args[0], w * h, "the grid");
        else n = numbers(vm, args[0], "the grid");
        ObjList* out = listArg(vm, args[7], "the mask written into");
        if (!out->packed() || out->pack->store != ListStore::Bools || out->size() != w * h) fail(vm, Thrown::IllegalArgumentException, "the mask is a grid of bools the grid's size");
        uint8_t* m = out->pack->block.data();
        std::fill(m, m + w * h, 0);
        const size_t seed = static_cast<size_t>(sy) * w + static_cast<size_t>(sx);
        std::vector<size_t> todo{seed};
        m[seed] = 1;
        int64_t count = 1;
        while (!todo.empty()) {
            const size_t k = todo.back();
            todo.pop_back();
            const size_t x = k % w, y = k / w;
            for (int dy = -1; dy <= 1; dy++)
                for (int dx = -1; dx <= 1; dx++) {
                    if ((dx == 0 && dy == 0) || (!diagonal && dx != 0 && dy != 0)) continue;
                    const int64_t nx = static_cast<int64_t>(x) + dx, ny = static_cast<int64_t>(y) + dy;
                    if (nx < 0 || ny < 0 || nx >= static_cast<int64_t>(w) || ny >= static_cast<int64_t>(h)) continue;
                    const size_t j = static_cast<size_t>(ny) * w + static_cast<size_t>(nx);
                    if (m[j] != 0 || difference(n, c, seed, j) > tolerance) continue;
                    m[j] = 1;
                    count++;
                    todo.push_back(j);
                }
        }
        return Value::integer(count);
    });

    // trues(mask, width): how many cells of a grid of bools are true, and the box round them:
    // [count, left, top, right, bottom], right and bottom just past; the box empty for none.
    def("trues", 2, 2, [](VM& vm, int, Value* args) -> Value {
        ObjList* mask = listArg(vm, args[0], "the mask");
        const size_t w = sizeArg(vm, args[1], "width");
        if (!mask->packed() || mask->pack->store != ListStore::Bools) fail(vm, Thrown::IllegalArgumentException, "a mask is a grid of bools");
        const uint8_t* m = mask->pack->block.data();
        int64_t count = 0, l = INT64_MAX, t = INT64_MAX, r = -1, b = -1;
        for (size_t k = 0; k < mask->size(); k++) {
            if (m[k] == 0) continue;
            count++;
            const int64_t x = static_cast<int64_t>(k % std::max<size_t>(w, 1)), y = static_cast<int64_t>(k / std::max<size_t>(w, 1));
            l = std::min(l, x), t = std::min(t, y), r = std::max(r, x), b = std::max(b, y);
        }
        std::vector<int64_t> out = count == 0 ? std::vector<int64_t>{0, 0, 0, 0, 0} : std::vector<int64_t>{count, l, t, r + 1, b + 1};
        ObjList* made = newNumbers(vm, false, 5);
        TempRoot keep(vm, Value::object(made));
        store(vm, made, out);
        return Value::object(made);
    });
    // places(width, height, a, b, c, d, e, f, xs, ys): for each cell of a grid `width` by
    // `height`, where its middle goes by x' = a x + c y + e, y' = b x + d y + f, written into two
    // lists of floats: the places an affine transform reads a picture from (Image.transformed).
    def("places", 10, 10, [](VM& vm, int, Value* args) -> Value {
        const size_t w = sizeArg(vm, args[0], "width"), h = sizeArg(vm, args[1], "height");
        double m[6];
        for (int i = 0; i < 6; i++) m[i] = vm.toNumber(args[2 + i], "a transform's number");
        std::vector<double> xs(w * h), ys(w * h);
        for (size_t y = 0; y < h; y++)
            for (size_t x = 0; x < w; x++) {
                const double px = static_cast<double>(x), py = static_cast<double>(y);
                xs[y * w + x] = m[0] * px + m[2] * py + m[4];
                ys[y * w + x] = m[1] * px + m[3] * py + m[5];
            }
        store(vm, listArg(vm, args[8], "xs"), xs);
        store(vm, listArg(vm, args[9], "ys"), ys);
        return Value::null();
    });

    // ---- What a canvas does to a layer's pixels (src/std/paint.ww), each pixel as the WinterWolf
    // that came before did it (2026-11-10, the user's choice of C++ for these) ----

    // paintCells(cells, width, height, table, cycle, linear, gx, gy, ox, oy, f0, f1, qx, qy, qq,
    // m00, m01, m02, m10, m11, m12): a layer drawn in solid black given a gradient's colours,
    // each pixel's alpha kept as a share. `table` is 256 colours; cycle 0 pad, 1 reflect, 2
    // repeat; m.. take a pixel's column and row to the gradient's units. Linear: t = (x - ox) *
    // gx + (y - oy) * gy. Radial: from the focus (f0, f1), q the focus less the centre, qq its
    // length squared less the radius squared.
    def("paintCells", 21, 21, [](VM& vm, int, Value* args) -> Value {
        const size_t w = sizeArg(vm, args[1], "width"), h = sizeArg(vm, args[2], "height");
        uint8_t* cells = colorCells(vm, args[0], w * h, "the layer");
        const uint8_t* table = colorCells(vm, args[3], 256, "the gradient's table");
        const int64_t cycle = vm.toInt(args[4], "cycle");
        const bool linear = args[5].truthy();
        double p[15];
        for (int k = 0; k < 15; k++) p[k] = vm.toNumber(args[6 + k], "a gradient's number");
        const double gx = p[0], gy = p[1], ox = p[2], oy = p[3], f0 = p[4], f1 = p[5], qx = p[6], qy = p[7], qq = p[8];
        const double m00 = p[9], m01 = p[10], m02 = p[11], m10 = p[12], m11 = p[13], m12 = p[14];
        auto index = [&](double u) -> int {
            if (cycle == 0) return u <= 0.0 ? 0 : u >= 1.0 ? 255 : static_cast<int>(rounded(u * 255.0));
            if (cycle == 2) u = u - std::floor(u);
            else {
                u = std::fmod(std::fabs(u), 2.0);
                if (u > 1.0) u = 2.0 - u;
            }
            if (!(u > 0.0)) return 0;
            if (u >= 1.0) return 255;
            return static_cast<int>(rounded(u * 255.0));
        };
        const double step = gx * m00 + gy * m10;
        for (size_t row = 0; row < h; row++) {
            double ux = m01 * static_cast<double>(row) + m02, uy = m11 * static_cast<double>(row) + m12;
            double t = (ux - ox) * gx + (uy - oy) * gy;
            uint8_t* c = cells + row * w * 4;
            for (size_t col = 0; col < w; col++, c += 4) {
                const int a = c[3];
                if (a != 0) {
                    double u = t;
                    if (!linear) {
                        const double dx = ux - f0, dy = uy - f1;
                        const double aa = dx * dx + dy * dy;
                        if (aa == 0.0) {
                            u = 0.0;
                        } else {
                            const double bb = 2.0 * (qx * dx + qy * dy);
                            const double root = (-bb + std::sqrt(std::max(0.0, bb * bb - 4.0 * aa * qq))) / (2.0 * aa);
                            u = root <= 0.0 ? 1.0 : 1.0 / root;
                        }
                    }
                    const uint8_t* from = table + index(u) * 4;
                    c[0] = from[0];
                    c[1] = from[1];
                    c[2] = from[2];
                    c[3] = a == 255 && from[3] == 255 ? 255 : static_cast<uint8_t>((a * from[3] + 127) / 255);
                }
                ux += m00;
                uy += m10;
                t += step;
            }
        }
        return Value::null();
    });

    // maskCells(cells, width, height, left, top, mask, maskWidth, maskHeight, maskLeft, maskTop):
    // a layer's alpha made a share of a mask's, the mask's pixel under each; the layer at (left,
    // top) and the mask at (maskLeft, maskTop) in the same pixels, outside the mask nothing.
    def("maskCells", 10, 10, [](VM& vm, int, Value* args) -> Value {
        const size_t w = sizeArg(vm, args[1], "width"), h = sizeArg(vm, args[2], "height");
        uint8_t* cells = colorCells(vm, args[0], w * h, "the layer");
        const int64_t left = vm.toInt(args[3], "left"), top = vm.toInt(args[4], "top");
        const size_t mw = sizeArg(vm, args[6], "the mask's width"), mh = sizeArg(vm, args[7], "the mask's height");
        const uint8_t* mask = colorCells(vm, args[5], mw * mh, "the mask");
        const int64_t maskLeft = vm.toInt(args[8], "maskLeft"), maskTop = vm.toInt(args[9], "maskTop");
        for (size_t row = 0; row < h; row++) {
            const int64_t my = top + static_cast<int64_t>(row) - maskTop;
            const bool rowIn = my >= 0 && my < static_cast<int64_t>(mh);
            uint8_t* c = cells + row * w * 4;
            for (size_t col = 0; col < w; col++, c += 4) {
                const int a = c[3];
                if (a == 0) continue;
                const int64_t mx = left + static_cast<int64_t>(col) - maskLeft;
                const int k = rowIn && mx >= 0 && mx < static_cast<int64_t>(mw) ? mask[(static_cast<size_t>(my) * mw + static_cast<size_t>(mx)) * 4 + 3] : 0;
                if (k != 255) c[3] = static_cast<uint8_t>((a * k + 127) / 255);
            }
        }
        return Value::null();
    });

    // resample(cells, width, height, picture, pictureWidth, pictureHeight, sx, sy, sw, sh, m00,
    // m01, m02, m10, m11, m12, interpolation, alpha): a part of a picture, (sx, sy) sw by sh of
    // its pixels, drawn into a layer: m.. take a layer pixel's column and row to where its middle
    // falls in the picture. Read by interpolation (0 nearest, 1 bilinear, 2 bicubic, Catmull-Rom),
    // weighed by alpha, the part's edge pixels repeated past it; its edges smoothed by how much
    // of each layer pixel they cover; see-through by `alpha`.
    def("resample", 18, 18, [](VM& vm, int, Value* args) -> Value {
        const size_t w = sizeArg(vm, args[1], "width"), h = sizeArg(vm, args[2], "height");
        uint8_t* cells = colorCells(vm, args[0], w * h, "the layer");
        const int64_t fw = static_cast<int64_t>(sizeArg(vm, args[4], "the picture's width")), fh = static_cast<int64_t>(sizeArg(vm, args[5], "the picture's height"));
        const uint8_t* src = colorCells(vm, args[3], static_cast<size_t>(fw * fh), "the picture");
        double p[10];
        for (int k = 0; k < 10; k++) p[k] = vm.toNumber(args[6 + k], "a place in the picture");
        const double sx = p[0], sy = p[1], sw = p[2], sh = p[3];
        const double m00 = p[4], m01 = p[5], m02 = p[6], m10 = p[7], m11 = p[8], m12 = p[9];
        const int64_t interpolation = vm.toInt(args[16], "interpolation");
        const double alpha = vm.toNumber(args[17], "alpha");
        const double reachX = std::max(1e-9, std::fabs(m00) + std::fabs(m01));
        const double reachY = std::max(1e-9, std::fabs(m10) + std::fabs(m11));
        const int64_t x0 = std::max<int64_t>(0, static_cast<int64_t>(std::floor(sx))), y0 = std::max<int64_t>(0, static_cast<int64_t>(std::floor(sy)));
        const int64_t x1 = std::min<int64_t>(fw, static_cast<int64_t>(std::ceil(sx + sw))) - 1;
        const int64_t y1 = std::min<int64_t>(fh, static_cast<int64_t>(std::ceil(sy + sh))) - 1;
        if (x1 < x0 || y1 < y0) return Value::null();
        double r = 0, g = 0, b = 0, a = 0;
        auto take = [&](int64_t px, int64_t py, double weight) {
            const uint8_t* c = src + (std::min(y1, std::max(y0, py)) * fw + std::min(x1, std::max(x0, px))) * 4;
            const double ca = c[3] * weight;
            r += c[0] * ca;
            g += c[1] * ca;
            b += c[2] * ca;
            a += ca;
        };
        auto cubic = [](double t, double into[4]) {
            const double t2 = t * t, t3 = t2 * t;
            into[0] = (-t3 + 2.0 * t2 - t) / 2.0;
            into[1] = (3.0 * t3 - 5.0 * t2 + 2.0) / 2.0;
            into[2] = (-3.0 * t3 + 4.0 * t2 + t) / 2.0;
            into[3] = (t3 - t2) / 2.0;
        };
        double wx[4], wy[4];
        for (size_t row = 0; row < h; row++) {
            double u = m01 * static_cast<double>(row) + m02, v = m11 * static_cast<double>(row) + m12;
            uint8_t* c = cells + row * w * 4;
            for (size_t col = 0; col < w; col++, c += 4, u += m00, v += m10) {
                const double coverX = std::min(1.0, std::max(0.0, std::min(u - sx, sx + sw - u) / reachX + 0.5));
                const double coverY = std::min(1.0, std::max(0.0, std::min(v - sy, sy + sh - v) / reachY + 0.5));
                const double cover = coverX * coverY;
                if (!(cover > 0.0)) continue;
                r = g = b = a = 0;
                if (interpolation == 0) {
                    take(static_cast<int64_t>(std::floor(u)), static_cast<int64_t>(std::floor(v)), 1.0);
                } else {
                    const double fx = u - 0.5, fy = v - 0.5;
                    const int64_t ix = static_cast<int64_t>(std::floor(fx)), iy = static_cast<int64_t>(std::floor(fy));
                    if (interpolation == 1) {
                        const double tx = fx - static_cast<double>(ix), ty = fy - static_cast<double>(iy);
                        take(ix, iy, (1.0 - tx) * (1.0 - ty));
                        take(ix + 1, iy, tx * (1.0 - ty));
                        take(ix, iy + 1, (1.0 - tx) * ty);
                        take(ix + 1, iy + 1, tx * ty);
                    } else {
                        cubic(fx - static_cast<double>(ix), wx);
                        cubic(fy - static_cast<double>(iy), wy);
                        for (int m = 0; m < 4; m++)
                            for (int n = 0; n < 4; n++) take(ix - 1 + n, iy - 1 + m, wx[n] * wy[m]);
                    }
                }
                if (a > 0.0) {
                    const double out = std::min(255.0, a * cover * alpha);
                    auto part = [&](double q) { return static_cast<uint8_t>(std::min<int64_t>(255, std::max<int64_t>(0, rounded(q / a)))); };
                    c[0] = part(r);
                    c[1] = part(g);
                    c[2] = part(b);
                    c[3] = static_cast<uint8_t>(std::min<int64_t>(255, std::max<int64_t>(0, rounded(out))));
                }
            }
        }
        return Value::null();
    });

    vm.defineGlobal("__num", Value::object(lib));
}

}  // namespace ww
