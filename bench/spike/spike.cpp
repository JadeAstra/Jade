// M9.5's spike (docs/native-plan.html, step 0): the machine code each level of native code would
// run for three loops, written by hand in C++ and timed beside the VM (the same loops in Jade,
// spike.jade) and plain C++. Nothing in Jade uses this; it says whether the plan's targets hold.
//
// The loops are the VM's run code for them (JADE_DUMP_RUN), instruction for instruction:
//   mandelbrot   bench/numeric.jade's: typed float and int arithmetic, a while loop.
//   bytes        dst[i] = ((src[i] * 3 + 7) & 255).toByte() over 8 million bytes.
//   gradient     bench/kernels' K4: four bytes a pixel over 3840 by 2160.
//
// The levels:
//   cp        copy and patch: each instruction a piece of code on its own, its values read from and
//             written to the frame's 16-byte slots (kind and payload), with every check the VM's
//             handler makes on its quick way; a compiler barrier between instructions so that
//             nothing is kept or combined across them, as with stencils copied end to end.
//   reg       values in registers: the typed ints, floats and bools in registers from one
//             instruction to the next (no kinds written), objects still read from their slots,
//             every check kept; still a barrier between instructions, which nothing is combined
//             across.
//   loop      a loop template: the checks on the lists asked once before the loop, the rest of
//             the loop compiled as a whole, an int's overflow and a byte's range still checked.
//   ahead     a loop made ahead (bytes only): the commonest body made for its kinds, which a byte
//             source lets the compiler see cannot overflow; vector instructions.
//   c++       the same work in plain C++, no checks.
//
// Built with the same Clang as jade.exe: build.bat. Run pinned: bench\pinned.ps1 is for Jade;
// spike.ps1 runs this and spike.jade on the fast cores.

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace {

enum Kind : uint8_t { KNull = 0, KBool, KInt, KFloat, KByte, KObj };
struct V {
    uint8_t kind;
    uint8_t pad[7];
    union {
        int64_t i;
        double d;
        void* p;
        bool b;
    };
};
static_assert(sizeof(V) == 16, "a slot is 16 bytes");

enum ObjType : uint8_t { TList = 1, TBytes = 2 };
struct Bytes {
    ObjType type = TBytes;
    uint32_t core = 0;
    std::vector<uint8_t> data;
};

volatile int attention = 0;   // what LOOP looks at: a collection or another thread waiting
Bytes* volatile band = nullptr;  // a parallel band's object, which a read must not reach past
volatile uint32_t coreId = 0;

[[noreturn]] __attribute__((noinline, cold)) void slow(int at) {
    std::printf("left for the VM at %d\n", at);
    std::exit(1);
}

#define STEP asm volatile("" ::: "memory")
// A slot written as the VM writes it: kind and payload in one 16-byte store (setInt, setFloat).
inline void put(V& slot, Kind kind, uint64_t bits) {
    V made;
    std::memset(&made, 0, sizeof made);
    made.kind = kind;
    std::memcpy(&made.i, &bits, 8);
    std::memcpy(&slot, &made, sizeof made);
}
inline uint64_t bitsOf(double d) {
    uint64_t b;
    std::memcpy(&b, &d, 8);
    return b;
}
#define SETI(n, v) put(base[n], KInt, static_cast<uint64_t>(v))
#define SETD(n, v) put(base[n], KFloat, bitsOf(v))
#define SETB(n, v) put(base[n], KBool, (v) ? 1 : 0)
#define LOOPCHECK(at) \
    if (attention) slow(at)

// An untyped multiply, as R_MUL_SS's quick way: ints, floats, or an int and a float.
inline bool mulAny(V& out, const V& x, const V& y) {
    if (x.kind == KFloat && y.kind == KFloat) {
        out.kind = KFloat, out.d = x.d * y.d;
        return true;
    }
    if (x.kind == KInt && y.kind == KFloat) {
        out.kind = KFloat, out.d = static_cast<double>(x.i) * y.d;
        return true;
    }
    if (x.kind == KFloat && y.kind == KInt) {
        out.kind = KFloat, out.d = x.d * static_cast<double>(y.i);
        return true;
    }
    if (x.kind == KInt && y.kind == KInt) {
        int64_t z;
        if (__builtin_mul_overflow(x.i, y.i, &z)) return false;
        out.kind = KInt, out.i = z;
        return true;
    }
    return false;
}
inline bool addAny(V& out, const V& x, const V& y, bool sub) {
    if (x.kind == KFloat && y.kind == KFloat) {
        out.kind = KFloat, out.d = sub ? x.d - y.d : x.d + y.d;
        return true;
    }
    if (x.kind == KInt && y.kind == KFloat) {
        out.kind = KFloat, out.d = sub ? static_cast<double>(x.i) - y.d : static_cast<double>(x.i) + y.d;
        return true;
    }
    if (x.kind == KFloat && y.kind == KInt) {
        out.kind = KFloat, out.d = sub ? x.d - static_cast<double>(y.i) : x.d + static_cast<double>(y.i);
        return true;
    }
    if (x.kind == KInt && y.kind == KInt) {
        int64_t z;
        if (sub ? __builtin_sub_overflow(x.i, y.i, &z) : __builtin_add_overflow(x.i, y.i, &z)) return false;
        out.kind = KInt, out.i = z;
        return true;
    }
    return false;
}
// A byte of a Bytes, as R_GET_INDEX_SS reads it.
inline bool getByte(V& out, const V& obj, const V& index) {
    if (obj.kind != KObj || static_cast<Bytes*>(obj.p)->type != TBytes || index.kind != KInt) return false;
    Bytes* b = static_cast<Bytes*>(obj.p);
    const uint64_t i = static_cast<uint64_t>(index.i);
    if (i >= b->data.size() || b == band || (b->core != coreId && b->core != 0)) return false;
    out.kind = KByte, out.i = b->data[i];
    return true;
}
// SET_INDEX's quick way for a Bytes: obj, index, value from the stack.
inline bool setByte(const V& obj, const V& index, const V& value) {
    if (obj.kind != KObj || static_cast<Bytes*>(obj.p)->type != TBytes || index.kind != KInt) return false;
    Bytes* b = static_cast<Bytes*>(obj.p);
    if (b->core != coreId || !(value.kind == KByte || value.kind == KInt) || static_cast<uint64_t>(value.i) > 255 ||
        static_cast<uint64_t>(index.i) >= b->data.size())
        return false;
    b->data[static_cast<size_t>(index.i)] = static_cast<uint8_t>(value.i);
    return true;
}
// NUMERIC toByte on the top: an int from 0 to 255.
inline bool toByte(V& v) {
    if (v.kind == KByte) return true;
    if (v.kind != KInt || v.i < 0 || v.i > 255) return false;
    v.kind = KByte;
    return true;
}
// NUMERIC toInt on the top: a float made an int.
inline bool toInt(V& v) {
    if (v.kind == KInt) return true;
    if (v.kind != KFloat || !(v.d >= -9.2233720368547758e18 && v.d < 9.2233720368547758e18)) return false;
    v.kind = KInt, v.i = static_cast<int64_t>(v.d);
    return true;
}

// ---- mandelbrot ----

// Slots: 1 size, 2 limit, 3 inside, 4 py counter, 6 py, 8 ci, 9 px counter, 11 px, 13 cr,
// 14 zr, 15 zi, 16 n, 17 18 temporaries.
__attribute__((noinline)) int64_t mandelCp(V* base, int64_t size, int64_t limit) {
    SETI(1, size), SETI(2, limit), SETI(3, 0), SETI(4, 0);
    STEP;
    for (;;) {
        if (base[4].i >= base[1].i) break;  // RANGE_NEXT
        SETI(6, base[4].i);
        base[4].i += 1;
        STEP;
        {   // R_MUL_KS, R_DIV_SS, then R_SUB_F_SK: ci = 2.0 * py / size - 1.0 (once a row)
            const double q = (base[6].i * 2.0) / static_cast<double>(base[1].i);
            SETD(8, q);
            STEP;
            SETD(8, base[8].d - 1.0);
            STEP;
        }
        SETI(9, 0);
        STEP;
        for (;;) {
            if (base[9].i >= base[1].i) break;  // RANGE_NEXT
            SETI(11, base[9].i);
            base[9].i += 1;
            STEP;
            SETD(13, (base[11].i * 2.0) / static_cast<double>(base[1].i));  // R_MUL_KS, R_DIV_SS
            STEP;
            SETD(13, base[13].d - 1.5);  // R_SUB_F_SK
            STEP;
            SETD(14, 0.0), SETD(15, 0.0), SETI(16, 0);  // the three CONSTs
            STEP;
            for (;;) {
                SETB(17, base[16].i < base[2].i);  // R_LT_I_SS
                STEP;
                if (!(base[17].kind == KBool && base[17].i)) break;  // AND (false: out)
                STEP;
                SETD(17, base[14].d * base[14].d);  // R_MUL_F_SS
                STEP;
                SETD(18, base[15].d * base[15].d);
                STEP;
                SETD(17, base[17].d + base[18].d);  // R_ADD_F_SS
                STEP;
                SETB(17, base[17].d <= 4.0);  // R_LE_F_SK
                STEP;
                if (!base[17].i) break;  // R_JUMP_IF_FALSE
                STEP;
                SETD(17, base[14].d * base[14].d);
                STEP;
                SETD(18, base[15].d * base[15].d);
                STEP;
                SETD(17, base[17].d - base[18].d);
                STEP;
                SETD(17, base[17].d + base[13].d);
                STEP;
                SETD(18, 2.0 * base[14].d);
                STEP;
                SETD(18, base[18].d * base[15].d);
                STEP;
                SETD(15, base[18].d + base[8].d);
                STEP;
                base[14] = base[17];  // MOVE_S
                STEP;
                {
                    int64_t z;  // R_ADD_I_SK
                    if (__builtin_add_overflow(base[16].i, int64_t{1}, &z)) slow(3);
                    SETI(16, z);
                }
                STEP;
                LOOPCHECK(4);  // POP, LOOP
                STEP;
            }
            if (base[16].i == base[2].i) {  // R_JUMP_EQ_I_SS, R_ADD_I_SK
                int64_t z;
                if (__builtin_add_overflow(base[3].i, int64_t{1}, &z)) slow(5);
                SETI(3, z);
            }
            STEP;
            LOOPCHECK(6);
            STEP;
        }
        LOOPCHECK(7);
        STEP;
    }
    return base[3].i;
}

// The typed values in registers; each instruction still on its own: an empty asm that "changes"
// every live value stands between instructions, so the compiler keeps each in its register and
// combines nothing across (zr * zr is worked out twice, as two stencils would).
#define KEEP(...) asm volatile("" : __VA_ARGS__)
__attribute__((noinline)) int64_t mandelReg(V* base, int64_t size, int64_t limit) {
    int64_t inside = 0;
    for (int64_t py = 0; py < size; py++) {
        const double ci = (py * 2.0) / static_cast<double>(size) - 1.0;
        for (int64_t px = 0; px < size; px++) {
            const double cr = (px * 2.0) / static_cast<double>(size) - 1.5;
            double zr = 0.0, zi = 0.0, t1, t2;
            int64_t n = 0;
            bool c;
            for (;;) {
                c = n < limit;
                KEEP("+r"(c), "+r"(n), "+x"(zr), "+x"(zi));
                if (!c) break;
                t1 = zr * zr;
                KEEP("+x"(t1), "+x"(zr), "+x"(zi));
                t2 = zi * zi;
                KEEP("+x"(t1), "+x"(t2), "+x"(zr), "+x"(zi));
                t1 = t1 + t2;
                KEEP("+x"(t1), "+x"(zr), "+x"(zi));
                c = t1 <= 4.0;
                KEEP("+r"(c), "+x"(zr), "+x"(zi));
                if (!c) break;
                t1 = zr * zr;
                KEEP("+x"(t1), "+x"(zr), "+x"(zi));
                t2 = zi * zi;
                KEEP("+x"(t1), "+x"(t2), "+x"(zr), "+x"(zi));
                t1 = t1 - t2;
                KEEP("+x"(t1), "+x"(zr), "+x"(zi));
                t1 = t1 + cr;
                KEEP("+x"(t1), "+x"(zr), "+x"(zi));
                t2 = 2.0 * zr;
                KEEP("+x"(t1), "+x"(t2), "+x"(zr), "+x"(zi));
                t2 = t2 * zi;
                KEEP("+x"(t1), "+x"(t2), "+x"(zr), "+x"(zi));
                zi = t2 + ci;
                KEEP("+x"(t1), "+x"(zr), "+x"(zi));
                zr = t1;
                KEEP("+x"(zr), "+x"(zi));
                if (__builtin_add_overflow(n, int64_t{1}, &n)) slow(3);
                KEEP("+r"(n), "+x"(zr), "+x"(zi));
                LOOPCHECK(4);
            }
            if (n == limit && __builtin_add_overflow(inside, int64_t{1}, &inside)) slow(5);
            LOOPCHECK(6);
        }
        LOOPCHECK(7);
    }
    (void)base;
    return inside;
}

// Compiled as a whole, with the checks the language keeps (an int's overflow) and the loop's look
// at `attention`: what an optimizing level, or M14's C++, would make.
__attribute__((noinline)) int64_t mandelWhole(int64_t size, int64_t limit) {
    int64_t inside = 0;
    for (int64_t py = 0; py < size; py++) {
        const double ci = (py * 2.0) / static_cast<double>(size) - 1.0;
        for (int64_t px = 0; px < size; px++) {
            const double cr = (px * 2.0) / static_cast<double>(size) - 1.5;
            double zr = 0.0, zi = 0.0;
            int64_t n = 0;
            while (n < limit && zr * zr + zi * zi <= 4.0) {
                const double t = zr * zr - zi * zi + cr;
                zi = 2.0 * zr * zi + ci;
                zr = t;
                if (__builtin_add_overflow(n, int64_t{1}, &n)) slow(3);
                LOOPCHECK(4);
            }
            if (n == limit && __builtin_add_overflow(inside, int64_t{1}, &inside)) slow(5);
        }
    }
    return inside;
}

__attribute__((noinline)) int64_t mandelPlain(int64_t size, int64_t limit) {
    int64_t inside = 0;
    for (int64_t py = 0; py < size; py++) {
        const double ci = (py * 2.0) / static_cast<double>(size) - 1.0;
        for (int64_t px = 0; px < size; px++) {
            const double cr = (px * 2.0) / static_cast<double>(size) - 1.5;
            double zr = 0.0, zi = 0.0;
            int64_t n = 0;
            while (n < limit && zr * zr + zi * zi <= 4.0) {
                const double t = zr * zr - zi * zi + cr;
                zi = 2.0 * zr * zi + ci;
                zr = t;
                n++;
            }
            if (n == limit) inside++;
        }
        asm volatile("" ::: "memory");  // a side effect: the call stays between the clock's reads
    }
    return inside;
}

// ---- dst[i] = ((src[i] * 3 + 7) & 255).toByte() ----

// Slots: 1 src, 2 dst, 3 n, 4 counter, 7 i, 8 9 10 the stack (dst, i, the value).
__attribute__((noinline)) void bytesCp(V* base, Bytes* src, Bytes* dst, int64_t n) {
    base[1].kind = KObj, base[1].p = src;
    base[2].kind = KObj, base[2].p = dst;
    SETI(3, n), SETI(4, 0);
    STEP;
    for (;;) {
        if (base[4].i >= base[3].i) break;  // RANGE_NEXT
        SETI(7, base[4].i);
        base[4].i += 1;
        STEP;
        base[8] = base[2];  // GET_LOCAL dst
        STEP;
        base[9] = base[7];  // GET_LOCAL i
        STEP;
        if (!getByte(base[10], base[1], base[7])) slow(1);  // R_GET_INDEX_SS
        STEP;
        {
            int64_t z;  // R_MUL_I_SK
            if (__builtin_mul_overflow(base[10].i, int64_t{3}, &z)) slow(2);
            SETI(10, z);
        }
        STEP;
        {
            int64_t z;  // R_ADD_I_SK
            if (__builtin_add_overflow(base[10].i, int64_t{7}, &z)) slow(3);
            SETI(10, z);
        }
        STEP;
        SETI(10, base[10].i & 255);  // R_BIT_AND_SK
        STEP;
        if (!toByte(base[10])) slow(4);  // NUMERIC toByte
        STEP;
        if (!setByte(base[8], base[9], base[10])) slow(5);  // SET_INDEX
        base[8] = base[10];
        STEP;
        LOOPCHECK(6);  // POP, POP, LOOP
        STEP;
    }
}

__attribute__((noinline)) void bytesReg(V* base, Bytes* src, Bytes* dst, int64_t n) {
    base[1].kind = KObj, base[1].p = src;
    base[2].kind = KObj, base[2].p = dst;
    STEP;
    for (int64_t i = 0; i < n; i++) {
        KEEP("+r"(i));
        V index;
        index.kind = KInt, index.i = i;
        V got;
        if (!getByte(got, base[1], index)) slow(1);
        int64_t x = got.i;
        KEEP("+r"(x), "+r"(i));
        if (__builtin_mul_overflow(x, int64_t{3}, &x)) slow(2);
        KEEP("+r"(x), "+r"(i));
        if (__builtin_add_overflow(x, int64_t{7}, &x)) slow(3);
        KEEP("+r"(x), "+r"(i));
        x &= 255;
        KEEP("+r"(x), "+r"(i));
        if (x < 0 || x > 255) slow(4);
        KEEP("+r"(x), "+r"(i));
        V value;
        value.kind = KByte, value.i = x;
        if (!setByte(base[2], index, value)) slow(5);
        STEP;
        LOOPCHECK(6);
    }
}

// A loop template: the two Bytes asked once (their kind, the core, the band, the range of i
// against their sizes), then each item's work with its own checks (overflow, a byte's range).
__attribute__((noinline)) void bytesLoop(Bytes* src, Bytes* dst, int64_t n) {
    if (src->type != TBytes || dst->type != TBytes || src == band || dst == band || (src->core != coreId && src->core != 0) ||
        dst->core != coreId || n > static_cast<int64_t>(src->data.size()) || n > static_cast<int64_t>(dst->data.size()))
        slow(0);  // the loop as stencils instead
    const uint8_t* s = src->data.data();
    uint8_t* d = dst->data.data();
    for (int64_t i = 0; i < n; i++) {
        int64_t x;
        if (__builtin_mul_overflow(static_cast<int64_t>(s[i]), int64_t{3}, &x) || __builtin_add_overflow(x, int64_t{7}, &x)) slow(2);
        x &= 255;
        if (x < 0 || x > 255) slow(4);
        d[i] = static_cast<uint8_t>(x);
        if ((i & 0xFFFF) == 0) LOOPCHECK(6);
    }
}

// A loop made ahead for "a byte times a constant plus a constant, masked, to a byte": its
// constants known when it is copied, a byte source cannot overflow, so no check is left; vectors.
__attribute__((noinline)) void bytesAhead(Bytes* src, Bytes* dst, int64_t n, int64_t k, int64_t c) {
    if (src->type != TBytes || dst->type != TBytes || n > static_cast<int64_t>(src->data.size()) || n > static_cast<int64_t>(dst->data.size())) slow(0);
    const uint8_t* s = src->data.data();
    uint8_t* d = dst->data.data();
    for (int64_t i = 0; i < n; i++) d[i] = static_cast<uint8_t>((s[i] * k + c) & 255);
}

__attribute__((noinline)) void bytesPlain(const uint8_t* s, uint8_t* d, int64_t n) {
    for (int64_t i = 0; i < n; i++) d[i] = static_cast<uint8_t>((s[i] * 3 + 7) & 255);
}

// ---- gradient ----

// Slots: 1 px, 2 w, 3 h, 4 dx, 5 dy, 6 y counter, 9 y, 10 at, 11 x counter, 14 x, 15 t, then
// the stack 16 to 20.
__attribute__((noinline)) void gradientCp(V* base, Bytes* px, int64_t w, int64_t h) {
    base[1].kind = KObj, base[1].p = px;
    SETI(2, w), SETI(3, h);
    SETD(4, 0.6 / static_cast<double>(w)), SETD(5, 0.4 / static_cast<double>(h));
    SETI(6, 0);
    STEP;
    for (;;) {
        if (base[6].i >= base[3].i) break;
        SETI(9, base[6].i);
        base[6].i += 1;
        STEP;
        SETI(10, base[9].i * base[2].i * 4);
        STEP;
        SETI(11, 0);
        STEP;
        for (;;) {
            if (base[11].i >= base[2].i) break;  // RANGE_NEXT
            SETI(14, base[11].i);
            base[11].i += 1;
            STEP;
            if (!mulAny(base[15], base[14], base[4])) slow(1);  // R_MUL_SS x * dx
            STEP;
            if (!mulAny(base[16], base[9], base[5])) slow(2);  // R_MUL_SS y * dy
            STEP;
            SETD(15, base[15].d + base[16].d);  // R_ADD_F_SS
            STEP;
            const double kmul[3] = {200.0, 120.0, 150.0};
            const int64_t kadd[3] = {40, 90, 200};
            for (int ch = 0; ch < 3; ch++) {
                base[16] = base[1];  // GET_LOCAL px
                STEP;
                if (ch == 0) base[17] = base[10];  // GET_LOCAL at
                else if (__builtin_add_overflow(base[10].i, int64_t{ch}, &base[17].i)) slow(3);  // R_ADD_I_SK
                base[17].kind = KInt;
                STEP;
                base[18].kind = KInt, base[18].i = kadd[ch];  // CONST
                STEP;
                V k;
                k.kind = KFloat, k.d = kmul[ch];
                if (!mulAny(base[19], base[15], k)) slow(4);  // R_MUL_SK
                STEP;
                if (!addAny(base[18], base[18], base[19], ch == 2)) slow(5);  // R_ADD_SS / R_SUB_SS
                STEP;
                if (!toInt(base[18])) slow(6);  // NUMERIC toInt
                STEP;
                if (!toByte(base[18])) slow(7);  // NUMERIC toByte
                STEP;
                if (!setByte(base[16], base[17], base[18])) slow(8);  // SET_INDEX
                base[16] = base[18];
                STEP;  // POP
            }
            base[16] = base[1];  // GET_LOCAL px
            STEP;
            if (__builtin_add_overflow(base[10].i, int64_t{3}, &base[17].i)) slow(9);
            base[17].kind = KInt;
            STEP;
            base[18].kind = KByte, base[18].i = 255;  // CONST, NUMERIC toByte
            STEP;
            if (!setByte(base[16], base[17], base[18])) slow(10);
            STEP;
            {
                int64_t z;  // at += 4
                if (__builtin_add_overflow(base[10].i, int64_t{4}, &z)) slow(11);
                SETI(10, z);
            }
            STEP;
            LOOPCHECK(12);
            STEP;
        }
        LOOPCHECK(13);
        STEP;
    }
}

__attribute__((noinline)) void gradientReg(V* base, Bytes* px, int64_t w, int64_t h) {
    base[1].kind = KObj, base[1].p = px;
    STEP;
    const double dx = 0.6 / static_cast<double>(w), dy = 0.4 / static_cast<double>(h);
    for (int64_t y = 0; y < h; y++) {
        int64_t at = y * w * 4;
        for (int64_t x = 0; x < w; x++) {
            double t = x * dx;
            KEEP("+x"(t), "+r"(at), "+r"(x));
            double t2 = y * dy;
            KEEP("+x"(t), "+x"(t2), "+r"(at), "+r"(x));
            t = t + t2;
            KEEP("+x"(t), "+r"(at), "+r"(x));
            const double kmul[3] = {200.0, 120.0, 150.0};
            const int64_t kadd[3] = {40, 90, 200};
            for (int ch = 0; ch < 3; ch++) {
                int64_t place;
                if (__builtin_add_overflow(at, int64_t{ch}, &place)) slow(3);
                KEEP("+r"(place), "+x"(t), "+r"(at));
                double v = t * kmul[ch];
                KEEP("+x"(v), "+x"(t), "+r"(place), "+r"(at));
                v = ch == 2 ? kadd[ch] - v : kadd[ch] + v;
                KEEP("+x"(v), "+x"(t), "+r"(place), "+r"(at));
                if (!(v >= -9.2233720368547758e18 && v < 9.2233720368547758e18)) slow(6);
                int64_t iv = static_cast<int64_t>(v);
                KEEP("+r"(iv), "+x"(t), "+r"(place), "+r"(at));
                if (iv < 0 || iv > 255) slow(7);
                V index, value;
                index.kind = KInt, index.i = place;
                value.kind = KByte, value.i = iv;
                if (!setByte(base[1], index, value)) slow(8);
                STEP;
            }
            V index, value;
            index.kind = KInt, index.i = at + 3;
            value.kind = KByte, value.i = 255;
            if (!setByte(base[1], index, value)) slow(10);
            STEP;
            if (__builtin_add_overflow(at, int64_t{4}, &at)) slow(11);
            LOOPCHECK(12);
        }
    }
}

// A loop template of rows and columns: the Bytes asked once (its kind, core, band, and that
// at + 3 stays in it for the last pixel), then each pixel's work with its checks.
__attribute__((noinline)) void gradientLoop(Bytes* px, int64_t w, int64_t h) {
    if (px->type != TBytes || px == band || px->core != coreId || h * w * 4 > static_cast<int64_t>(px->data.size())) slow(0);
    uint8_t* d = px->data.data();
    const double dx = 0.6 / static_cast<double>(w), dy = 0.4 / static_cast<double>(h);
    for (int64_t y = 0; y < h; y++) {
        int64_t at = y * w * 4;
        for (int64_t x = 0; x < w; x++) {
            const double t = x * dx + y * dy;
            const double v0 = 40 + t * 200, v1 = 90 + t * 120, v2 = 200 - t * 150;
            if (!(v0 >= 0 && v0 < 256 && v1 >= 0 && v1 < 256 && v2 >= 0 && v2 < 256)) slow(7);
            d[at] = static_cast<uint8_t>(static_cast<int64_t>(v0));
            d[at + 1] = static_cast<uint8_t>(static_cast<int64_t>(v1));
            d[at + 2] = static_cast<uint8_t>(static_cast<int64_t>(v2));
            d[at + 3] = 255;
            at += 4;
        }
        LOOPCHECK(13);
    }
}

__attribute__((noinline)) void gradientPlain(uint8_t* d, int64_t w, int64_t h) {
    const double dx = 0.6 / static_cast<double>(w), dy = 0.4 / static_cast<double>(h);
    for (int64_t y = 0; y < h; y++) {
        int64_t at = y * w * 4;
        for (int64_t x = 0; x < w; x++) {
            const double t = x * dx + y * dy;
            d[at] = static_cast<uint8_t>(static_cast<int64_t>(40 + t * 200));
            d[at + 1] = static_cast<uint8_t>(static_cast<int64_t>(90 + t * 120));
            d[at + 2] = static_cast<uint8_t>(static_cast<int64_t>(200 - t * 150));
            d[at + 3] = 255;
            at += 4;
        }
    }
}

template <class F>
double best(F&& f, int runs = 5) {
    double b = 1e30;
    for (int r = 0; r < runs; r++) {
        const auto s = std::chrono::steady_clock::now();
        f();
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - s).count();
        if (ms < b) b = ms;
    }
    return b;
}

}  // namespace

int main(int argc, char** argv) {
    const char* which = argc > 1 ? argv[1] : "all";
    std::vector<V> frame(64);
    V* base = frame.data();
    auto is = [&](const char* name) { return std::strcmp(which, "all") == 0 || std::strcmp(which, name) == 0; };
    if (is("mandelbrot")) {
        int64_t r1 = 0, r2 = 0, r3 = 0, r4 = 0;
        const double cp = best([&] { r1 = mandelCp(base, 400, 100); });
        const double reg = best([&] { r2 = mandelReg(base, 400, 100); });
        const double whole = best([&] { r3 = mandelWhole(400, 100); });
        volatile int64_t size = 400, limit = 100;  // not known when compiling, or the answer would be
        const double plain = best([&] { r4 = mandelPlain(size, limit); });
        std::printf("mandelbrot  cp %.1f  reg %.1f  whole %.1f  c++ %.1f ms  (%lld %lld %lld %lld)\n", cp, reg, whole, plain,
                    static_cast<long long>(r1), static_cast<long long>(r2), static_cast<long long>(r3), static_cast<long long>(r4));
    }
    if (is("bytes")) {
        const int64_t n = 8000000;
        Bytes src, dst;
        src.data.resize(n);
        dst.data.resize(n);
        for (int64_t i = 0; i < n; i++) src.data[static_cast<size_t>(i)] = static_cast<uint8_t>(i * 31 % 251);
        auto check = [&] { return static_cast<int>(dst.data[12345]); };
        const double cp = best([&] { bytesCp(base, &src, &dst, n); });
        const int c1 = check();
        const double reg = best([&] { bytesReg(base, &src, &dst, n); });
        const int c2 = check();
        const double loop = best([&] { bytesLoop(&src, &dst, n); });
        const int c3 = check();
        const double ahead = best([&] { bytesAhead(&src, &dst, n, 3, 7); });
        const int c4 = check();
        const double plain = best([&] { bytesPlain(src.data.data(), dst.data.data(), n); });
        std::printf("bytes       cp %.1f  reg %.1f  loop %.1f  ahead %.2f  c++ %.2f ms  (%d %d %d %d %d)\n", cp, reg, loop, ahead, plain, c1, c2, c3, c4,
                    check());
    }
    if (is("gradient")) {
        const int64_t w = 3840, h = 2160;
        Bytes px;
        px.data.resize(static_cast<size_t>(w * h * 4));
        auto check = [&] { return static_cast<int>(px.data[static_cast<size_t>(w * 4 * 100 + 400)]); };
        const double cp = best([&] { gradientCp(base, &px, w, h); });
        const int c1 = check();
        const double reg = best([&] { gradientReg(base, &px, w, h); });
        const int c2 = check();
        const double loop = best([&] { gradientLoop(&px, w, h); });
        const int c3 = check();
        const double plain = best([&] { gradientPlain(px.data.data(), w, h); });
        std::printf("gradient    cp %.1f  reg %.1f  loop %.1f  c++ %.1f ms  (%d %d %d %d)\n", cp, reg, loop, plain, c1, c2, c3, check());
    }
    return 0;
}
