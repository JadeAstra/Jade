// The register stencils of native code (M9.5, docs/native-plan.html): each a small function
// compiled by Clang when ww.exe is built, never called as itself, but read out of the object
// file by tools/stencils and copied, end to end, into a region's machine code as it runs.
//
// Every stencil has the same parameters: the frame's slots, the region's context (the VM's flag,
// a collection or another thread waiting; the core of a parallel section it runs on and the
// object lent to it), then the region's registers, ints i0 to i9 and floats f0 to f15, which
// `preserve_none` keeps in the processor's registers (all twelve of the processor's it gives for
// ints; for floats all eight XMM registers it gives, two floats to each since 2026-11-14: see
// f0 below). A stencil does one thing to them and goes on to the next with
// `musttail`, which Clang makes a plain jump; a register it does not touch is passed through as
// it is. i9 is the region's flag register: a comparison's result, which the jumps and `and`
// after it read (src/native/regions.cpp gives out i0 to i8).
//
// Its gaps are references to the names below, filled in when it is copied:
//   ww_next, ww_jump, ww_exit   where it goes on: the stencil after it, a jump's target,
//                                     the way out to the VM this step has
//   ww_kf, ww_ki                  a float or an int constant
//   ww_slot, ww_slot2             slots of the frame
//   ww_slotat, ww_slot2at         those slots' offsets in the frame, built into the instruction
//                                     itself (a 32-bit value, not a word after the code)
//   ww_ret                          what the region gives back: where the VM goes on
// Each is reached through a word placed after the region's code (a 32-bit offset from the
// instruction), so that no stencil needs an absolute address.
//
// A stencil's name says what it does and to which registers: addf_4_4_5 is f4 = f4 + f5 (and
// addf_20_4_5 the same, the register written whole). The
// jumps are named as the VM's typed jumps work, jumping where the comparison does NOT hold
// (jnlt: not less; with a NaN every float comparison fails, so they jump), and the comparisons
// put their answer in i9. A stencil that reads an object (a list's item, say) does what the VM
// does at once where it can, and leaves for the VM, which does the rest as it always does,
// where it cannot: what it reads of an object is in layout.h.

#include <cstdint>
#include <cstring>
#include <emmintrin.h>

#include "layout.h"

#define INLINE static inline __attribute__((always_inline))

namespace {
using namespace ww::native::layout;
struct Slot {  // a frame's slot, as the VM keeps it: a kind and an 8-byte payload
    uint8_t kind;
    uint8_t pad[7];
    union {
        int64_t i;
        double d;
        const uint8_t* p;
    };
};
static_assert(sizeof(Slot) == kValueSize, "a slot is a value");
struct Ctx {  // native.h's RegionContext
    const volatile uint8_t* attention;
    uint64_t core;
    const void* band;
    uint64_t lists[24];
    uint64_t t[256];
    const uint8_t* globals;
    uint64_t held[32];
    uint64_t heldAt[16];
    uint64_t homes[32];
    const uint8_t* const* upvalues;
};

// What the VM's fast ways ask of an object: a list or bytes it may read (owned by the core it
// runs on, or by none, and not the object lent to the section), or change (owned by its core).
INLINE const uint8_t* objectIn(const Slot& s, uint8_t type) {
    if (s.kind != kKindObj || s.p[kObjType] != type) return nullptr;
    return s.p;
}
INLINE bool mayRead(const uint8_t* o, const Ctx* ctx) {
    return o != ctx->band && (o[kObjCore] == ctx->core || o[kObjCore] == 0);
}
template <typename T>
INLINE T word(const uint8_t* at) {
    T v;
    __builtin_memcpy(&v, at, sizeof v);
    return v;
}
// A packed list's block of items of the kind `store`, none of them null, or null.
INLINE const uint8_t* packOf(const uint8_t* list, uint8_t store) {
    const uint8_t* pack = word<const uint8_t*>(list + kListPack);
    if (pack == nullptr || pack[kPackStore] != store || pack[kPackNullable] != 0) return nullptr;
    return pack;
}
// A float register written whole (f16 to f23): its value in the low half, the high one left as
// the instruction that made it leaves it.
struct Whole {
    __m128d& v;
    __attribute__((always_inline)) operator double() const { return v[0]; }
    __attribute__((always_inline)) Whole& operator=(double x) {
        const __m128d one = _mm_set_sd(x);
        v = __builtin_shufflevector(one, one, 0, -1);
        return *this;
    }
};
// The frame's slot ww_slot (ww_slot2): its offset built into the instruction (the gaps
// ww_slotat, ww_slot2at, filled with the slot's offset itself; 2026-11-14). Before, the slot's
// number was read from a word placed after the code and shifted before the slot could be: three
// instructions, two reads of memory one after the other, for what is now one (a load, a store)
// or one before the reads of it (an address).
INLINE Slot* slotAt(Slot* base) {
    Slot* s;
    __asm__("leaq ww_slotat(%1), %0" : "=r"(s) : "r"(base));
    return s;
}
INLINE Slot* slot2At(Slot* base) {
    Slot* s;
    __asm__("leaq ww_slot2at(%1), %0" : "=r"(s) : "r"(base));
    return s;
}
// Slot ww_slot (ww_slot2) written whole, in one store (see storeSlot), the store's place built in.
INLINE void storeSlotAt(Slot* base, uint8_t kind, uint64_t bits) {
    const __m128i v = _mm_set_epi64x(static_cast<int64_t>(bits), kind);
    __asm__ volatile("movdqu %1, ww_slotat(%0)" : : "r"(base), "x"(v) : "memory");
}
INLINE void storeSlot2At(Slot* base, uint8_t kind, uint64_t bits) {
    const __m128i v = _mm_set_epi64x(static_cast<int64_t>(bits), kind);
    __asm__ volatile("movdqu %1, ww_slot2at(%0)" : : "r"(base), "x"(v) : "memory");
}
INLINE void storeSlot(Slot* base, int64_t slot, uint8_t kind, uint64_t bits) {
    // One store of the whole slot (2026-11-13): what reads it whole after (the VM's copies of a
    // value) is given it at once, where four stores of its parts had the read wait for them all
    // to reach memory.
    _mm_storeu_si128(reinterpret_cast<__m128i*>(&base[slot]), _mm_set_epi64x(static_cast<int64_t>(bits), kind));
}
}  // namespace

#define PARAMS                                                                                                         \
    Slot *base, Ctx *ctx, int64_t i0, int64_t i1, int64_t i2, int64_t i3, int64_t i4, int64_t i5, int64_t i6,    \
        int64_t i7, int64_t i8, int64_t i9, __m128d v0, __m128d v1, __m128d v2, __m128d v3, __m128d v4, __m128d v5,  \
        __m128d v6, __m128d v7
#define ARGS base, ctx, i0, i1, i2, i3, i4, i5, i6, i7, i8, i9, v0, v1, v2, v3, v4, v5, v6, v7
// The sixteen float registers: two to each of the processor's eight XMM registers preserve_none
// passes, f0 to f7 their low halves, f8 to f15 their high ones. A write to one half keeps the
// other, and so waits for whatever last wrote the register; f16 to f23 are f0 to f7 written whole
// (the high half left as it falls), which regions.cpp gives a stencil where f8 to f15 holds
// nothing wanted after it.
#define f0 v0[0]
#define f1 v1[0]
#define f2 v2[0]
#define f3 v3[0]
#define f4 v4[0]
#define f5 v5[0]
#define f6 v6[0]
#define f7 v7[0]
#define f8 v0[1]
#define f9 v1[1]
#define f10 v2[1]
#define f11 v3[1]
#define f12 v4[1]
#define f13 v5[1]
#define f14 v6[1]
#define f15 v7[1]
#define f16 (Whole{v0})
#define f17 (Whole{v1})
#define f18 (Whole{v2})
#define f19 (Whole{v3})
#define f20 (Whole{v4})
#define f21 (Whole{v5})
#define f22 (Whole{v6})
#define f23 (Whole{v7})
// Each float register's XMM register, and an 8-byte load into it: movlps into a low half,
// movhps into a high one (the other half kept), movsd whole (f16 to f23).
#define FVEC_0 v0
#define FVEC_1 v1
#define FVEC_2 v2
#define FVEC_3 v3
#define FVEC_4 v4
#define FVEC_5 v5
#define FVEC_6 v6
#define FVEC_7 v7
#define FVEC_8 v0
#define FVEC_9 v1
#define FVEC_10 v2
#define FVEC_11 v3
#define FVEC_12 v4
#define FVEC_13 v5
#define FVEC_14 v6
#define FVEC_15 v7
#define FVEC_16 v0
#define FVEC_17 v1
#define FVEC_18 v2
#define FVEC_19 v3
#define FVEC_20 v4
#define FVEC_21 v5
#define FVEC_22 v6
#define FVEC_23 v7
#define FLOADOP_0 "movlps"
#define FLOADOP_1 "movlps"
#define FLOADOP_2 "movlps"
#define FLOADOP_3 "movlps"
#define FLOADOP_4 "movlps"
#define FLOADOP_5 "movlps"
#define FLOADOP_6 "movlps"
#define FLOADOP_7 "movlps"
#define FLOADOP_8 "movhps"
#define FLOADOP_9 "movhps"
#define FLOADOP_10 "movhps"
#define FLOADOP_11 "movhps"
#define FLOADOP_12 "movhps"
#define FLOADOP_13 "movhps"
#define FLOADOP_14 "movhps"
#define FLOADOP_15 "movhps"
#define FLOADOP_16 "movsd"
#define FLOADOP_17 "movsd"
#define FLOADOP_18 "movsd"
#define FLOADOP_19 "movsd"
#define FLOADOP_20 "movsd"
#define FLOADOP_21 "movsd"
#define FLOADOP_22 "movsd"
#define FLOADOP_23 "movsd"
#define WW_STENCIL extern "C" __attribute__((preserve_none)) int64_t

WW_STENCIL ww_next(PARAMS);
WW_STENCIL ww_jump(PARAMS);
WW_STENCIL ww_exit(PARAMS);
extern "C" const double ww_kf;
extern "C" const int64_t ww_ki;
extern "C" const int64_t ww_slot;
extern "C" const int64_t ww_slot2;
extern "C" const int64_t ww_ret;

#define STENCIL(name) WW_STENCIL st_##name(PARAMS)
#define NEXT [[clang::musttail]] return ww_next(ARGS)
#define JUMP [[clang::musttail]] return ww_jump(ARGS)
#define EXIT [[clang::musttail]] return ww_exit(ARGS)

// Each register of a kind, for the variants: three levels, as a macro may not use itself.
#define F_A(M, ...) M(0, __VA_ARGS__) M(1, __VA_ARGS__) M(2, __VA_ARGS__) M(3, __VA_ARGS__) M(4, __VA_ARGS__) M(5, __VA_ARGS__) M(6, __VA_ARGS__) M(7, __VA_ARGS__) M(8, __VA_ARGS__) M(9, __VA_ARGS__) M(10, __VA_ARGS__) M(11, __VA_ARGS__) M(12, __VA_ARGS__) M(13, __VA_ARGS__) M(14, __VA_ARGS__) M(15, __VA_ARGS__)
#define F_B(M, ...) M(0, __VA_ARGS__) M(1, __VA_ARGS__) M(2, __VA_ARGS__) M(3, __VA_ARGS__) M(4, __VA_ARGS__) M(5, __VA_ARGS__) M(6, __VA_ARGS__) M(7, __VA_ARGS__) M(8, __VA_ARGS__) M(9, __VA_ARGS__) M(10, __VA_ARGS__) M(11, __VA_ARGS__) M(12, __VA_ARGS__) M(13, __VA_ARGS__) M(14, __VA_ARGS__) M(15, __VA_ARGS__)
// The float registers a stencil writes: each of the sixteen, and f0 to f7 written whole.
#define F_W(M, ...) F_A(M, __VA_ARGS__) M(16, __VA_ARGS__) M(17, __VA_ARGS__) M(18, __VA_ARGS__) M(19, __VA_ARGS__) M(20, __VA_ARGS__) M(21, __VA_ARGS__) M(22, __VA_ARGS__) M(23, __VA_ARGS__)
#define F_C(M, ...) M(0, __VA_ARGS__) M(1, __VA_ARGS__) M(2, __VA_ARGS__) M(3, __VA_ARGS__) M(4, __VA_ARGS__) M(5, __VA_ARGS__) M(6, __VA_ARGS__) M(7, __VA_ARGS__) M(8, __VA_ARGS__) M(9, __VA_ARGS__) M(10, __VA_ARGS__) M(11, __VA_ARGS__) M(12, __VA_ARGS__) M(13, __VA_ARGS__) M(14, __VA_ARGS__) M(15, __VA_ARGS__)
#define I_A(M, ...) M(0, __VA_ARGS__) M(1, __VA_ARGS__) M(2, __VA_ARGS__) M(3, __VA_ARGS__) M(4, __VA_ARGS__) M(5, __VA_ARGS__) M(6, __VA_ARGS__) M(7, __VA_ARGS__) M(8, __VA_ARGS__)
#define I_B(M, ...) M(0, __VA_ARGS__) M(1, __VA_ARGS__) M(2, __VA_ARGS__) M(3, __VA_ARGS__) M(4, __VA_ARGS__) M(5, __VA_ARGS__) M(6, __VA_ARGS__) M(7, __VA_ARGS__) M(8, __VA_ARGS__)
#define I_C(M, ...) M(0, __VA_ARGS__) M(1, __VA_ARGS__) M(2, __VA_ARGS__) M(3, __VA_ARGS__) M(4, __VA_ARGS__) M(5, __VA_ARGS__) M(6, __VA_ARGS__) M(7, __VA_ARGS__) M(8, __VA_ARGS__)

// The range a float may have to be made an int, as the VM's toInt, floor, ceil and round ask.
#define FITS_INT(x) ((x) > -9.2e18 && (x) < 9.2e18)

// ---- Floats ----

// fa = fb op fc
#define FBIN_C(c, a, b, name, op) STENCIL(name##_##a##_##b##_##c) { f##a = f##b op f##c; NEXT; }
#define FBIN_B(b, a, name, op) F_C(FBIN_C, a, b, name, op)
#define FBIN_A(a, name, op) F_B(FBIN_B, a, name, op)
F_W(FBIN_A, addf, +)
F_W(FBIN_A, subf, -)
F_W(FBIN_A, mulf, *)
F_W(FBIN_A, divf, /)

// fa = fb op K, fa = K op fb
#define FBINK_B(b, a, name, op) STENCIL(name##k_##a##_##b) { f##a = f##b op ww_kf; NEXT; } \
                                STENCIL(name##kr_##a##_##b) { f##a = ww_kf op f##b; NEXT; }
#define FBINK_A(a, name, op) F_B(FBINK_B, a, name, op)
F_W(FBINK_A, addf, +)
F_W(FBINK_A, subf, -)
F_W(FBINK_A, mulf, *)
F_W(FBINK_A, divf, /)

// fa = fb; fa = K; fa = -fb, |fb|, the square root of fb; fa = the less or the more of fa and
// fb, fa kept unless fb is less (more), as the VM's Math.min and max keep the first.
#define FUN_B(b, a)                                                             \
    STENCIL(movf_##a##_##b) { f##a = f##b; NEXT; }                              \
    STENCIL(negf_##a##_##b) { f##a = -f##b; NEXT; }                             \
    STENCIL(absf_##a##_##b) { f##a = __builtin_fabs(f##b); NEXT; }              \
    STENCIL(sqrtf_##a##_##b) { f##a = _mm_cvtsd_f64(_mm_sqrt_sd(_mm_setzero_pd(), _mm_set_sd(f##b))); NEXT; } \
    STENCIL(minf_##a##_##b) { if (f##b < f##a) f##a = f##b; NEXT; }             \
    STENCIL(maxf_##a##_##b) { if (f##b > f##a) f##a = f##b; NEXT; }
#define FUN_A(a, ...) F_B(FUN_B, a)
F_W(FUN_A, x)
#define FSETK(a, ...) STENCIL(setfk_##a) { f##a = ww_kf; NEXT; }
F_W(FSETK, x)

// fa = ib, an int made a float; ia = fb made an int, its whole part, or its floor, ceiling or
// nearest (half away from 0), leaving for the VM where it does not fit (or is NaN).
// (Into a register cleared first: cvtsi2sd keeps the register's upper half, and so would wait
// for whatever last wrote it, a chain of the loop's float work perhaps. Written out, as the
// compiler drops a clearing it sees no use for.)
#define CVT_B(b, a)                                                             \
    STENCIL(cvtif_##a##_##b) {                                                  \
        double d_;                                                              \
        __asm__("xorps %0, %0\n\tcvtsi2sdq %1, %0" : "=&x"(d_) : "r"(i##b));    \
        f##a = d_;                                                              \
        NEXT;                                                                   \
    }
#define CVT_A(a, ...) I_B(CVT_B, a)
F_W(CVT_A, x)
#define TOINT_B(b, a)                                                           \
    STENCIL(cvtfi_##a##_##b) {                                                  \
        if (!FITS_INT(f##b)) EXIT;                                              \
        i##a = static_cast<int64_t>(f##b);                                      \
        NEXT;                                                                   \
    }                                                                           \
    STENCIL(floori_##a##_##b) {                                                 \
        if (!FITS_INT(f##b)) EXIT;                                              \
        const int64_t t = static_cast<int64_t>(f##b);                           \
        i##a = t - (static_cast<double>(t) > f##b);                             \
        NEXT;                                                                   \
    }                                                                           \
    STENCIL(ceili_##a##_##b) {                                                  \
        if (!FITS_INT(f##b)) EXIT;                                              \
        const int64_t t = static_cast<int64_t>(f##b);                           \
        i##a = t + (static_cast<double>(t) < f##b);                             \
        NEXT;                                                                   \
    }                                                                           \
    STENCIL(roundi_##a##_##b) {                                                 \
        if (!FITS_INT(f##b)) EXIT;                                              \
        const int64_t t = static_cast<int64_t>(f##b);                           \
        const double part = f##b - static_cast<double>(t);                      \
        i##a = t + (part >= 0.5) - (part <= -0.5);                              \
        NEXT;                                                                   \
    }
#define TOINT_A(a, ...) F_B(TOINT_B, a)
I_A(TOINT_A, x)

// Jumps on a float and a constant, where `fa op K` holds (as the build's check uses them).
#define FJUMPK(a, ...)                                                          \
    STENCIL(jgtfk_##a) { if (f##a > ww_kf) JUMP; NEXT; }                      \
    STENCIL(jlefk_##a) { if (f##a <= ww_kf) JUMP; NEXT; }
F_A(FJUMPK, x)

// The VM's typed jumps on floats: to the target where `fa op fb` (or `fa op K`) does not hold.
#define FJN_B(b, a)                                                             \
    STENCIL(jnltff_##a##_##b) { if (!(f##a < f##b)) JUMP; NEXT; }               \
    STENCIL(jnleff_##a##_##b) { if (!(f##a <= f##b)) JUMP; NEXT; }              \
    STENCIL(jngtff_##a##_##b) { if (!(f##a > f##b)) JUMP; NEXT; }               \
    STENCIL(jngeff_##a##_##b) { if (!(f##a >= f##b)) JUMP; NEXT; }              \
    STENCIL(jneqff_##a##_##b) { if (!(f##a == f##b)) JUMP; NEXT; }
#define FJN_A(a, ...) F_B(FJN_B, a)
F_A(FJN_A, x)
#define FJNK(a, ...)                                                            \
    STENCIL(jnltfk_##a) { if (!(f##a < ww_kf)) JUMP; NEXT; }                  \
    STENCIL(jnlefk_##a) { if (!(f##a <= ww_kf)) JUMP; NEXT; }                 \
    STENCIL(jngtfk_##a) { if (!(f##a > ww_kf)) JUMP; NEXT; }                  \
    STENCIL(jngefk_##a) { if (!(f##a >= ww_kf)) JUMP; NEXT; }                 \
    STENCIL(jneqfk_##a) { if (!(f##a == ww_kf)) JUMP; NEXT; }
F_A(FJNK, x)

// The VM's typed comparisons of floats, their answer in i9.
#define FCMP_B(b, a)                                                            \
    STENCIL(ltff_##a##_##b) { i9 = f##a < f##b; NEXT; }                         \
    STENCIL(leff_##a##_##b) { i9 = f##a <= f##b; NEXT; }                        \
    STENCIL(gtff_##a##_##b) { i9 = f##a > f##b; NEXT; }                         \
    STENCIL(geff_##a##_##b) { i9 = f##a >= f##b; NEXT; }                        \
    STENCIL(eqff_##a##_##b) { i9 = f##a == f##b; NEXT; }
#define FCMP_A(a, ...) F_B(FCMP_B, a)
F_A(FCMP_A, x)
#define FCMPK(a, ...)                                                           \
    STENCIL(ltfk_##a) { i9 = f##a < ww_kf; NEXT; }                            \
    STENCIL(lefk_##a) { i9 = f##a <= ww_kf; NEXT; }                           \
    STENCIL(gtfk_##a) { i9 = f##a > ww_kf; NEXT; }                            \
    STENCIL(gefk_##a) { i9 = f##a >= ww_kf; NEXT; }                           \
    STENCIL(eqfk_##a) { i9 = f##a == ww_kf; NEXT; }
F_A(FCMPK, x)

// ---- Ints ----

// ia = K; ia = ib; ia = ia - 1 (a range's count, never below 0); ia = ia + ib wrapping round (a
// range's next value, as the VM steps it).
#define ISETK(a, ...)                                                           \
    STENCIL(setik_##a) { i##a = ww_ki; NEXT; }                                \
    STENCIL(deci_##a) { i##a = i##a - 1; NEXT; }
I_A(ISETK, x)
#define IMOV_B(b, a)                                                            \
    STENCIL(movi_##a##_##b) { i##a = i##b; NEXT; }                              \
    STENCIL(addwi_##a##_##b) {                                                  \
        i##a = static_cast<int64_t>(static_cast<uint64_t>(i##a) + static_cast<uint64_t>(i##b)); \
        NEXT;                                                                   \
    }
#define IMOV_A(a, ...) I_B(IMOV_B, a)
I_A(IMOV_A, x)

// ia = ib op ic, an overflow leaving for the VM (before ia changes), which reports it.
#define IBIN_C(c, a, b, name, builtin)                                          \
    STENCIL(name##_##a##_##b##_##c) {                                           \
        int64_t z;                                                              \
        if (builtin(i##b, i##c, &z)) EXIT;                                      \
        i##a = z;                                                               \
        NEXT;                                                                   \
    }
#define IBIN_B(b, a, name, builtin) I_C(IBIN_C, a, b, name, builtin)
#define IBIN_A(a, name, builtin) I_B(IBIN_B, a, name, builtin)
I_A(IBIN_A, addi, __builtin_add_overflow)
I_A(IBIN_A, subi, __builtin_sub_overflow)
I_A(IBIN_A, muli, __builtin_mul_overflow)
// ia = ib op K, ia = K op ib
#define IBINK_B(b, a, name, builtin)                                            \
    STENCIL(name##k_##a##_##b) {                                                \
        int64_t z;                                                              \
        if (builtin(i##b, ww_ki, &z)) EXIT;                                   \
        i##a = z;                                                               \
        NEXT;                                                                   \
    }                                                                           \
    STENCIL(name##kr_##a##_##b) {                                               \
        int64_t z;                                                              \
        if (builtin(ww_ki, i##b, &z)) EXIT;                                   \
        i##a = z;                                                               \
        NEXT;                                                                   \
    }
#define IBINK_A(a, name, builtin) I_B(IBINK_B, a, name, builtin)
I_A(IBINK_A, addi, __builtin_add_overflow)
I_A(IBINK_A, subi, __builtin_sub_overflow)
I_A(IBINK_A, muli, __builtin_mul_overflow)

// ia = ia + 1, the commonest: an increment, with no constant to read.
#define IINC(a, ...)                                                            \
    STENCIL(inci_##a) {                                                         \
        int64_t z;                                                              \
        if (__builtin_add_overflow(i##a, int64_t{1}, &z)) EXIT;                 \
        i##a = z;                                                               \
        NEXT;                                                                   \
    }
I_A(IINC, x)

// Two operands, the first changed: ia = ia / ib and ia % ib (the divisor checked before, by
// chkdivi), ia = ia & ib and ia | ib, ia << ib and ia >> ib (the count checked before, by chkshift),
// ia = -ib (leaving where it does not fit), the less or the more of ia and ib (as the floats'). With
// a constant: ia = ib / K, ib % K (K neither 0 nor -1),
// ib & K, ib | K, ib << K, ib >> K (K from 0 to 63).
#define ITWO_B(b, a)                                                            \
    STENCIL(divi_##a##_##b) { i##a = i##a / i##b; NEXT; }                       \
    STENCIL(modi_##a##_##b) { i##a = i##a % i##b; NEXT; }                       \
    STENCIL(andi_##a##_##b) { i##a = i##a & i##b; NEXT; }                       \
    STENCIL(ori_##a##_##b) { i##a = i##a | i##b; NEXT; }                        \
    STENCIL(shli_##a##_##b) {                                                   \
        i##a = static_cast<int64_t>(static_cast<uint64_t>(i##a) << (i##b & 63)); \
        NEXT;                                                                   \
    }                                                                           \
    STENCIL(shri_##a##_##b) { i##a = i##a >> (i##b & 63); NEXT; }               \
    STENCIL(ushri_##a##_##b) {                                                  \
        i##a = static_cast<int64_t>(static_cast<uint64_t>(i##a) >> (i##b & 63)); \
        NEXT;                                                                   \
    }                                                                           \
    STENCIL(xori_##a##_##b) { i##a = i##a ^ i##b; NEXT; }                       \
    STENCIL(waddi_##a##_##b) {                                                  \
        i##a = static_cast<int64_t>(static_cast<uint64_t>(i##a) + static_cast<uint64_t>(i##b)); \
        NEXT;                                                                   \
    }                                                                           \
    STENCIL(wsubi_##a##_##b) {                                                  \
        i##a = static_cast<int64_t>(static_cast<uint64_t>(i##a) - static_cast<uint64_t>(i##b)); \
        NEXT;                                                                   \
    }                                                                           \
    STENCIL(wmuli_##a##_##b) {                                                  \
        i##a = static_cast<int64_t>(static_cast<uint64_t>(i##a) * static_cast<uint64_t>(i##b)); \
        NEXT;                                                                   \
    }                                                                           \
    STENCIL(negi_##a##_##b) {                                                   \
        if (i##b == INT64_MIN) EXIT;                                            \
        i##a = -i##b;                                                           \
        NEXT;                                                                   \
    }                                                                           \
    STENCIL(mini_##a##_##b) { if (i##b < i##a) i##a = i##b; NEXT; }             \
    STENCIL(maxi_##a##_##b) { if (i##b > i##a) i##a = i##b; NEXT; }             \
    STENCIL(divik_##a##_##b) { i##a = i##b / ww_ki; NEXT; }                   \
    STENCIL(modik_##a##_##b) { i##a = i##b % ww_ki; NEXT; }                   \
    STENCIL(andik_##a##_##b) { i##a = i##b & ww_ki; NEXT; }                   \
    STENCIL(orik_##a##_##b) { i##a = i##b | ww_ki; NEXT; }                    \
    STENCIL(shlik_##a##_##b) {                                                  \
        i##a = static_cast<int64_t>(static_cast<uint64_t>(i##b) << ww_ki);    \
        NEXT;                                                                   \
    }                                                                           \
    STENCIL(shrik_##a##_##b) { i##a = i##b >> ww_ki; NEXT; }                    \
    STENCIL(ushrik_##a##_##b) {                                                 \
        i##a = static_cast<int64_t>(static_cast<uint64_t>(i##b) >> ww_ki);    \
        NEXT;                                                                   \
    }                                                                           \
    STENCIL(xorik_##a##_##b) { i##a = i##b ^ ww_ki; NEXT; }                   \
    STENCIL(waddik_##a##_##b) {                                                 \
        i##a = static_cast<int64_t>(static_cast<uint64_t>(i##b) + static_cast<uint64_t>(ww_ki)); \
        NEXT;                                                                   \
    }                                                                           \
    STENCIL(wsubik_##a##_##b) {                                                 \
        i##a = static_cast<int64_t>(static_cast<uint64_t>(i##b) - static_cast<uint64_t>(ww_ki)); \
        NEXT;                                                                   \
    }                                                                           \
    STENCIL(wmulik_##a##_##b) {                                                 \
        i##a = static_cast<int64_t>(static_cast<uint64_t>(i##b) * static_cast<uint64_t>(ww_ki)); \
        NEXT;                                                                   \
    }
#define ITWO_A(a, ...) I_B(ITWO_B, a)
I_A(ITWO_A, x)

// Checks of one int, leaving for the VM where they fail: a divisor that is 0 or -1 (which the
// VM's division has rules for), a shift's count outside 0 to 63, an int's toByte() outside 0 to
// 255, |ia| that does not fit.
#define ICHECK(a, ...)                                                          \
    STENCIL(chkdivi_##a) {                                                      \
        if (static_cast<uint64_t>(i##a) + 1 <= 1) EXIT;                         \
        NEXT;                                                                   \
    }                                                                           \
    STENCIL(chkshift_##a) {                                                     \
        if (static_cast<uint64_t>(i##a) > 63) EXIT;                             \
        NEXT;                                                                   \
    }                                                                           \
    STENCIL(chkbyte_##a) {                                                      \
        if (static_cast<uint64_t>(i##a) > 255) EXIT;                            \
        NEXT;                                                                   \
    }                                                                           \
    STENCIL(absi_##a) {                                                         \
        if (i##a == INT64_MIN) EXIT;                                            \
        if (i##a < 0) i##a = -i##a;                                             \
        NEXT;                                                                   \
    }
I_A(ICHECK, x)

// Jumps on two ints where `ia op ib` holds (as the build's check uses them).
#define IJUMP_B(b, a)                                                           \
    STENCIL(jgeii_##a##_##b) { if (i##a >= i##b) JUMP; NEXT; }                  \
    STENCIL(jneii_##a##_##b) { if (i##a != i##b) JUMP; NEXT; }
#define IJUMP_A(a, ...) I_B(IJUMP_B, a)
I_A(IJUMP_A, x)

// The VM's typed jumps on ints: to the target where `ia op ib` (or `ia op K`) does not hold.
#define IJN_B(b, a)                                                             \
    STENCIL(jnltii_##a##_##b) { if (!(i##a < i##b)) JUMP; NEXT; }               \
    STENCIL(jnleii_##a##_##b) { if (!(i##a <= i##b)) JUMP; NEXT; }              \
    STENCIL(jngtii_##a##_##b) { if (!(i##a > i##b)) JUMP; NEXT; }               \
    STENCIL(jngeii_##a##_##b) { if (!(i##a >= i##b)) JUMP; NEXT; }              \
    STENCIL(jneqii_##a##_##b) { if (!(i##a == i##b)) JUMP; NEXT; }
#define IJN_A(a, ...) I_B(IJN_B, a)
I_A(IJN_A, x)
#define IJNK(a, ...)                                                            \
    STENCIL(jnltik_##a) { if (!(i##a < ww_ki)) JUMP; NEXT; }                  \
    STENCIL(jnleik_##a) { if (!(i##a <= ww_ki)) JUMP; NEXT; }                 \
    STENCIL(jngtik_##a) { if (!(i##a > ww_ki)) JUMP; NEXT; }                  \
    STENCIL(jngeik_##a) { if (!(i##a >= ww_ki)) JUMP; NEXT; }                 \
    STENCIL(jneqik_##a) { if (!(i##a == ww_ki)) JUMP; NEXT; }
I_A(IJNK, x)

// The VM's typed comparisons of ints, their answer in i9.
#define ICMP_B(b, a)                                                            \
    STENCIL(ltii_##a##_##b) { i9 = i##a < i##b; NEXT; }                         \
    STENCIL(leii_##a##_##b) { i9 = i##a <= i##b; NEXT; }                        \
    STENCIL(gtii_##a##_##b) { i9 = i##a > i##b; NEXT; }                         \
    STENCIL(geii_##a##_##b) { i9 = i##a >= i##b; NEXT; }                        \
    STENCIL(eqii_##a##_##b) { i9 = i##a == i##b; NEXT; }
#define ICMP_A(a, ...) I_B(ICMP_B, a)
I_A(ICMP_A, x)
#define ICMPK(a, ...)                                                           \
    STENCIL(ltik_##a) { i9 = i##a < ww_ki; NEXT; }                            \
    STENCIL(leik_##a) { i9 = i##a <= ww_ki; NEXT; }                           \
    STENCIL(gtik_##a) { i9 = i##a > ww_ki; NEXT; }                            \
    STENCIL(geik_##a) { i9 = i##a >= ww_ki; NEXT; }                           \
    STENCIL(eqik_##a) { i9 = i##a == ww_ki; NEXT; }
I_A(ICMPK, x)

// On the flag register: to the target where it is false, or true (JumpIfFalse, And, Or).
STENCIL(jfalse) {
    if (i9 == 0) JUMP;
    NEXT;
}
// The flag register's bool turned round (RNot of a comparison's answer).
STENCIL(notflag) {
    i9 = i9 == 0;
    NEXT;
}
STENCIL(jtrue) {
    if (i9 != 0) JUMP;
    NEXT;
}
// A bool slot read into the flag register, leaving for the VM where it holds no bool.
STENCIL(loadflag) {
    if ((*slotAt(base)).kind != kKindBool) EXIT;
    i9 = static_cast<uint8_t>((*slotAt(base)).i) != 0;
    NEXT;
}

// ---- Lists and bytes ----

// fa = list's float item ic (or item K): a packed list of floats, none null, the item in
// range; anything else the VM's.
#define IDXF_B(c, a)                                                            \
    STENCIL(idxf_##a##_##c) {                                                   \
        const uint8_t* list = objectIn((*slotAt(base)), kTypeList);             \
        if (list == nullptr || !mayRead(list, ctx)) EXIT;                       \
        const uint8_t* pack = packOf(list, kStoreFloats);                       \
        if (pack == nullptr || static_cast<uint64_t>(i##c) >= word<uint64_t>(pack + kPackCount)) EXIT; \
        f##a = word<double>(word<const uint8_t*>(pack + kPackBlock) + i##c * 8); \
        NEXT;                                                                   \
    }
#define IDXF_A(a, ...) I_B(IDXF_B, a)
F_W(IDXF_A, x)
#define IDXFK(a, ...)                                                           \
    STENCIL(idxfk_##a) {                                                        \
        const uint8_t* list = objectIn((*slotAt(base)), kTypeList);             \
        if (list == nullptr || !mayRead(list, ctx)) EXIT;                       \
        const uint8_t* pack = packOf(list, kStoreFloats);                       \
        if (pack == nullptr || static_cast<uint64_t>(ww_ki) >= word<uint64_t>(pack + kPackCount)) EXIT; \
        f##a = word<double>(word<const uint8_t*>(pack + kPackBlock) + ww_ki * 8); \
        NEXT;                                                                   \
    }
F_W(IDXFK, x)

// ia = item ic of a whole number's list: a packed list of ints, none null, or bytes; the item in
// range. Which it was the region tells by the object, where it writes the item to its slot
// (storew: a byte from bytes, an int from a list).
// A List<byte>'s items, kept as a Bytes keeps them (one byte each, 2026-11-13): its pack, or null.
INLINE const uint8_t* bytesPack(const uint8_t* list) {
    const uint8_t* pack = packOf(list, kStoreNarrow);
    return pack != nullptr && pack[kPackNarrow] == kNarrowByte ? pack : nullptr;
}
INLINE bool wholeItem(const Slot& s, int64_t index, const Ctx* ctx, int64_t& out) {
    if (s.kind != kKindObj) return false;
    const uint8_t* o = s.p;
    if (!mayRead(o, ctx)) return false;
    if (o[kObjType] == kTypeBytes) {
        const uint8_t* first = word<const uint8_t*>(o + kBytesData);
        const uint8_t* end = word<const uint8_t*>(o + kBytesData + 8);
        if (static_cast<uint64_t>(index) >= static_cast<uint64_t>(end - first)) return false;
        out = first[index];
        return true;
    }
    if (o[kObjType] != kTypeList) return false;
    if (const uint8_t* bytes = bytesPack(o)) {
        if (static_cast<uint64_t>(index) >= word<uint64_t>(bytes + kPackCount)) return false;
        out = word<const uint8_t*>(bytes + kPackBlock)[index];
        return true;
    }
    const uint8_t* pack = packOf(o, kStoreInts);
    if (pack == nullptr || static_cast<uint64_t>(index) >= word<uint64_t>(pack + kPackCount)) return false;
    out = word<int64_t>(word<const uint8_t*>(pack + kPackBlock) + index * 8);
    return true;
}
#define IDXW_B(c, a)                                                            \
    STENCIL(idxw_##a##_##c) {                                                   \
        int64_t v;                                                              \
        if (!wholeItem((*slotAt(base)), i##c, ctx, v)) EXIT;                    \
        i##a = v;                                                               \
        NEXT;                                                                   \
    }
#define IDXW_A(a, ...) I_B(IDXW_B, a)
I_A(IDXW_A, x)
#define IDXWK(a, ...)                                                           \
    STENCIL(idxwk_##a) {                                                        \
        int64_t v;                                                              \
        if (!wholeItem((*slotAt(base)), ww_ki, ctx, v)) EXIT;                 \
        i##a = v;                                                               \
        NEXT;                                                                   \
    }
I_A(IDXWK, x)

// Item ic of a list (or bytes) in slot ww_slot set to a value, as the VM's SetIndex does at
// once: a float into a packed list of floats, none null; an int into a packed list of ints, or
// one from 0 to 255 into bytes; a byte into bytes. The object its core's own, a list not frozen,
// the item in range; anything else the VM's.
INLINE uint8_t* listToChange(const Slot& s, int64_t index, uint8_t store, const Ctx* ctx) {
    if (s.kind != kKindObj) return nullptr;
    const uint8_t* o = s.p;
    if (o[kObjType] != kTypeList || o[kObjCore] != ctx->core || o[kObjFrozen] != 0) return nullptr;
    const uint8_t* pack = packOf(o, store);
    if (pack == nullptr || static_cast<uint64_t>(index) >= word<uint64_t>(pack + kPackCount)) return nullptr;
    return word<uint8_t*>(pack + kPackBlock) + index * 8;
}
// A List<bool>'s item (2026-11-15: a Mask's pixels, a flood's marks): a packed list of bools, a
// byte each (0 or 1), none null, the item in range; to read (its core's or none's) or to change
// (its core's own, not frozen). Null where not: the VM's.
INLINE uint8_t* boolItem(const Slot& s, int64_t index, const Ctx* ctx, bool change) {
    if (s.kind != kKindObj) return nullptr;
    const uint8_t* o = s.p;
    if (o[kObjType] != kTypeList) return nullptr;
    if (change ? (o[kObjCore] != ctx->core || o[kObjFrozen] != 0) : !mayRead(o, ctx)) return nullptr;
    const uint8_t* pack = packOf(o, kStoreBools);
    if (pack == nullptr || static_cast<uint64_t>(index) >= word<uint64_t>(pack + kPackCount)) return nullptr;
    return word<uint8_t*>(pack + kPackBlock) + index;
}
// The flag register = item ic of a List<bool>; item ic = the flag register.
#define IDXBOOL(c, ...)                                                         \
    STENCIL(idxbool_##c) {                                                      \
        const uint8_t* at = boolItem((*slotAt(base)), i##c, ctx, false);        \
        if (at == nullptr) EXIT;                                                \
        i9 = *at != 0;                                                          \
        NEXT;                                                                   \
    }                                                                           \
    STENCIL(setbool_##c) {                                                      \
        uint8_t* at = boolItem((*slotAt(base)), i##c, ctx, true);               \
        if (at == nullptr) EXIT;                                                \
        *at = i9 != 0;                                                          \
        NEXT;                                                                   \
    }                                                                           \
    STENCIL(hidxbool_##c) {                                                     \
        const uint64_t* h = ctx->lists + 4 * ww_slot;                         \
        if (static_cast<uint64_t>(i##c) >= h[1]) EXIT;                          \
        i9 = reinterpret_cast<const uint8_t*>(h[0])[i##c] != 0;                 \
        NEXT;                                                                   \
    }                                                                           \
    STENCIL(hsetbool_##c) {                                                     \
        const uint64_t* h = ctx->lists + 4 * ww_slot;                         \
        if (static_cast<uint64_t>(i##c) >= h[2]) EXIT;                          \
        reinterpret_cast<uint8_t*>(h[0])[i##c] = i9 != 0;                       \
        NEXT;                                                                   \
    }
I_A(IDXBOOL, x)
INLINE uint8_t* byteToChange(const Slot& s, int64_t index, const Ctx* ctx) {
    if (s.kind != kKindObj) return nullptr;
    const uint8_t* o = s.p;
    if (o[kObjType] == kTypeList) {
        // A List<byte>: its core's own and not frozen, as a list to change is.
        if (o[kObjCore] != ctx->core || o[kObjFrozen] != 0) return nullptr;
        const uint8_t* bytes = bytesPack(o);
        if (bytes == nullptr || static_cast<uint64_t>(index) >= word<uint64_t>(bytes + kPackCount)) return nullptr;
        return word<uint8_t*>(bytes + kPackBlock) + index;
    }
    if (o[kObjType] != kTypeBytes || o[kObjCore] != ctx->core) return nullptr;
    uint8_t* first = word<uint8_t*>(o + kBytesData);
    const uint8_t* end = word<const uint8_t*>(o + kBytesData + 8);
    if (static_cast<uint64_t>(index) >= static_cast<uint64_t>(end - first)) return nullptr;
    return first + index;
}
#define SETF_B(c, v)                                                            \
    STENCIL(setf_##v##_##c) {                                                   \
        uint8_t* at = listToChange((*slotAt(base)), i##c, kStoreFloats, ctx);   \
        if (at == nullptr) EXIT;                                                \
        { const double d_ = f##v; __builtin_memcpy(at, &d_, 8); }                                              \
        NEXT;                                                                   \
    }
#define SETF_A(v, ...) I_B(SETF_B, v)
F_A(SETF_A, x)
#define SETW_B(c, v)                                                            \
    STENCIL(setwi_##v##_##c) {                                                  \
        const Slot& s = (*slotAt(base));                                        \
        if (s.kind == kKindObj && (s.p[kObjType] == kTypeBytes || (s.p[kObjType] == kTypeList && bytesPack(s.p) != nullptr))) { \
            uint8_t* at = byteToChange(s, i##c, ctx);                           \
            if (at == nullptr || static_cast<uint64_t>(i##v) > 255) EXIT;       \
            *at = static_cast<uint8_t>(i##v);                                   \
            NEXT;                                                               \
        }                                                                       \
        uint8_t* at = listToChange(s, i##c, kStoreInts, ctx);                   \
        if (at == nullptr) EXIT;                                                \
        __builtin_memcpy(at, &i##v, 8);                                              \
        NEXT;                                                                   \
    }                                                                           \
    STENCIL(setwb_##v##_##c) {                                                  \
        uint8_t* at = byteToChange((*slotAt(base)), i##c, ctx);                 \
        if (at == nullptr) {                                                    \
            /* a byte into a list of ints: the int it is */                     \
            uint8_t* item = listToChange((*slotAt(base)), i##c, kStoreInts, ctx); \
            if (item == nullptr) EXIT;                                          \
            __builtin_memcpy(item, &i##v, 8);                                   \
            NEXT;                                                               \
        }                                                                       \
        *at = static_cast<uint8_t>(i##v);                                       \
        NEXT;                                                                   \
    }
#define SETW_A(v, ...) I_B(SETW_B, v)
I_A(SETW_A, x)

// ia (or fa) = a field of item ic of a list of a value type's items, in slot ww_slot, read where
// it is (`items[i].x`, M9.5 B1): the list's items of the class ww_ki, none null, the item in
// range, the field ww_slot2 bytes into it; a byte (idxpb), an int (idxpi) or a float (idxpf).
// Anything else the VM's.
INLINE const uint8_t* itemField(const Slot& s, int64_t index, const Ctx* ctx) {
    if (s.kind != kKindObj) return nullptr;
    const uint8_t* o = s.p;
    if (o[kObjType] != kTypeList || !mayRead(o, ctx)) return nullptr;
    const uint8_t* pack = packOf(o, kStoreItems);
    if (pack == nullptr || word<int64_t>(pack + kPackItemClass) != ww_ki) return nullptr;
    if (static_cast<uint64_t>(index) >= word<uint64_t>(pack + kPackCount)) return nullptr;
    return word<const uint8_t*>(pack + kPackBlock) + index * static_cast<int64_t>(word<uint32_t>(pack + kPackWidth)) + ww_slot2;
}
#define IDXP_B(c, a)                                                            \
    STENCIL(idxpb_##a##_##c) {                                                  \
        const uint8_t* at = itemField((*slotAt(base)), i##c, ctx);              \
        if (at == nullptr) EXIT;                                                \
        i##a = *at;                                                             \
        NEXT;                                                                   \
    }                                                                           \
    STENCIL(idxpi_##a##_##c) {                                                  \
        const uint8_t* at = itemField((*slotAt(base)), i##c, ctx);              \
        if (at == nullptr) EXIT;                                                \
        i##a = word<int64_t>(at);                                               \
        NEXT;                                                                   \
    }
#define IDXP_A(a, ...) I_B(IDXP_B, a)
I_A(IDXP_A, x)
#define IDXPF_B(c, a)                                                           \
    STENCIL(idxpf_##a##_##c) {                                                  \
        const uint8_t* at = itemField((*slotAt(base)), i##c, ctx);              \
        if (at == nullptr) EXIT;                                                \
        f##a = word<double>(at);                                                \
        NEXT;                                                                   \
    }
#define IDXPF_A(a, ...) I_B(IDXPF_B, a)
F_W(IDXPF_A, x)

// Slot ww_slot2 = a Ref to item ic of the list of a value type's items in slot ww_slot, none
// null, the item in range: the item where it is, as GetIndexRef gives it. Anything else the VM's.
// A value written as two words, its kind and the rest first and its payload after, and read back
// the same way: what the processor hands on from a store to a load of the same place and size,
// where a load of part of one 16-byte store waits for the store to reach the cache.
INLINE void putValue(Slot* at, uint64_t head, uint64_t payload) {
    uint8_t* const p = reinterpret_cast<uint8_t*>(at);
    __builtin_memcpy(p, &head, 8);
    __asm__ volatile("" ::: "memory");
    __builtin_memcpy(p + 8, &payload, 8);
}
// A 16-byte value the compiler may not take for a constant: made in a register where it is used.
// A constant would be read from the stencil's data (`pand (%rip)`), which a copied stencil does
// not keep on a 16-byte boundary, as an SSE instruction's memory operand must be.
INLINE __m128i made128(__m128i v) {
    __asm__("" : "+x"(v));
    return v;
}
// n bytes moved forward from x to o (a loop template's items moved at once). Where it is more
// than the caches would keep (1 MB on) and the two do not overlap, the stores go past the cache
// (non-temporal), 64 bytes at a time: the place written is not read into the cache first, a
// third of the memory's traffic saved (2026-11-15: a picture's copy, 48 MB in 3.8 ms by rep
// movsb). Otherwise rep movsb, as the steps would go.
INLINE void moveBytes(uint8_t* o, const uint8_t* x, uint64_t n) {
    if (n >= (1u << 20) && (o + n <= x || x + n <= o)) {
        uint64_t head = (64 - (reinterpret_cast<uintptr_t>(o) & 63)) & 63;
        n -= head;
        __asm__ volatile("rep movsb" : "+D"(o), "+S"(x), "+c"(head) : : "memory");
        for (; n >= 64; n -= 64, o += 64, x += 64) {
            const __m128i a = _mm_loadu_si128(reinterpret_cast<const __m128i*>(x));
            const __m128i b = _mm_loadu_si128(reinterpret_cast<const __m128i*>(x + 16));
            const __m128i c = _mm_loadu_si128(reinterpret_cast<const __m128i*>(x + 32));
            const __m128i d = _mm_loadu_si128(reinterpret_cast<const __m128i*>(x + 48));
            _mm_stream_si128(reinterpret_cast<__m128i*>(o), a);
            _mm_stream_si128(reinterpret_cast<__m128i*>(o + 16), b);
            _mm_stream_si128(reinterpret_cast<__m128i*>(o + 32), c);
            _mm_stream_si128(reinterpret_cast<__m128i*>(o + 48), d);
        }
        _mm_sfence();
    }
    __asm__ volatile("rep movsb" : "+D"(o), "+S"(x), "+c"(n) : : "memory");
}
INLINE const uint8_t* itemsList(const Slot& s, const Ctx* ctx) {
    if (s.kind != kKindObj) return nullptr;
    const uint8_t* o = s.p;
    if (o[kObjType] != kTypeList || !mayRead(o, ctx)) return nullptr;
    return packOf(o, kStoreItems) != nullptr ? o : nullptr;
}
#define IREF(c, ...)                                                            \
    STENCIL(iref_##c) {                                                         \
        const uint8_t* list = itemsList((*slotAt(base)), ctx);                  \
        if (list == nullptr) EXIT;                                              \
        const uint8_t* pack = word<const uint8_t*>(list + kListPack);           \
        if (static_cast<uint64_t>(i##c) >= word<uint64_t>(pack + kPackCount)) EXIT; \
        putValue(&(*slot2At(base)), kKindRef | static_cast<uint64_t>(i##c) << 32, reinterpret_cast<uint64_t>(list)); \
        NEXT;                                                                   \
    }
I_A(IREF, x)
// Slot (ww_slot2 >> 8 & 0xFFFFFF) = a copy of item ic of the list in slot ww_slot, a value kept
// in the value itself (a Color): the list's items of the class ww_ki, kept with no type of their
// own, none null, the item in range; the item's first (ww_slot2 & 0xFF) bytes, and the type's
// number (ww_slot2 >> 32). Anything else the VM's.
#define IDXSM(c, ...)                                                           \
    STENCIL(idxsm_##c) {                                                        \
        const uint8_t* list = itemsList((*slotAt(base)), ctx);                  \
        if (list == nullptr) EXIT;                                              \
        const uint8_t* pack = word<const uint8_t*>(list + kListPack);           \
        if (word<int64_t>(pack + kPackItemClass) != ww_ki || word<const void*>(pack + kPackItemType) != nullptr || \
            word<uint32_t>(pack + kPackTypeSlot) != 0 || static_cast<uint64_t>(i##c) >= word<uint64_t>(pack + kPackCount)) \
            EXIT;                                                               \
        const uint64_t what = static_cast<uint64_t>(ww_slot2);                \
        const uint32_t size = static_cast<uint32_t>(what & 0xFF), id = static_cast<uint32_t>(what >> 32); \
        const uint8_t* from = word<const uint8_t*>(pack + kPackBlock) + i##c * static_cast<int64_t>(word<uint32_t>(pack + kPackWidth)); \
        uint64_t bits = 0;                                                      \
        for (uint32_t k = 0; k < size; k++) bits |= static_cast<uint64_t>(from[k]) << (8 * k); \
        putValue(&base[(what >> 8) & 0xFFFFFF], kKindSmall | static_cast<uint64_t>(id) << 32, bits); \
        NEXT;                                                                   \
    }
I_A(IDXSM, x)
// Slot ww_slot2 = global ww_ki, as the VM reads it (B3); one not worked out yet the VM's.
STENCIL(getg) {
    const Slot* g = reinterpret_cast<const Slot*>(ctx->globals) + ww_ki;
    if (g->kind == kKindUndefined) EXIT;
    __builtin_memcpy(&(*slot2At(base)), g, sizeof(Slot));
    NEXT;
}
// Making a value of a value type kept in the value itself, from its header's byte fields (B3:
// `Color(r, g, b, a)`): ia |= the byte in ic, ww_ki bits up; then slot (ww_slot2 &
// 0xFFFFFFFF) = a value of the type numbered (ww_slot2 >> 32) holding ia, where slot ww_slot
// holds the class ww_ki (what is called); anything else the VM's.
#define PACKB_B(c, a) STENCIL(packb_##a##_##c) { i##a |= (static_cast<uint64_t>(i##c) & 0xFF) << ww_ki; NEXT; }
#define PACKB_A(a, ...) I_B(PACKB_B, a)
I_A(PACKB_A, x)
#define MKSM(a, ...)                                                            \
    STENCIL(mksm_##a) {                                                         \
        const Slot& f = (*slotAt(base));                                        \
        if (f.kind != kKindObj || reinterpret_cast<int64_t>(f.p) != ww_ki) EXIT; \
        const uint64_t what = static_cast<uint64_t>(ww_slot2);                \
        putValue(&base[what & 0xFFFFFFFF], kKindSmall | (what >> 32) << 32, static_cast<uint64_t>(i##a)); \
        NEXT;                                                                   \
    }
I_A(MKSM, x)
// Slot ww_slot2 = a Ref to item ic of the list of a value type's items in slot ww_slot, to be
// changed there (PlaceIndex, B3): the list its core's own and not frozen, none null, the item in
// range. Anything else the VM's.
#define PLACEI(c, ...)                                                          \
    STENCIL(placei_##c) {                                                       \
        const Slot& s = (*slotAt(base));                                        \
        if (s.kind != kKindObj || s.p[kObjType] != kTypeList || s.p[kObjCore] != ctx->core || s.p[kObjFrozen] != 0 || s.p == ctx->band) EXIT; \
        const uint8_t* pack = packOf(s.p, kStoreItems);                         \
        if (pack == nullptr || static_cast<uint64_t>(i##c) >= word<uint64_t>(pack + kPackCount)) EXIT; \
        putValue(&(*slot2At(base)), kKindRef | static_cast<uint64_t>(i##c) << 32, reinterpret_cast<uint64_t>(s.p)); \
        NEXT;                                                                   \
    }
I_A(PLACEI, x)
// Item ic of the list in slot ww_slot set to the value in slot (ww_slot2 & 0xFFFFFF) (B3): the
// list its core's own and not frozen, of the class ww_ki's items kept in the value itself, none
// null and none with a type of its own, the item in range; the value a Small one of the type
// numbered (ww_slot2 >> 32), its first ((ww_slot2 >> 24) & 0xFF) bytes copied; or a Ref to
// that item itself (made there already). Anything else the VM's.
#define SETSM(c, ...)                                                           \
    STENCIL(setsm_##c) {                                                        \
        const Slot& s = (*slotAt(base));                                        \
        if (s.kind != kKindObj || s.p[kObjType] != kTypeList || s.p[kObjCore] != ctx->core || s.p[kObjFrozen] != 0 || s.p == ctx->band) EXIT; \
        const uint8_t* pack = packOf(s.p, kStoreItems);                         \
        if (pack == nullptr || word<int64_t>(pack + kPackItemClass) != ww_ki || word<const void*>(pack + kPackItemType) != nullptr || \
            word<uint32_t>(pack + kPackTypeSlot) != 0 || static_cast<uint64_t>(i##c) >= word<uint64_t>(pack + kPackCount)) \
            EXIT;                                                               \
        const uint64_t what = static_cast<uint64_t>(ww_slot2);                \
        const Slot& v = base[what & 0xFFFFFF];                                  \
        const uint64_t head = word<uint64_t>(reinterpret_cast<const uint8_t*>(&v)); \
        if ((head & 0xFFFFFFFF) == kKindRef && v.p == s.p && (head >> 32) == static_cast<uint64_t>(i##c)) NEXT; \
        if ((head & 0xFF) != kKindSmall || (head >> 32) != what >> 32) EXIT;    \
        uint8_t* to = word<uint8_t*>(pack + kPackBlock) + i##c * static_cast<int64_t>(word<uint32_t>(pack + kPackWidth)); \
        const uint64_t bits = static_cast<uint64_t>(v.i);                       \
        for (uint32_t k = 0; k < ((what >> 24) & 0xFF); k++) to[k] = static_cast<uint8_t>(bits >> (8 * k)); \
        NEXT;                                                                   \
    }
I_A(SETSM, x)

// ---- A list's add (2026-11-15) ----

// Where list.add(x) puts its item without the VM: the list in slot ww_slot, packed as `store`
// (and, for narrow items, as bytes), none null, its core's own, not frozen, not the object lent
// to the section; `width` bytes left in its block's room past its end. The item is then written
// there and the end moved on (added), as the VM's add does where its vector has room; a list
// with none is the VM's, which makes room (and counts the memory it takes). Null for anything
// else. The VM's add gives true, written to slot ww_slot2.
INLINE uint8_t* roomToAdd(const Slot& s, const Ctx* ctx, uint8_t store, uint8_t*& pack) {
    if (s.kind != kKindObj) return nullptr;
    const uint8_t* o = s.p;
    if (o[kObjType] != kTypeList || o[kObjCore] != ctx->core || o[kObjFrozen] != 0 || o == ctx->band) return nullptr;
    pack = word<uint8_t*>(o + kListPack);
    if (pack == nullptr || pack[kPackStore] != store || pack[kPackNullable] != 0) return nullptr;
    if (store == kStoreNarrow && pack[kPackNarrow] != kNarrowByte) return nullptr;
    uint8_t* const end = word<uint8_t*>(pack + kPackBlock + 8);
    const uint8_t* const room = word<uint8_t*>(pack + kPackBlock + 16);
    if (static_cast<uint64_t>(room - end) < word<uint32_t>(pack + kPackWidth)) return nullptr;
    return end;
}
INLINE void added(uint8_t* pack) {
    const uint32_t width = word<uint32_t>(pack + kPackWidth);
    uint8_t* const end = word<uint8_t*>(pack + kPackBlock + 8) + width;
    __builtin_memcpy(pack + kPackBlock + 8, &end, 8);
    const uint64_t count = word<uint64_t>(pack + kPackCount) + 1;
    __builtin_memcpy(pack + kPackCount, &count, 8);
}
// An int (ia) added to a List<int>.
#define ADDLI(a, ...)                                                           \
    STENCIL(pushi_##a) {                                                        \
        uint8_t* pack;                                                          \
        uint8_t* at = roomToAdd((*slotAt(base)), ctx, kStoreInts, pack);        \
        if (at == nullptr) EXIT;                                                \
        __builtin_memcpy(at, &i##a, 8);                                         \
        added(pack);                                                            \
        storeSlot2At(base, kKindBool, 1);                                       \
        NEXT;                                                                   \
    }                                                                           \
    STENCIL(pushb_##a) {                                                        \
        uint8_t* pack;                                                          \
        uint8_t* at = roomToAdd((*slotAt(base)), ctx, kStoreInts, pack);        \
        if (at != nullptr) {                                                    \
            __builtin_memcpy(at, &i##a, 8);                                     \
        } else {                                                                \
            at = roomToAdd((*slotAt(base)), ctx, kStoreNarrow, pack);           \
            if (at == nullptr) EXIT;                                            \
            *at = static_cast<uint8_t>(i##a);                                   \
        }                                                                       \
        added(pack);                                                            \
        storeSlot2At(base, kKindBool, 1);                                       \
        NEXT;                                                                   \
    }
I_A(ADDLI, x)
// A float (fa) added to a List<float>.
#define ADDLF(a, ...)                                                           \
    STENCIL(pushf_##a) {                                                        \
        uint8_t* pack;                                                          \
        uint8_t* at = roomToAdd((*slotAt(base)), ctx, kStoreFloats, pack);      \
        if (at == nullptr) EXIT;                                                \
        { const double d_ = f##a; __builtin_memcpy(at, &d_, 8); }               \
        added(pack);                                                            \
        storeSlot2At(base, kKindBool, 1);                                       \
        NEXT;                                                                   \
    }
F_A(ADDLF, x)
// The value in slot (ww_slot2 & 0xFFFFFF) added to a list of the class ww_ki's items, kept in
// the value itself (at most 8 bytes an item), none with a type of its own; true to slot
// (ww_slot2 >> 24 & 0xFFFF). The value a Small one of the type numbered (ww_slot2 >> 40); or a
// Ref, none deep, to an item of a list of the same class's items, which may be read here
// (to.add(from[i])): its bytes copied. Anything else the VM's.
STENCIL(pushsm) {
    uint8_t* pack;
    uint8_t* at = roomToAdd((*slotAt(base)), ctx, kStoreItems, pack);
    if (at == nullptr || word<int64_t>(pack + kPackItemClass) != ww_ki || word<const void*>(pack + kPackItemType) != nullptr ||
        word<uint32_t>(pack + kPackTypeSlot) != 0)
        EXIT;
    const uint32_t width = word<uint32_t>(pack + kPackWidth);
    if (width > 8) EXIT;
    const uint64_t what = static_cast<uint64_t>(ww_slot2);
    const Slot& v = base[what & 0xFFFFFF];
    const uint64_t head = word<uint64_t>(reinterpret_cast<const uint8_t*>(&v));
    uint64_t bits = 0;
    if ((head & 0xFF) == kKindSmall) {
        if ((head >> 32) != what >> 40) EXIT;
        bits = static_cast<uint64_t>(v.i);
    } else {
        if ((head & 0xFFFFFFFF) != kKindRef) EXIT;
        const uint8_t* from = v.p;
        if (from[kObjType] != kTypeList || !mayRead(from, ctx)) EXIT;
        const uint8_t* fromPack = packOf(from, kStoreItems);
        if (fromPack == nullptr || word<int64_t>(fromPack + kPackItemClass) != ww_ki || word<const void*>(fromPack + kPackItemType) != nullptr ||
            word<uint32_t>(fromPack + kPackTypeSlot) != 0 || (head >> 32) >= word<uint64_t>(fromPack + kPackCount))
            EXIT;
        const uint8_t* item = word<const uint8_t*>(fromPack + kPackBlock) + (head >> 32) * width;
        for (uint32_t k = 0; k < width; k++) bits |= static_cast<uint64_t>(item[k]) << (8 * k);
    }
    for (uint32_t k = 0; k < width; k++) at[k] = static_cast<uint8_t>(bits >> (8 * k));
    added(pack);
    storeSlot(base, static_cast<int64_t>((what >> 24) & 0xFFFF), kKindBool, 1);
    NEXT;
}
// OwnLocal where slot ww_slot is in memory (B2): a Ref, none deep, to an item of a list of the
// class ww_ki's items kept in the value itself, none with a type of its own, made a copy of its
// first (ww_slot2 & 0xFF) bytes, of the type numbered (ww_slot2 >> 32); anything but a Ref
// as it is. Any other Ref the VM's (a box, or a frame's place, which stays).
STENCIL(ownsm) {
    Slot& s = (*slotAt(base));
    const uint64_t head = word<uint64_t>(reinterpret_cast<const uint8_t*>(&s));
    if ((head & 0xFF) != kKindRef) NEXT;
    if ((head & 0xFFFFFFFF) != kKindRef) EXIT;
    const uint8_t* o = s.p;
    if (o[kObjType] == kTypePlaces) {
        // A value in a place of a frame's, of the class and with no type of its own (2026-11-13:
        // one made where a call leaves it): its bytes copied out.
        if (word<int64_t>(o + kPlaceClass) != ww_ki || word<const void*>(o + kPlaceType) != nullptr) EXIT;
        const uint64_t what = static_cast<uint64_t>(ww_slot2);
        uint64_t bits = 0;
        for (uint32_t k = 0; k < (what & 0xFF); k++) bits |= static_cast<uint64_t>(o[kPlaceData + k]) << (8 * k);
        putValue(&s, kKindSmall | (what >> 32) << 32, bits);
        NEXT;
    }
    if (o[kObjType] != kTypeList || !mayRead(o, ctx)) EXIT;
    const uint8_t* pack = packOf(o, kStoreItems);
    if (pack == nullptr || word<int64_t>(pack + kPackItemClass) != ww_ki || word<const void*>(pack + kPackItemType) != nullptr ||
        word<uint32_t>(pack + kPackTypeSlot) != 0 || (head >> 32) >= word<uint64_t>(pack + kPackCount))
        EXIT;
    const uint64_t what = static_cast<uint64_t>(ww_slot2);
    const uint8_t* from = word<const uint8_t*>(pack + kPackBlock) + (head >> 32) * word<uint32_t>(pack + kPackWidth);
    uint64_t bits = 0;
    for (uint32_t k = 0; k < (what & 0xFF); k++) bits |= static_cast<uint64_t>(from[k]) << (8 * k);
    putValue(&s, kKindSmall | (what >> 32) << 32, bits);
    NEXT;
}
// Slot ww_slot2 = the variable the function captures as its upvalue ww_ki, as the VM's
// R_GET_UPVALUE reads it (2026-11-13: a loop in a lambda reading a variable around it).
STENCIL(getuv) {
    const uint8_t* cell = ctx->upvalues[ww_ki];
    __builtin_memcpy(&(*slot2At(base)), word<const Slot*>(cell + kUpvalueLocation), sizeof(Slot));
    NEXT;
}
// The flag register = whether slot ww_slot holds an object of exactly the class global ww_ki
// holds (JumpIfNotClass: a method put in place behind the check, 2026-11-13).
STENCIL(clsflag) {
    const Slot& s = (*slotAt(base));
    const Slot* g = reinterpret_cast<const Slot*>(ctx->globals) + ww_ki;
    i9 = s.kind == kKindObj && s.p[kObjType] == kTypeInstance && g->kind == kKindObj && word<const uint8_t*>(s.p + kInstClass) == g->p;
    NEXT;
}
// `value!!` where slot ww_slot is in memory: nothing, unless it is null, which the VM throws for.
STENCIL(notnull) {
    if ((*slotAt(base)).kind == kKindNull) EXIT;
    NEXT;
}
// KeepLocal where slot ww_slot is in memory: nothing, unless it holds a Ref to a place of the
// frame's, which the VM copies.
STENCIL(keepl) {
    const Slot& s = (*slotAt(base));
    if ((word<uint64_t>(reinterpret_cast<const uint8_t*>(&s)) & 0xFF) == kKindRef && s.p[kObjType] == kTypePlaces) EXIT;
    NEXT;
}
// ia (or fa) = a field of the value of a value type in slot ww_slot: a Small value of the type
// numbered (ww_slot2 >> 32), or a Ref, none deep, to an item of a list of the class ww_ki's
// items; the field (ww_slot2 & 0xFFFFFFFF) bytes into it. Anything else the VM's.
INLINE const uint8_t* valueField(const Slot& s, const Ctx* ctx) {
    // Its kind, how deep, where in the item and the index or type number, in one word.
    const uint64_t head = word<uint64_t>(reinterpret_cast<const uint8_t*>(&s));
    const uint32_t offset = static_cast<uint32_t>(ww_slot2);
    if ((head & 0xFF) == kKindSmall) {
        if ((head >> 32) != static_cast<uint64_t>(ww_slot2) >> 32) return nullptr;
        return reinterpret_cast<const uint8_t*>(&s.i) + offset;
    }
    if ((head & 0xFFFFFFFF) != kKindRef) return nullptr;  // a Ref none deep, at its item's start
    const uint8_t* o = s.p;
    if (o[kObjType] != kTypeList || !mayRead(o, ctx)) return nullptr;
    const uint8_t* pack = packOf(o, kStoreItems);
    if (pack == nullptr || word<int64_t>(pack + kPackItemClass) != ww_ki) return nullptr;
    const uint64_t index = head >> 32;
    if (index >= word<uint64_t>(pack + kPackCount)) return nullptr;
    return word<const uint8_t*>(pack + kPackBlock) + index * word<uint32_t>(pack + kPackWidth) + offset;
}
// i9 = whether the values in slots ww_slot and ww_slot2 are the same, byte for byte (2026-11-14:
// a value type whose == is its bytes', regions.cpp's bytesCompared): each a Small value of the
// type numbered (ww_kf's bits >> 32), or a Ref, none deep, to an item of a list of the class
// ww_ki's items; (ww_kf's bits & 0xFF) bytes. Anything else the VM's.
INLINE const uint8_t* valueBytes(const Slot& s, const Ctx* ctx, uint64_t id) {
    const uint64_t head = word<uint64_t>(reinterpret_cast<const uint8_t*>(&s));
    if ((head & 0xFF) == kKindSmall) return (head >> 32) == id ? reinterpret_cast<const uint8_t*>(&s.i) : nullptr;
    if ((head & 0xFFFFFFFF) != kKindRef) return nullptr;
    const uint8_t* o = s.p;
    if (o[kObjType] != kTypeList || !mayRead(o, ctx)) return nullptr;
    const uint8_t* pack = packOf(o, kStoreItems);
    if (pack == nullptr || word<int64_t>(pack + kPackItemClass) != ww_ki) return nullptr;
    const uint64_t index = head >> 32;
    if (index >= word<uint64_t>(pack + kPackCount)) return nullptr;
    return word<const uint8_t*>(pack + kPackBlock) + index * word<uint32_t>(pack + kPackWidth);
}
STENCIL(eqsm) {
    const uint64_t what = word<uint64_t>(reinterpret_cast<const uint8_t*>(&ww_kf));
    const uint8_t* x = valueBytes((*slotAt(base)), ctx, what >> 32);
    const uint8_t* y = valueBytes((*slot2At(base)), ctx, what >> 32);
    if (x == nullptr || y == nullptr) EXIT;
    bool same = true;
    for (uint64_t k = 0; k < (what & 0xFF); k++) same &= x[k] == y[k];
    i9 = same;
    NEXT;
}
#define VALP(a, ...)                                                            \
    STENCIL(valpb_##a) {                                                        \
        const uint8_t* at = valueField((*slotAt(base)), ctx);                   \
        if (at == nullptr) EXIT;                                                \
        i##a = *at;                                                             \
        NEXT;                                                                   \
    }                                                                           \
    STENCIL(valpi_##a) {                                                        \
        const uint8_t* at = valueField((*slotAt(base)), ctx);                   \
        if (at == nullptr) EXIT;                                                \
        i##a = word<int64_t>(at);                                               \
        NEXT;                                                                   \
    }                                                                           \
    STENCIL(valph_##a) {                                                        \
        const uint8_t* at = valueField((*slotAt(base)), ctx);                   \
        if (at == nullptr) EXIT;                                                \
        i##a = word<uint16_t>(at);                                              \
        NEXT;                                                                   \
    }                                                                           \
    STENCIL(valpw_##a) {                                                        \
        const uint8_t* at = valueField((*slotAt(base)), ctx);                   \
        if (at == nullptr) EXIT;                                                \
        i##a = word<uint32_t>(at);                                              \
        NEXT;                                                                   \
    }
I_A(VALP, x)
#define VALPF(a, ...)                                                           \
    STENCIL(valpf_##a) {                                                        \
        const uint8_t* at = valueField((*slotAt(base)), ctx);                   \
        if (at == nullptr) EXIT;                                                \
        f##a = word<double>(at);                                                \
        NEXT;                                                                   \
    }
F_W(VALPF, x)

// ---- Slots, and going on ----

// A register read from its slot, as a region begins; written back as it leaves (kind and
// payload in one store, as the VM writes a slot): an int, a byte, a float, the flag register's
// bool; a whole number read from a list or bytes (storew: a byte where the object in ww_slot2
// is bytes, an int otherwise); a slot copied whole from another (copyslot: what a pushed object
// stands for).
#define ILOAD(a, ...)                                                           \
    STENCIL(loadi_##a) {                                                        \
        __asm__("movq ww_slotat+8(%1), %0" : "=r"(i##a) : "r"(base) : "memory"); \
        NEXT;                                                                   \
    }                                                                           \
    STENCIL(storei_##a) {                                                       \
        storeSlotAt(base, kKindInt, static_cast<uint64_t>(i##a));      \
        NEXT;                                                                   \
    }                                                                           \
    STENCIL(storeby_##a) {                                                      \
        storeSlotAt(base, kKindByte, static_cast<uint64_t>(i##a));     \
        NEXT;                                                                   \
    }                                                                           \
    STENCIL(storew_##a) {                                                       \
        const Slot& o = (*slot2At(base));                                       \
        const bool bytes = o.kind == kKindObj && o.p[kObjType] == kTypeBytes;   \
        storeSlotAt(base, bytes ? kKindByte : kKindInt, static_cast<uint64_t>(i##a)); \
        NEXT;                                                                   \
    }
I_A(ILOAD, x)
#define FLOAD(a, ...)                                                           \
    STENCIL(loadf_##a) {                                                        \
        __asm__(FLOADOP_##a " ww_slotat+8(%1), %0" : "+x"(FVEC_##a) : "r"(base) : "memory"); \
        NEXT;                                                                   \
    }                                                                           \
    STENCIL(storef_##a) {                                                       \
        uint64_t bits;                                                          \
        { const double d_ = f##a; __builtin_memcpy(&bits, &d_, 8); }                                           \
        storeSlotAt(base, kKindFloat, bits);                           \
        NEXT;                                                                   \
    }
F_W(FLOAD, x)
STENCIL(storeb) {
    storeSlotAt(base, kKindBool, i9 != 0);
    NEXT;
}
STENCIL(copyslot) {
    __builtin_memcpy(&(*slotAt(base)), &(*slot2At(base)), sizeof(Slot));
    NEXT;
}

// The next step run once more (WW_PROFILE_NATIVE, 2026-11-15: regions.cpp puts one before each
// step): one added to the count at the address ww_ki holds. Not atomic: where two threads run
// one region at once, a count may come out a little low.
STENCIL(count) {
    ++*reinterpret_cast<uint64_t*>(ww_ki);
    NEXT;
}
// A jump; a look at the VM's flag; a loop's end (the flag looked at, then back to its start);
// the region's end, giving where the VM goes on.
STENCIL(jump) { JUMP; }
STENCIL(safepoint) {
    if (*ctx->attention != 0) EXIT;
    NEXT;
}
STENCIL(loopback) {
    if (*ctx->attention != 0) EXIT;
    JUMP;
}
STENCIL(ret) { return ww_ret; }

// A slot the region has not written, checked to hold what an untyped instruction is taken to
// read in it: a whole number, or a float. Where it does not, the VM does the instruction.
STENCIL(chki) {
    const uint8_t k = (*slotAt(base)).kind;
    if (k != kKindInt && k != kKindByte) EXIT;
    NEXT;
}
STENCIL(chkf) {
    if ((*slotAt(base)).kind != kKindFloat) EXIT;
    NEXT;
}

// A range's count, as rangeCount gives it for a step of 1 or -1 (ascending or descending,
// without or with its end): ic = how many from is to ic, at most INT64_MAX. And ia = ia + K
// wrapping round (a range's next value, by a step known).
#define RCNT_B(s, c)                                                            \
    STENCIL(rcntae_##c##_##s) {                                                 \
        const uint64_t n = i##c < i##s ? 0 : static_cast<uint64_t>(i##c) - static_cast<uint64_t>(i##s); \
        i##c = n > static_cast<uint64_t>(INT64_MAX) ? INT64_MAX : static_cast<int64_t>(n); \
        NEXT;                                                                   \
    }                                                                           \
    STENCIL(rcntai_##c##_##s) {                                                 \
        const uint64_t n = i##c < i##s ? 0 : static_cast<uint64_t>(i##c) - static_cast<uint64_t>(i##s) + 1; \
        i##c = n > static_cast<uint64_t>(INT64_MAX) ? INT64_MAX : static_cast<int64_t>(n); \
        NEXT;                                                                   \
    }                                                                           \
    STENCIL(rcntde_##c##_##s) {                                                 \
        const uint64_t n = i##c > i##s ? 0 : static_cast<uint64_t>(i##s) - static_cast<uint64_t>(i##c); \
        i##c = n > static_cast<uint64_t>(INT64_MAX) ? INT64_MAX : static_cast<int64_t>(n); \
        NEXT;                                                                   \
    }                                                                           \
    STENCIL(rcntdi_##c##_##s) {                                                 \
        const uint64_t n = i##c > i##s ? 0 : static_cast<uint64_t>(i##s) - static_cast<uint64_t>(i##c) + 1; \
        i##c = n > static_cast<uint64_t>(INT64_MAX) ? INT64_MAX : static_cast<int64_t>(n); \
        NEXT;                                                                   \
    }
#define RCNT_A(c, ...) I_B(RCNT_B, c)
I_A(RCNT_A, x)
#define IADDWK(a, ...)                                                          \
    STENCIL(addwik_##a) {                                                       \
        i##a = static_cast<int64_t>(static_cast<uint64_t>(i##a) + static_cast<uint64_t>(ww_ki)); \
        NEXT;                                                                   \
    }
I_A(IADDWK, x)
// A slot checked to hold an int (not a byte): what the VM's range asks of its bounds.
STENCIL(chkint) {
    if ((*slotAt(base)).kind != kKindInt) EXIT;
    NEXT;
}
// The object a whole number was read from checked to be no bytes: the number an int, not a byte.
STENCIL(chklist) {
    const Slot& o = (*slotAt(base));
    if (o.kind == kKindObj && o.p[kObjType] == kTypeBytes) EXIT;
    NEXT;
}

// fa = a number read from a slot, an int made a float: the other operand of a float's
// arithmetic, whatever it is (an int beside a float makes a float); anything else the VM's.
#define NUMF(a, ...)                                                            \
    STENCIL(numf_##a) {                                                         \
        const Slot& v = (*slotAt(base));                                        \
        if (v.kind == kKindFloat) f##a = v.d;                                   \
        else if (v.kind == kKindInt) f##a = static_cast<double>(v.i);           \
        else EXIT;                                                              \
        NEXT;                                                                   \
    }
F_W(NUMF, x)
// A slot checked to hold a byte (what the region takes a slot entered as a byte to hold).
STENCIL(chkbyk) {
    if ((*slotAt(base)).kind != kKindByte) EXIT;
    NEXT;
}

// ---- Fields ----

// A field of the object of a class in slot ww_slot, where the cache word at ww_ki (an
// address: the place's cache, which the VM fills) says it is, its layout the one the cache
// saw; read where the object's core may read it (or change it, `change`: its own core's).
// Null where not: the VM's.
INLINE uint8_t* fieldAt(const Slot& s, const Ctx* ctx, bool change, uint64_t& seen) {
    if (s.kind != kKindObj || s.p[kObjType] != kTypeInstance) return nullptr;
    seen = *reinterpret_cast<const volatile uint64_t*>(ww_ki);
    if (static_cast<uint32_t>(seen >> 32) != word<uint32_t>(s.p + kInstShape)) return nullptr;
    const uint8_t core = s.p[kObjCore];
    if (change ? core != ctx->core : (core != 0 && core != ctx->core)) return nullptr;
    return const_cast<uint8_t*>(s.p) + ((seen >> 16) & 0xFFFF);
}
// A field read as a number of kind `kind`: kept as its payload alone, or as a value.
INLINE bool numberField(const uint8_t* at, uint64_t seen, uint8_t kind, uint64_t& bits) {
    if ((seen & 0x80) != 0) {
        if ((seen & 3) != kind) return false;
    } else {
        if (at[0] != kind) return false;
        at += 8;
    }
    __builtin_memcpy(&bits, at, 8);
    return true;
}
// A number written to a field, as the VM's SetProp does at once: kept as its payload alone (its
// kind the field's, the field not one set only once and set already), or as a value of a field
// declared with no type or that number's type; anything else the VM's.
INLINE bool setNumberField(const Slot& s, const Ctx* ctx, uint8_t kind, uint8_t typeId, uint64_t bits) {
    uint64_t seen;
    uint8_t* at = fieldAt(s, ctx, true, seen);
    if (at == nullptr) return false;
    if ((seen & 0x80) != 0) {
        if ((seen & 3) != kind) return false;
        const uint32_t bit = (seen >> 8) & 0xFF;
        uint8_t* data = const_cast<uint8_t*>(s.p) + kInstData;
        const bool hasOne = (seen & 0x20) != 0 && (data[bit / 8] >> (bit % 8) & 1) != 0;
        if ((seen & 0x40) != 0 && hasOne) return false;
        __builtin_memcpy(at, &bits, 8);
        if ((seen & 0x20) != 0) data[bit / 8] |= static_cast<uint8_t>(1u << (bit % 8));
        return true;
    }
    if ((seen & 2) != 0 || ((seen & 1) != 0 && at[0] != kKindUndefined)) return false;
    const uint8_t type = static_cast<uint8_t>(seen >> 8);
    if (type != 0 && type != typeId) return false;
    Slot v;
    __builtin_memset(&v, 0, sizeof v);
    v.kind = kind;
    __builtin_memcpy(&v.i, &bits, 8);
    __builtin_memcpy(at, &v, sizeof v);
    return true;
}
#define GETPF(a, ...)                                                           \
    STENCIL(getpf_##a) {                                                        \
        uint64_t seen, bits;                                                    \
        const uint8_t* at = fieldAt((*slotAt(base)), ctx, false, seen);         \
        if (at == nullptr || !numberField(at, seen, kKindFloat, bits)) EXIT;    \
        { double d_; __builtin_memcpy(&d_, &bits, 8); f##a = d_; }                                           \
        NEXT;                                                                   \
    }                                                                           \
    STENCIL(setpf_##a) {                                                        \
        uint64_t bits;                                                          \
        { const double d_ = f##a; __builtin_memcpy(&bits, &d_, 8); }                                           \
        if (!setNumberField((*slotAt(base)), ctx, kKindFloat, kTypeIdFloat, bits)) EXIT; \
        NEXT;                                                                   \
    }
F_W(GETPF, x)
#define GETPI(a, ...)                                                           \
    STENCIL(getpi_##a) {                                                        \
        uint64_t seen, bits;                                                    \
        const uint8_t* at = fieldAt((*slotAt(base)), ctx, false, seen);         \
        if (at == nullptr || !numberField(at, seen, kKindInt, bits)) EXIT;      \
        i##a = static_cast<int64_t>(bits);                                      \
        NEXT;                                                                   \
    }                                                                           \
    STENCIL(setpi_##a) {                                                        \
        if (!setNumberField((*slotAt(base)), ctx, kKindInt, kTypeIdInt, static_cast<uint64_t>(i##a))) EXIT; \
        NEXT;                                                                   \
    }
I_A(GETPI, x)
// ia = the size of the list or bytes in slot ww_slot, as `size` gives it (a while loop's
// `i < items.size`); anything else the VM's.
INLINE bool sizeOf(const Slot& s, const Ctx* ctx, int64_t& out) {
    if (s.kind != kKindObj) return false;
    const uint8_t* o = s.p;
    if (!mayRead(o, ctx)) return false;
    if (o[kObjType] == kTypeBytes) {
        out = word<const uint8_t*>(o + kBytesData + 8) - word<const uint8_t*>(o + kBytesData);
        return true;
    }
    if (o[kObjType] != kTypeList) return false;
    const uint8_t* pack = word<const uint8_t*>(o + kListPack);
    if (pack != nullptr) {
        out = static_cast<int64_t>(word<uint64_t>(pack + kPackCount));
        return true;
    }
    out = (word<const uint8_t*>(o + kListItems + 8) - word<const uint8_t*>(o + kListItems)) / kValueSize;
    return true;
}
#define SIZEW(a, ...)                                                           \
    STENCIL(sizew_##a) {                                                        \
        int64_t v;                                                              \
        if (!sizeOf((*slotAt(base)), ctx, v)) EXIT;                             \
        i##a = v;                                                               \
        NEXT;                                                                   \
    }
I_A(SIZEW, x)

// A field of any kind copied to slot ww_slot2, as the VM copies it (no value held elsewhere).
STENCIL(getpv) {
    uint64_t seen;
    const uint8_t* at = fieldAt((*slotAt(base)), ctx, false, seen);
    if (at == nullptr) EXIT;
    if ((seen & 0x80) != 0) {
        uint64_t bits;
        __builtin_memcpy(&bits, at, 8);
        storeSlot2At(base, static_cast<uint8_t>(seen & 3), bits);
        NEXT;
    }
    if (at[0] == kKindRef || at[0] == kKindUndefined) EXIT;
    __builtin_memcpy(&(*slot2At(base)), at, sizeof(Slot));
    NEXT;
}

// ---- A for over a list ----

// The iterator in slot ww_slot over a list: the iterator, and the list in `list`.
INLINE uint8_t* listIterator(const Slot& s, const uint8_t*& list) {
    if (s.kind != kKindObj || s.p[kObjType] != kTypeIterator) return nullptr;
    const Slot& source = *reinterpret_cast<const Slot*>(s.p + kIterSource);
    if (source.kind != kKindObj || source.p[kObjType] != kTypeList) return nullptr;
    list = source.p;
    return const_cast<uint8_t*>(s.p);
}
// The next item of a list kept as values, to slot ww_slot2; to the jump's target where there
// are no more.
STENCIL(iterv) {
    const uint8_t* list;
    uint8_t* it = listIterator((*slotAt(base)), list);
    if (it == nullptr || word<const uint8_t*>(list + kListPack) != nullptr) EXIT;
    const Slot* first = word<const Slot*>(list + kListItems);
    const Slot* last = word<const Slot*>(list + kListItems + 8);
    const uint64_t index = word<uint64_t>(it + kIterIndex);
    if (index >= static_cast<uint64_t>(last - first)) JUMP;
    __builtin_memcpy(&(*slot2At(base)), first + index, sizeof(Slot));
    const uint64_t next = index + 1;
    __builtin_memcpy(it + kIterIndex, &next, 8);
    NEXT;
}
// The next item of a packed list of floats (ints), none null, into fa (ia), where the list is
// not the one lent to a parallel section's core.
INLINE const uint8_t* packedIterator(const Slot& s, uint8_t store, const Ctx* ctx, uint8_t*& it) {
    const uint8_t* list;
    it = listIterator(s, list);
    if (it == nullptr || list == ctx->band) return nullptr;
    return packOf(list, store);
}
#define ITERF(a, ...)                                                           \
    STENCIL(iterf_##a) {                                                        \
        uint8_t* it;                                                            \
        const uint8_t* pack = packedIterator((*slotAt(base)), kStoreFloats, ctx, it); \
        if (pack == nullptr) EXIT;                                              \
        const uint64_t index = word<uint64_t>(it + kIterIndex);                 \
        if (index >= word<uint64_t>(pack + kPackCount)) JUMP;                   \
        f##a = word<double>(word<const uint8_t*>(pack + kPackBlock) + index * 8); \
        const uint64_t next = index + 1;                                        \
        __builtin_memcpy(it + kIterIndex, &next, 8);                                 \
        NEXT;                                                                   \
    }
F_W(ITERF, x)
#define ITERI(a, ...)                                                           \
    STENCIL(iteri_##a) {                                                        \
        uint8_t* it;                                                            \
        const uint8_t* pack = packedIterator((*slotAt(base)), kStoreInts, ctx, it); \
        if (pack == nullptr) EXIT;                                              \
        const uint64_t index = word<uint64_t>(it + kIterIndex);                 \
        if (index >= word<uint64_t>(pack + kPackCount)) JUMP;                   \
        i##a = word<int64_t>(word<const uint8_t*>(pack + kPackBlock) + index * 8); \
        const uint64_t next = index + 1;                                        \
        __builtin_memcpy(it + kIterIndex, &next, 8);                                 \
        NEXT;                                                                   \
    }
I_A(ITERI, x)
// The next item of a list of a value type's items, none null, where it is: a Ref to it in slot
// ww_slot2, as IterNextRef gives it (M9.5 B1).
STENCIL(iterref) {
    const uint8_t* list;
    uint8_t* it = listIterator((*slotAt(base)), list);
    if (it == nullptr || list == ctx->band || packOf(list, kStoreItems) == nullptr) EXIT;
    const uint64_t index = word<uint64_t>(it + kIterIndex);
    if (index >= word<uint64_t>(word<const uint8_t*>(list + kListPack) + kPackCount)) JUMP;
    putValue(&(*slot2At(base)), kKindRef | index << 32, reinterpret_cast<uint64_t>(list));
    const uint64_t next = index + 1;
    __builtin_memcpy(it + kIterIndex, &next, 8);
    NEXT;
}
// The next item of a list of the class ww_ki's items kept in the value itself (a Color), none
// null and none with a type of its own: a copy of its first (ww_slot2 & 0xFF) bytes, of the
// type numbered (ww_slot2 >> 32), to slot (ww_slot2 >> 8 & 0xFFFFFF) (M9.5 B1).
STENCIL(itersm) {
    uint8_t* it;
    const uint8_t* pack = packedIterator((*slotAt(base)), kStoreItems, ctx, it);
    if (pack == nullptr || word<int64_t>(pack + kPackItemClass) != ww_ki || word<const void*>(pack + kPackItemType) != nullptr ||
        word<uint32_t>(pack + kPackTypeSlot) != 0)
        EXIT;
    const uint64_t index = word<uint64_t>(it + kIterIndex);
    if (index >= word<uint64_t>(pack + kPackCount)) JUMP;
    const uint64_t what = static_cast<uint64_t>(ww_slot2);
    const uint8_t* from = word<const uint8_t*>(pack + kPackBlock) + index * word<uint32_t>(pack + kPackWidth);
    uint64_t bits = 0;
    for (uint32_t k = 0; k < (what & 0xFF); k++) bits |= static_cast<uint64_t>(from[k]) << (8 * k);
    putValue(&base[(what >> 8) & 0xFFFFFF], kKindSmall | (what >> 32) << 32, bits);
    const uint64_t next = index + 1;
    __builtin_memcpy(it + kIterIndex, &next, 8);
    NEXT;
}

// null, true or false pushed: written to slot ww_slot as the VM writes them.
STENCIL(setnull) {
    storeSlotAt(base, 0, 0);
    NEXT;
}
STENCIL(settrue) {
    storeSlotAt(base, kKindBool, 1);
    NEXT;
}
STENCIL(setfalse) {
    storeSlotAt(base, kKindBool, 0);
    NEXT;
}

// ---- Lists looked at once, as a region starts ----

// A list's or bytes' items, looked at once where nothing in the region can change where they are
// or how many (no instruction the VM's in it; and the region is entered afresh after each way
// out): in the context's words 4k to 4k+3, its items' address, how many may be read, how many
// changed (0 where the object may not be, or is not what the region reads as), and what they are
// (1 ints, 2 floats, 3 bytes: a Bytes, or a List<byte>, whose items are kept as a Bytes keeps
// them, 2026-11-13). slot: the object; ww_ki: k.
INLINE void lookAt(const Slot& s, Ctx* ctx, int64_t k, bool floats) {
    uint64_t* h = ctx->lists + 4 * k;
    h[0] = h[1] = h[2] = h[3] = 0;
    if (s.kind != kKindObj) return;
    const uint8_t* o = s.p;
    if (o[kObjType] == kTypeList) {
        const uint8_t* pack = packOf(o, floats ? kStoreFloats : kStoreInts);
        if (pack == nullptr && !floats) {
            const uint8_t* narrow = packOf(o, kStoreNarrow);
            if (narrow != nullptr && narrow[kPackNarrow] == kNarrowByte) {
                const uint64_t count = word<uint64_t>(narrow + kPackCount);
                h[0] = word<uint64_t>(narrow + kPackBlock);
                if (mayRead(o, ctx)) h[1] = count;
                if (o[kObjCore] == ctx->core && o[kObjFrozen] == 0) h[2] = count;
                h[3] = 3;
            }
            return;
        }
        if (pack == nullptr) return;
        const uint64_t count = word<uint64_t>(pack + kPackCount);
        h[0] = word<uint64_t>(pack + kPackBlock);
        if (mayRead(o, ctx)) h[1] = count;
        if (o[kObjCore] == ctx->core && o[kObjFrozen] == 0) h[2] = count;
        h[3] = floats ? 2 : 1;
    } else if (!floats && o[kObjType] == kTypeBytes) {
        const uint8_t* first = word<const uint8_t*>(o + kBytesData);
        const uint8_t* end = word<const uint8_t*>(o + kBytesData + 8);
        const uint64_t count = static_cast<uint64_t>(end - first);
        h[0] = reinterpret_cast<uint64_t>(first);
        if (mayRead(o, ctx)) h[1] = count;
        if (o[kObjCore] == ctx->core) h[2] = count;
        h[3] = 3;
    }
}
STENCIL(lookf) {
    lookAt((*slotAt(base)), ctx, ww_ki, true);
    NEXT;
}
STENCIL(lookw) {
    lookAt((*slotAt(base)), ctx, ww_ki, false);
    NEXT;
}
// A List<bool> looked at once (2026-11-15): its bytes, how many may be read and changed, 4.
STENCIL(lookbool) {
    uint64_t* h = ctx->lists + 4 * ww_ki;
    h[0] = h[1] = h[2] = h[3] = 0;
    const Slot& s = (*slotAt(base));
    if (s.kind == kKindObj && s.p[kObjType] == kTypeList) {
        const uint8_t* o = s.p;
        if (const uint8_t* pack = packOf(o, kStoreBools)) {
            const uint64_t count = word<uint64_t>(pack + kPackCount);
            h[0] = word<uint64_t>(pack + kPackBlock);
            if (mayRead(o, ctx)) h[1] = count;
            if (o[kObjCore] == ctx->core && o[kObjFrozen] == 0) h[2] = count;
            h[3] = 4;
        }
    }
    NEXT;
}
// Items read and written through what was looked at (k in ww_slot): the index checked
// against the count, nothing else.
#define HIDXF_B(c, a)                                                           \
    STENCIL(hidxf_##a##_##c) {                                                  \
        const uint64_t* h = ctx->lists + 4 * ww_slot;                         \
        if (static_cast<uint64_t>(i##c) >= h[1]) EXIT;                          \
        f##a = reinterpret_cast<const double*>(h[0])[i##c];                     \
        NEXT;                                                                   \
    }                                                                           \
    STENCIL(hsetf_##a##_##c) {                                                  \
        const uint64_t* h = ctx->lists + 4 * ww_slot;                         \
        if (static_cast<uint64_t>(i##c) >= h[2]) EXIT;                          \
        reinterpret_cast<double*>(h[0])[i##c] = f##a;                           \
        NEXT;                                                                   \
    }
#define HIDXF_A(a, ...) I_B(HIDXF_B, a)
F_W(HIDXF_A, x)
#define HIDXFK(a, ...)                                                          \
    STENCIL(hidxfk_##a) {                                                       \
        const uint64_t* h = ctx->lists + 4 * ww_slot;                         \
        if (static_cast<uint64_t>(ww_ki) >= h[1]) EXIT;                       \
        f##a = reinterpret_cast<const double*>(h[0])[ww_ki];                  \
        NEXT;                                                                   \
    }
F_W(HIDXFK, x)
#define HIDXW_B(c, a)                                                           \
    STENCIL(hidxw_##a##_##c) {                                                  \
        const uint64_t* h = ctx->lists + 4 * ww_slot;                         \
        if (static_cast<uint64_t>(i##c) >= h[1]) EXIT;                          \
        i##a = h[3] == 3 ? reinterpret_cast<const uint8_t*>(h[0])[i##c] : reinterpret_cast<const int64_t*>(h[0])[i##c]; \
        NEXT;                                                                   \
    }                                                                           \
    STENCIL(hsetwi_##a##_##c) {                                                 \
        const uint64_t* h = ctx->lists + 4 * ww_slot;                         \
        if (static_cast<uint64_t>(i##c) >= h[2]) EXIT;                          \
        if (h[3] == 3) {                                                        \
            if (static_cast<uint64_t>(i##a) > 255) EXIT;                        \
            reinterpret_cast<uint8_t*>(h[0])[i##c] = static_cast<uint8_t>(i##a); \
        } else {                                                                \
            reinterpret_cast<int64_t*>(h[0])[i##c] = i##a;                      \
        }                                                                       \
        NEXT;                                                                   \
    }                                                                           \
    STENCIL(hsetwb_##a##_##c) {                                                 \
        const uint64_t* h = ctx->lists + 4 * ww_slot;                         \
        if (static_cast<uint64_t>(i##c) >= h[2]) EXIT;                          \
        if (h[3] == 3) reinterpret_cast<uint8_t*>(h[0])[i##c] = static_cast<uint8_t>(i##a); \
        else reinterpret_cast<int64_t*>(h[0])[i##c] = i##a; /* a byte into ints: the int it is */ \
        NEXT;                                                                   \
    }
#define HIDXW_A(a, ...) I_B(HIDXW_B, a)
I_A(HIDXW_A, x)
#define HIDXWK(a, ...)                                                          \
    STENCIL(hidxwk_##a) {                                                       \
        const uint64_t* h = ctx->lists + 4 * ww_slot;                         \
        if (static_cast<uint64_t>(ww_ki) >= h[1]) EXIT;                       \
        i##a = h[3] == 3 ? reinterpret_cast<const uint8_t*>(h[0])[ww_ki] : reinterpret_cast<const int64_t*>(h[0])[ww_ki]; \
        NEXT;                                                                   \
    }
I_A(HIDXWK, x)

// A list of a value type's items looked at once (M9.5 B1), as lookAt: in the context's words 4k
// to 4k+3, its items' address, how many may be read, how many changed, and the list itself (what
// a Ref read later must be into). Only a list of the class ww_ki's items, none null, each
// (ww_slot2 >> 32) bytes wide, and with ww_slot2's bit 8, none with a type of its own (items
// copied out or set as the bytes they are); anything else leaves all four 0, and every step
// through them leaves for the VM. How many changed: 0 for the object lent to a parallel section,
// whose parts are the VM's to check. ww_slot: the list; ww_slot2 & 0xFF: k.
INLINE void lookAtItems(const Slot& s, Ctx* ctx) {
    uint64_t* h = ctx->lists + 4 * (static_cast<uint64_t>(ww_slot2) & 0xFF);
    h[0] = h[1] = h[2] = h[3] = 0;
    if (s.kind == kKindObj && s.p[kObjType] == kTypeList) {
        const uint8_t* o = s.p;
        const uint8_t* pack = packOf(o, kStoreItems);
        if (pack != nullptr && word<int64_t>(pack + kPackItemClass) == ww_ki &&
            word<uint32_t>(pack + kPackWidth) == static_cast<uint64_t>(ww_slot2) >> 32 &&
            ((static_cast<uint64_t>(ww_slot2) & 0x100) == 0 ||
             (word<const void*>(pack + kPackItemType) == nullptr && word<uint32_t>(pack + kPackTypeSlot) == 0))) {
            const uint64_t count = word<uint64_t>(pack + kPackCount);
            h[0] = word<uint64_t>(pack + kPackBlock);
            if (mayRead(o, ctx)) h[1] = count;
            if (o[kObjCore] == ctx->core && o[kObjFrozen] == 0 && o != ctx->band) h[2] = count;
            h[3] = reinterpret_cast<uint64_t>(o);
        }
    }
}
// A field looked at once (of an object the region never changes, a field none of its steps
// writes): as getpv reads it, into the context's held words 2k and 2k+1 (k: ww_slot2), from the
// object in slot ww_slot (lookp) or held as ww_slot (lookph), where the place's cache at
// ww_ki says. One it cannot read so is held as Undefined, and each read of it is the VM's.
INLINE const Slot& heldSlot(const Ctx* ctx, int64_t k) { return reinterpret_cast<const Slot*>(ctx->held)[k]; }
INLINE void lookAtField(const Slot& s, Ctx* ctx) {
    Slot* const held = reinterpret_cast<Slot*>(ctx->held);
    storeSlot(held, ww_slot2, kKindUndefined, 0);
    uint64_t seen;
    const uint8_t* at = fieldAt(s, ctx, false, seen);
    if (at == nullptr) return;
    if ((seen & 0x80) != 0) {
        uint64_t bits;
        __builtin_memcpy(&bits, at, 8);
        storeSlot(held, ww_slot2, static_cast<uint8_t>(seen & 3), bits);
        return;
    }
    if (at[0] == kKindRef || at[0] == kKindUndefined) return;
    __builtin_memcpy(&held[ww_slot2], at, sizeof(Slot));
}
STENCIL(lookp) {
    lookAtField((*slotAt(base)), ctx);
    NEXT;
}
STENCIL(lookph) {
    const Slot from = heldSlot(ctx, ww_slot);
    lookAtField(from, ctx);
    NEXT;
}
// A field the region writes, looked at once (lookpw, lookpwh): held as lookp holds it, where it
// may be written back as SET_PROP's setp writes it (the object its core's own; a number kept as
// its payload alone, set any number of times; or kept as a value, an int or a float of a field
// of no type or that number's, not one set only once), and its address in heldAt[k] (bit 0: kept
// as its payload alone). One that may not is held as Undefined, and each read and write of it is
// the VM's.
INLINE void lookAtFieldToChange(const Slot& s, Ctx* ctx) {
    Slot* const held = reinterpret_cast<Slot*>(ctx->held);
    storeSlot(held, ww_slot2, kKindUndefined, 0);
    ctx->heldAt[ww_slot2] = 0;
    uint64_t seen;
    uint8_t* at = fieldAt(s, ctx, true, seen);
    if (at == nullptr) return;
    if ((seen & 0x80) != 0) {
        const uint8_t kind = static_cast<uint8_t>(seen & 3);
        if ((seen & 0x60) != 0 || (kind != kKindInt && kind != kKindFloat)) return;
        uint64_t bits;
        __builtin_memcpy(&bits, at, 8);
        storeSlot(held, ww_slot2, kind, bits);
        ctx->heldAt[ww_slot2] = reinterpret_cast<uint64_t>(at) | 1;
        return;
    }
    if ((seen & 3) != 0) return;
    const uint8_t type = static_cast<uint8_t>(seen >> 8);
    if (at[0] == kKindInt) {
        if (type != 0 && type != kTypeIdInt) return;
    } else if (at[0] == kKindFloat) {
        if (type != 0 && type != kTypeIdFloat) return;
    } else {
        return;
    }
    __builtin_memcpy(&held[ww_slot2], at, sizeof(Slot));
    ctx->heldAt[ww_slot2] = reinterpret_cast<uint64_t>(at);
}
STENCIL(lookpw) {
    lookAtFieldToChange((*slotAt(base)), ctx);
    NEXT;
}
STENCIL(lookpwh) {
    const Slot from = heldSlot(ctx, ww_slot);
    lookAtFieldToChange(from, ctx);
    NEXT;
}
// A field held so written back to its object (a way out): where it may be.
STENCIL(putp) {
    const uint64_t at = ctx->heldAt[ww_slot];
    if (at != 0) {
        const Slot& h = heldSlot(ctx, ww_slot);
        if ((at & 1) != 0) __builtin_memcpy(reinterpret_cast<uint8_t*>(at & ~uint64_t{1}), &h.i, 8);
        else __builtin_memcpy(reinterpret_cast<uint8_t*>(at), &h, sizeof(Slot));
    }
    NEXT;
}
// Writes of a field held so (held as ww_slot): the number it holds, of its kind.
#define HSETPI(a, ...)                                                          \
    STENCIL(hsetpi_##a) {                                                       \
        Slot& h = reinterpret_cast<Slot*>(ctx->held)[ww_slot];                \
        if (h.kind != kKindInt) EXIT;                                           \
        h.i = i##a;                                                             \
        NEXT;                                                                   \
    }
I_A(HSETPI, x)
#define HSETPF(a, ...)                                                          \
    STENCIL(hsetpf_##a) {                                                       \
        Slot& h = reinterpret_cast<Slot*>(ctx->held)[ww_slot];                \
        if (h.kind != kKindFloat) EXIT;                                         \
        h.d = f##a;                                                             \
        NEXT;                                                                   \
    }
F_A(HSETPF, x)
// Reads of a field looked at once (held as ww_slot): its kind checked, nothing else.
#define HGETPI(a, ...)                                                          \
    STENCIL(hgetpi_##a) {                                                       \
        const Slot& h = heldSlot(ctx, ww_slot);                               \
        if (h.kind != kKindInt) EXIT;                                           \
        i##a = h.i;                                                             \
        NEXT;                                                                   \
    }
I_A(HGETPI, x)
#define HGETPF(a, ...)                                                          \
    STENCIL(hgetpf_##a) {                                                       \
        const Slot& h = heldSlot(ctx, ww_slot);                               \
        if (h.kind != kKindFloat) EXIT;                                         \
        f##a = h.d;                                                             \
        NEXT;                                                                   \
    }
F_W(HGETPF, x)
STENCIL(hgetpv) {
    const Slot& h = heldSlot(ctx, ww_slot);
    if (h.kind == kKindUndefined) EXIT;
    __builtin_memcpy(&(*slot2At(base)), &h, sizeof(Slot));
    NEXT;
}
// Lists a field looked at once holds (held as ww_slot), looked at as a slot's are.
STENCIL(lookfh) {
    const Slot from = heldSlot(ctx, ww_slot);
    lookAt(from, ctx, ww_ki, true);
    NEXT;
}
STENCIL(lookwh) {
    const Slot from = heldSlot(ctx, ww_slot);
    lookAt(from, ctx, ww_ki, false);
    NEXT;
}
STENCIL(lookih) {
    const Slot from = heldSlot(ctx, ww_slot);
    lookAtItems(from, ctx);
    NEXT;
}
STENCIL(looki) {
    lookAtItems((*slotAt(base)), ctx);
    NEXT;
}
// The same for the list the iterator in slot ww_slot goes through (a for over it).
STENCIL(lookit) {
    const Slot& s = (*slotAt(base));
    if (s.kind == kKindObj && s.p[kObjType] == kTypeIterator) {
        lookAtItems(*reinterpret_cast<const Slot*>(s.p + kIterSource), ctx);
    } else {
        uint64_t* h = ctx->lists + 4 * (static_cast<uint64_t>(ww_slot2) & 0xFF);
        h[0] = h[1] = h[2] = h[3] = 0;
    }
    NEXT;
}
// Through a list of items looked at once (k in ww_slot): ia (or fa) = item ic's field, the
// field (ww_slot2 & 0xFFFFFFFF) bytes into an item (ww_slot2 >> 32) bytes wide; the index
// checked against the count, nothing else.
#define HIDXP_B(c, a)                                                           \
    STENCIL(hidxpb_##a##_##c) {                                                 \
        const uint64_t* h = ctx->lists + 4 * ww_slot;                         \
        if (static_cast<uint64_t>(i##c) >= h[1]) EXIT;                          \
        i##a = reinterpret_cast<const uint8_t*>(h[0])[i##c * (static_cast<uint64_t>(ww_slot2) >> 32) + static_cast<uint32_t>(ww_slot2)]; \
        NEXT;                                                                   \
    }                                                                           \
    STENCIL(hidxpi_##a##_##c) {                                                 \
        const uint64_t* h = ctx->lists + 4 * ww_slot;                         \
        if (static_cast<uint64_t>(i##c) >= h[1]) EXIT;                          \
        i##a = word<int64_t>(reinterpret_cast<const uint8_t*>(h[0]) + i##c * (static_cast<uint64_t>(ww_slot2) >> 32) + static_cast<uint32_t>(ww_slot2)); \
        NEXT;                                                                   \
    }
#define HIDXP_A(a, ...) I_B(HIDXP_B, a)
I_A(HIDXP_A, x)
// ia = items ic and ic + 1 of a list of items looked at once (k in ww_slot), 4, 2 or 1 bytes
// wide, read as one 8, 4 or 2-byte int, item ic's bytes the low ones (regions.cpp's
// twoItemsAtOnce): both indexes checked against the count.
#define HIDXP2_B(c, a)                                                          \
    STENCIL(hidxp2i_##a##_##c) {                                                \
        const uint64_t* h = ctx->lists + 4 * ww_slot;                         \
        if (static_cast<uint64_t>(i##c) >= h[1] || static_cast<uint64_t>(i##c) + 1 >= h[1]) EXIT; \
        i##a = word<int64_t>(reinterpret_cast<const uint8_t*>(h[0]) + i##c * 4); \
        NEXT;                                                                   \
    }                                                                           \
    STENCIL(hidxp2w_##a##_##c) {                                                \
        const uint64_t* h = ctx->lists + 4 * ww_slot;                         \
        if (static_cast<uint64_t>(i##c) >= h[1] || static_cast<uint64_t>(i##c) + 1 >= h[1]) EXIT; \
        i##a = word<uint32_t>(reinterpret_cast<const uint8_t*>(h[0]) + i##c * 2); \
        NEXT;                                                                   \
    }                                                                           \
    STENCIL(hidxp2h_##a##_##c) {                                                \
        const uint64_t* h = ctx->lists + 4 * ww_slot;                         \
        if (static_cast<uint64_t>(i##c) >= h[1] || static_cast<uint64_t>(i##c) + 1 >= h[1]) EXIT; \
        i##a = word<uint16_t>(reinterpret_cast<const uint8_t*>(h[0]) + i##c); \
        NEXT;                                                                   \
    }
#define HIDXP2_A(a, ...) I_B(HIDXP2_B, a)
I_A(HIDXP2_A, x)
#define HIDXPF_B(c, a)                                                          \
    STENCIL(hidxpf_##a##_##c) {                                                 \
        const uint64_t* h = ctx->lists + 4 * ww_slot;                         \
        if (static_cast<uint64_t>(i##c) >= h[1]) EXIT;                          \
        f##a = word<double>(reinterpret_cast<const uint8_t*>(h[0]) + i##c * (static_cast<uint64_t>(ww_slot2) >> 32) + static_cast<uint32_t>(ww_slot2)); \
        NEXT;                                                                   \
    }
#define HIDXPF_A(a, ...) I_B(HIDXPF_B, a)
F_W(HIDXPF_A, x)
// Slot ww_slot2 = a Ref to item ic of a list of items looked at once (k in ww_slot).
#define HIREF(c, ...)                                                           \
    STENCIL(hiref_##c) {                                                        \
        const uint64_t* h = ctx->lists + 4 * ww_slot;                         \
        if (static_cast<uint64_t>(i##c) >= h[1]) EXIT;                          \
        putValue(&(*slot2At(base)), kKindRef | static_cast<uint64_t>(i##c) << 32, h[3]); \
        NEXT;                                                                   \
    }
I_A(HIREF, x)
// Items copied out, set, and made a place to be set (idxsm, setsm, placei) through a list of
// items looked at once (k in ww_slot), each ww_ki bytes wide: the index checked against the
// count, and a value set its type's; nothing else (the list's class, and that its items have no
// type of their own, checked as it was looked at).
#define HIDXSM(c, ...)                                                          \
    STENCIL(hidxsm_##c) {                                                       \
        const uint64_t* h = ctx->lists + 4 * ww_slot;                         \
        if (static_cast<uint64_t>(i##c) >= h[1]) EXIT;                          \
        const uint64_t what = static_cast<uint64_t>(ww_slot2);                \
        const uint32_t size = static_cast<uint32_t>(what & 0xFF), id = static_cast<uint32_t>(what >> 32); \
        const uint8_t* from = reinterpret_cast<const uint8_t*>(h[0]) + i##c * ww_ki; \
        uint64_t bits = 0;                                                      \
        for (uint32_t k = 0; k < size; k++) bits |= static_cast<uint64_t>(from[k]) << (8 * k); \
        putValue(&base[(what >> 8) & 0xFFFFFF], kKindSmall | static_cast<uint64_t>(id) << 32, bits); \
        NEXT;                                                                   \
    }                                                                           \
    STENCIL(hsetsm_##c) {                                                       \
        const uint64_t* h = ctx->lists + 4 * ww_slot;                         \
        if (static_cast<uint64_t>(i##c) >= h[2]) EXIT;                          \
        const uint64_t what = static_cast<uint64_t>(ww_slot2);                \
        const Slot& v = base[what & 0xFFFFFF];                                  \
        const uint64_t head = word<uint64_t>(reinterpret_cast<const uint8_t*>(&v)); \
        if ((head & 0xFFFFFFFF) == kKindRef && v.p == reinterpret_cast<const uint8_t*>(h[3]) && (head >> 32) == static_cast<uint64_t>(i##c)) NEXT; \
        if ((head & 0xFF) != kKindSmall || (head >> 32) != what >> 32) EXIT;    \
        uint8_t* to = reinterpret_cast<uint8_t*>(h[0]) + i##c * ww_ki;        \
        const uint64_t bits = static_cast<uint64_t>(v.i);                       \
        for (uint32_t k = 0; k < ((what >> 24) & 0xFF); k++) to[k] = static_cast<uint8_t>(bits >> (8 * k)); \
        NEXT;                                                                   \
    }                                                                           \
    STENCIL(hplacei_##c) {                                                      \
        const uint64_t* h = ctx->lists + 4 * ww_slot;                         \
        if (static_cast<uint64_t>(i##c) >= h[2]) EXIT;                          \
        putValue(&(*slot2At(base)), kKindRef | static_cast<uint64_t>(i##c) << 32, h[3]); \
        NEXT;                                                                   \
    }
I_A(HIDXSM, x)
// ia (or fa) = a field of the value in slot ww_slot, as valp: a Small value of the type numbered
// (ww_slot2 >> 32), or a Ref, none deep, to an item of the list looked at once as k (ww_ki);
// the field (ww_slot2 & 0xFFFF) bytes into an item ((ww_slot2 >> 16) & 0xFFFF) bytes wide.
INLINE const uint8_t* lookedField(const Slot& s, const Ctx* ctx) {
    const uint64_t head = word<uint64_t>(reinterpret_cast<const uint8_t*>(&s));
    const uint64_t what = static_cast<uint64_t>(ww_slot2);
    if ((head & 0xFF) == kKindSmall) {
        if ((head >> 32) != what >> 32) return nullptr;
        return reinterpret_cast<const uint8_t*>(&s.i) + (what & 0xFFFF);
    }
    const uint64_t* h = ctx->lists + 4 * ww_ki;
    if ((head & 0xFFFFFFFF) != kKindRef || s.p != reinterpret_cast<const uint8_t*>(h[3]) || (head >> 32) >= h[1]) return nullptr;
    return reinterpret_cast<const uint8_t*>(h[0]) + (head >> 32) * ((what >> 16) & 0xFFFF) + (what & 0xFFFF);
}
#define HVALP(a, ...)                                                           \
    STENCIL(hvalpb_##a) {                                                       \
        const uint8_t* at = lookedField((*slotAt(base)), ctx);                  \
        if (at == nullptr) EXIT;                                                \
        i##a = *at;                                                             \
        NEXT;                                                                   \
    }                                                                           \
    STENCIL(hvalpi_##a) {                                                       \
        const uint8_t* at = lookedField((*slotAt(base)), ctx);                  \
        if (at == nullptr) EXIT;                                                \
        i##a = word<int64_t>(at);                                               \
        NEXT;                                                                   \
    }                                                                           \
    STENCIL(hvalph_##a) {                                                       \
        const uint8_t* at = lookedField((*slotAt(base)), ctx);                  \
        if (at == nullptr) EXIT;                                                \
        i##a = word<uint16_t>(at);                                              \
        NEXT;                                                                   \
    }                                                                           \
    STENCIL(hvalpw_##a) {                                                       \
        const uint8_t* at = lookedField((*slotAt(base)), ctx);                  \
        if (at == nullptr) EXIT;                                                \
        i##a = word<uint32_t>(at);                                              \
        NEXT;                                                                   \
    }
I_A(HVALP, x)
#define HVALPF(a, ...)                                                          \
    STENCIL(hvalpf_##a) {                                                       \
        const uint8_t* at = lookedField((*slotAt(base)), ctx);                  \
        if (at == nullptr) EXIT;                                                \
        f##a = word<double>(at);                                                \
        NEXT;                                                                   \
    }
F_W(HVALPF, x)

// ---- Whole loops made ahead (loop templates) ----

// A loop of the region recognised as one of the shapes below (regions.cpp, Region::makeAhead) is
// run here, every item, before its own steps: its words in the context's t, set by the steps
// before (ctxk, ctxs*): 0 its shape, 1 the counter's next value, 2 how many are left, 3 an
// accumulator, 4 to 6 the lists it writes and reads (k, as looked at once; -1 for none), 7 to 11
// its parameters. Where anything is not as the shape wants (a list of another kind, an index
// out of range, storage shared in part), it does nothing, and the loop's own steps run as they
// would. Where an item fails (an int's overflow, a byte out of range), it stops before that item:
// the loop's own steps do it, and leave for the VM there. Each item is worked out with the same
// operations, in the same order, as the steps would: the same answer to the bit.
enum : uint64_t {
    kLoopFCopy = 1, kLoopFFill, kLoopFOpK, kLoopFKOp, kLoopFOp, kLoopFAffine, kLoopFBlend, kLoopFSum,
    kLoopWCopy, kLoopWFill, kLoopWSum, kLoopWMap, kLoopWLookup, kLoopWMix, kLoopWSumField, kLoopWIterSumField, kLoopWField, kLoopWFillItems, kLoopWMixItems,
    kLoopWCopyItems, kLoopWSameItems, kLoopWAddItems, kLoopFMulAdd, kLoopWToFloats, kLoopWFromFloats,
    kLoopWCountItems, kLoopFGather4, kLoopWBlendNormal,
};
// Parameters: an operator is '+', '-', '*' or '/'. kLoopWMap: v = sign * a + c, then up to two
// of min (1) or max (2) with a constant, then stored (into bytes, from 0 to 255).

INLINE double bitsF(uint64_t b) {
    double d;
    __builtin_memcpy(&d, &b, 8);
    return d;
}
// A list looked at once: its items as doubles, int64s or bytes; its count read and changed.
struct Items {
    uint64_t* h;
    bool ok() const { return h != nullptr; }
    uint64_t kind() const { return h[3]; }
    uint8_t* bytes() const { return reinterpret_cast<uint8_t*>(h[0]); }
    int64_t* ints() const { return reinterpret_cast<int64_t*>(h[0]); }
    double* floats() const { return reinterpret_cast<double*>(h[0]); }
    uint64_t width() const { return h[3] == 3 ? 1 : 8; }
    int64_t whole(int64_t j) const { return h[3] == 3 ? bytes()[j] : ints()[j]; }
};
// The second's storage shared with the first's in part: not the same items, and not apart.
INLINE bool sharedInPart(const Items& a, const Items& b, int64_t end) {
    if (!a.ok() || !b.ok() || a.h[0] == b.h[0]) return false;
    // A list of a value type's items (h[3] the list itself, lookAtItems) keeps a block of its own,
    // never part of another's (2026-11-13: measured as 8 bytes an item, a list of colours seemed to
    // reach into the next one's block, and a template refused wherever the two lay end to end).
    if (a.h[3] > 3 || b.h[3] > 3) return false;
    const uint64_t a0 = a.h[0], a1 = a0 + static_cast<uint64_t>(end) * a.width();
    const uint64_t b0 = b.h[0], b1 = b0 + static_cast<uint64_t>(end) * b.width();
    return a0 < b1 && b0 < a1;
}
// An int's value stored where a whole number goes: into bytes, from 0 to 255 only.
INLINE bool storeWhole(const Items& out, int64_t j, int64_t v) {
    if (out.kind() == 3) {
        if (static_cast<uint64_t>(v) > 255) return false;
        out.bytes()[j] = static_cast<uint8_t>(v);
    } else {
        out.ints()[j] = v;
    }
    return true;
}
INLINE int64_t wholeMap(const Ctx* ctx, int64_t v, bool& fine) {
    const uint64_t* t = ctx->t;
    int64_t r;
    if (__builtin_mul_overflow(v, static_cast<int64_t>(t[7]), &r) || __builtin_add_overflow(r, static_cast<int64_t>(t[8]), &r)) {
        fine = false;
        return 0;
    }
    for (int s = 0; s < 2; s++) {
        const uint64_t op = t[9 + 2 * s];
        const int64_t k = static_cast<int64_t>(t[10 + 2 * s]);
        if (op == 1 && k < r) r = k;
        else if (op == 2 && k > r) r = k;
    }
    return r;
}

INLINE Items itemsOf(Ctx* ctx, int which) {
    const int64_t k = static_cast<int64_t>(ctx->t[4 + which]);
    return Items{k < 0 || k >= 6 ? nullptr : ctx->lists + 4 * k};
}
// Of the kind (0: a whole number's, ints or bytes), with `end` items to be read (changed).
INLINE bool itemsFit(const Items& x, uint64_t kind, int64_t end, int count) {
    return x.ok() && (kind == 0 ? (x.kind() == 1 || x.kind() == 3) : x.kind() == kind) && static_cast<uint64_t>(end) <= x.h[count];
}
#define reads(x, kind) itemsFit(x, kind, end, 1)
#define writes(x, kind) itemsFit(x, kind, end, 2)

INLINE bool small(uint64_t w) { return static_cast<int64_t>(w) > -(1 << 20) && static_cast<int64_t>(w) < (1 << 20); }

// A template's operand (2026-11-13): words d and v, d's low byte its kind: 0 the constant v, 1
// the counter plus v, 2 v itself (a register stored there by the steps before), 3 the int field
// held as v, 4 the bool field held as v (1 or 0); for 2 and 3, d's bits from 16 a constant added.
INLINE bool operand(const Ctx* ctx, uint64_t d, uint64_t v, int64_t counter, int64_t& out) {
    const int64_t plus = static_cast<int64_t>(d) >> 16;
    switch (d & 0xFF) {
        case 0: out = static_cast<int64_t>(v); return true;
        case 1: return !__builtin_add_overflow(counter, static_cast<int64_t>(v), &out);
        case 2: return !__builtin_add_overflow(static_cast<int64_t>(v), plus, &out);
        case 3:
        case 4: {
            if (v >= 16) return false;
            const Slot& h = reinterpret_cast<const Slot*>(ctx->held)[v];
            if ((d & 0xFF) == 4) {
                if (h.kind != kKindBool) return false;
                out = static_cast<uint8_t>(h.i) != 0;
                return true;
            }
            if (h.kind != kKindInt) return false;
            return !__builtin_add_overflow(h.i, plus, &out);
        }
        default: return false;
    }
}
// Held int fields the loop changes each time round (2026-11-13: an image's changed rectangle and
// its count of changes), word 70 how many, each four words from 71: the field, how (1 its min
// with an operand, 2 its max, 3 a constant added), the operand (or the constant). Worked out for
// `rounds` times round from `first` at once: a min of the counter plus a constant takes the
// first, a max the last, the rest the same each time; an add the constant times the rounds.
INLINE bool heldUpdates(Ctx* ctx, int64_t first, int64_t rounds, bool apply) {
    const uint64_t* t = ctx->t;
    if (t[70] > 6) return false;
    Slot* held = reinterpret_cast<Slot*>(ctx->held);
    for (uint64_t u = 0; u < t[70]; u++) {
        const uint64_t* w = t + 71 + 4 * u;
        const uint64_t k = w[0];
        if (k >= 16 || held[k].kind != kKindInt) return false;
        int64_t v = held[k].i;
        if (w[1] == 3) {
            int64_t by;
            if (__builtin_mul_overflow(static_cast<int64_t>(w[3]), rounds, &by) || __builtin_add_overflow(v, by, &v)) return false;
        } else if (w[1] == 1 || w[1] == 2) {
            int64_t o;
            const int64_t at = w[1] == 1 ? first : first + rounds - 1;  // the counter rising: the least first, the most last
            if (!operand(ctx, w[2], w[3], at, o)) return false;
            v = w[1] == 1 ? (o < v ? o : v) : (o > v ? o : v);
        } else {
            return false;
        }
        if (apply) held[k].i = v;
    }
    return true;
}
// Where items of a list looked at once start: base from words 50 to 54 (0 none, 1 an operand, 2
// one times another, each two words), plus the counter.
INLINE bool itemBase(const Ctx* ctx, int64_t& at, int w = 50) {
    const uint64_t* t = ctx->t;
    at = 0;
    if (t[w] == 0) return true;
    if (t[w] != 1 && t[w] != 2) return false;
    if (!operand(ctx, t[w + 1], t[w + 2], 0, at)) return false;
    if (t[w] == 2) {
        int64_t by;
        if (!operand(ctx, t[w + 3], t[w + 4], 0, by) || __builtin_mul_overflow(at, by, &at)) return false;
    }
    return true;
}
// Whether no guard of the loop fires for any counter from `first` to `last` (word 16 how many,
// each five words from 17: its comparison, then its operands). A guard is the condition of a
// way to a throw or a return; each side of it the counter plus a constant or the same each time
// round, so that where it holds at neither end it holds nowhere between.
INLINE bool guardsHold(const Ctx* ctx, int64_t first, int64_t last) {
    const uint64_t* t = ctx->t;
    if (t[16] > 12) return false;
    for (uint64_t g = 0; g < t[16]; g++) {
        const uint64_t* w = t + (g < 6 ? 17 + 5 * g : 192 + 5 * (g - 6));  // the first six from 17, the rest from 192
        const int64_t ends[2] = {first, last};
        for (const int64_t c : ends) {
            int64_t a, b;
            if (!operand(ctx, w[1], w[2], c, a) || !operand(ctx, w[3], w[4], c, b)) return false;
            bool fires;
            switch (w[0]) {
                case 1: fires = a < b; break;
                case 2: fires = a <= b; break;
                case 3: fires = a > b; break;
                case 4: fires = a >= b; break;
                case 5: fires = a == b; break;
                case 6: fires = a != b; break;
                default: return false;
            }
            if (fires) return false;
        }
    }
    return true;
}

// How a mix ends (kLoopWMix, kLoopWMixItems; 2026-11-13): `>> shift`, `/ divisor` (truncating toward
// zero, as WinterWolf's int division does), or both, divided first or shifted first. Where every number
// divided is from 0 to 65535, by a multiply instead (exact there; a division by a number known only
// as the loop runs is slow).
struct MixEnd {
    int32_t shift = 0, divisor = 1;
    bool divideFirst = false, quick = false;
    uint64_t magic = 0;
    __attribute__((always_inline)) int32_t divide(int32_t x) const {
        return quick ? static_cast<int32_t>((static_cast<uint64_t>(static_cast<uint32_t>(x)) * magic) >> 32) : x / divisor;
    }
    __attribute__((always_inline)) int32_t operator()(int32_t v) const {
        if (divisor == 1) return v >> shift;
        return divideFirst ? divide(v) >> shift : divide(v >> shift);
    }
    // Set up for sums from least to most.
    void prepare(int32_t least, int32_t most) {
        const int32_t lo = divideFirst ? least : least >> shift, hi = divideFirst ? most : most >> shift;
        quick = divisor > 1 && divisor < 65536 && lo >= 0 && hi <= 65535;
        magic = quick ? ((uint64_t{1} << 32) + static_cast<uint64_t>(divisor) - 1) / static_cast<uint64_t>(divisor) : 0;
    }
};

// A float mix of bytes of an item (kLoopWMixItems' kinds 0x60 + j; regions.cpp, floatLane): n
// weights, each times a byte of the item as a float, added in order, a constant added last where
// there is one, held between lo and hi, and cut to a whole number: as the steps work it out,
// double by double in the same order (no multiply and add fused), so the same bytes. Words: n |
// a constant << 8; the bytes' offsets; the weights; the constant, lo and hi, as doubles' bits.
struct FloatMix {
    int n = 0;
    bool hasC = false;
    uint64_t at[4] = {};
    double w[4] = {}, c = 0, lo = 0, hi = 0;
    __attribute__((always_inline)) bool read(const uint64_t* q, uint64_t width) {
        n = static_cast<int>(q[0] & 0xFF);
        hasC = ((q[0] >> 8) & 1) != 0;
        if (n < 1 || n > 4) return false;
        for (int k = 0; k < n; k++) {
            at[k] = q[1 + k];
            w[k] = bitsF(q[5 + k]);
            if (at[k] >= width || !__builtin_isfinite(w[k])) return false;
        }
        c = bitsF(q[9]), lo = bitsF(q[10]), hi = bitsF(q[11]);
        // Every sum finite, and anything between lo and hi a byte once cut: then min and max in
        // either order, and the check of the byte, are as the steps have them.
        return __builtin_isfinite(c) && lo > -1.0 && hi < 256.0 && lo <= hi;
    }
    __attribute__((always_inline)) uint8_t held(double s) const {
#pragma clang fp contract(off)
        s = s + c;  // 0 where there is none: s + 0 is s but for -0, which is cut to 0 as well
        const double m = s > lo ? s : lo;
        return static_cast<uint8_t>(static_cast<int64_t>(m < hi ? m : hi));
    }
    __attribute__((always_inline)) uint8_t operator()(const uint8_t* p) const {
#pragma clang fp contract(off)
        double s = w[0] * static_cast<double>(p[at[0]]);
        for (int k = 1; k < n; k++) {
            const double term = w[k] * static_cast<double>(p[at[k]]);
            s = s + term;
        }
        return held(s);
    }
    __attribute__((always_inline)) uint8_t of3(const uint8_t* p) const {
#pragma clang fp contract(off)
        const double t0 = w[0] * static_cast<double>(p[at[0]]), t1 = w[1] * static_cast<double>(p[at[1]]), t2 = w[2] * static_cast<double>(p[at[2]]);
        double s = t0 + t1;
        s = s + t2;
        return held(s);
    }
};

INLINE void runLoop(Slot* base, Ctx* ctx) {
    uint64_t* t = ctx->t;
    // A variable the loop reads and never writes, checked as its steps check it each time round
    // (14: its slot, or all ones; 15: 1 a float, 2 an int, 3 an int or a byte).
    if (t[14] != ~uint64_t{0}) {
        const uint8_t k = base[t[14]].kind;
        if (t[15] == 1 ? k != kKindFloat : t[15] == 2 ? k != kKindInt : (k != kKindInt && k != kKindByte)) return;
    }
    // Objects as the loop entered them, each of exactly a class (112: how many; 113 on: the slot
    // and the global holding the class, two words each), as clsflag checks; where one is not, the
    // loop's own steps leave the region (2026-11-13: an extension's bare call of a global).
    for (uint64_t k = 0; k < t[112]; k++) {
        const Slot& s = base[t[113 + 2 * k]];
        const Slot* g = reinterpret_cast<const Slot*>(ctx->globals) + t[114 + 2 * k];
        if (s.kind != kKindObj || s.p[kObjType] != kTypeInstance || g->kind != kKindObj || word<const uint8_t*>(s.p + kInstClass) != g->p) return;
    }
    // A for over a list of items (word 60 the iterator's slot): from the iterator's index to the
    // list's end, the list the one looked at once as word 5's; the iterator left after the last
    // item done (2026-11-13).
    uint8_t* iterator = nullptr;
    if (t[0] == kLoopWIterSumField) {
        const uint8_t* list;
        iterator = listIterator(base[t[60]], list);
        const Items items = itemsOf(ctx, 1);
        if (iterator == nullptr || list == ctx->band || !items.ok() || reinterpret_cast<const uint8_t*>(items.h[3]) != list) return;
        const uint64_t index = word<uint64_t>(iterator + kIterIndex);
        if (index >= items.h[1]) return;
        t[1] = index;
        t[2] = items.h[1] - index;
    }
    const int64_t next = static_cast<int64_t>(t[1]), left = static_cast<int64_t>(t[2]);
    if (next < 0 || left <= 0 || next > INT64_MAX - left) return;
    const int64_t end = next + left;
    if (!guardsHold(ctx, next, end - 1)) return;
    if (t[70] != 0 && !heldUpdates(ctx, next, left, false)) return;
    const Items out = itemsOf(ctx, 0), a = itemsOf(ctx, 1), b = itemsOf(ctx, 2);
    if (t[13] != 0 && (!out.ok() || out.kind() != 3)) return;  // a byte's value: into bytes alone
    if (sharedInPart(out, a, end) || sharedInPart(out, b, end)) return;
    const double c1 = bitsF(t[7]), c2 = bitsF(t[8]);
    const uint64_t op = t[9];
    int64_t done = left;
    switch (t[0]) {
        case kLoopFCopy: {
            if (!reads(a, 2) || !writes(out, 2)) return;
            double* o = out.floats();
            const double* x = a.floats();
            for (int64_t j = next; j < end; j++) o[j] = x[j];
            break;
        }
        case kLoopFFill: {
            if (!writes(out, 2)) return;
            double* o = out.floats();
            for (int64_t j = next; j < end; j++) o[j] = c1;
            break;
        }
        case kLoopFOpK:
        case kLoopFKOp: {
            if (!reads(a, 2) || !writes(out, 2)) return;
            double* o = out.floats();
            const double* x = a.floats();
            const bool kFirst = t[0] == kLoopFKOp;
            if (op == '+') for (int64_t j = next; j < end; j++) o[j] = kFirst ? c1 + x[j] : x[j] + c1;
            else if (op == '-') for (int64_t j = next; j < end; j++) o[j] = kFirst ? c1 - x[j] : x[j] - c1;
            else if (op == '*') for (int64_t j = next; j < end; j++) o[j] = kFirst ? c1 * x[j] : x[j] * c1;
            else if (op == '/') for (int64_t j = next; j < end; j++) o[j] = kFirst ? c1 / x[j] : x[j] / c1;
            else return;
            break;
        }
        case kLoopFOp: {
            if (!reads(a, 2) || !reads(b, 2) || !writes(out, 2)) return;
            double* o = out.floats();
            const double *x = a.floats(), *y = b.floats();
            if (op == '+') for (int64_t j = next; j < end; j++) o[j] = x[j] + y[j];
            else if (op == '-') for (int64_t j = next; j < end; j++) o[j] = x[j] - y[j];
            else if (op == '*') for (int64_t j = next; j < end; j++) o[j] = x[j] * y[j];
            else if (op == '/') for (int64_t j = next; j < end; j++) o[j] = x[j] / y[j];
            else return;
            break;
        }
        case kLoopFAffine: {  // a * c1 + c2
            if (!reads(a, 2) || !writes(out, 2)) return;
            double* o = out.floats();
            const double* x = a.floats();
            for (int64_t j = next; j < end; j++) {
                const double m = x[j] * c1;
                o[j] = m + c2;
            }
            break;
        }
        case kLoopFBlend: {  // a * c1 + b * c2
            if (!reads(a, 2) || !reads(b, 2) || !writes(out, 2)) return;
            double* o = out.floats();
            const double *x = a.floats(), *y = b.floats();
            for (int64_t j = next; j < end; j++) {
                const double m = x[j] * c1;
                const double n = y[j] * c2;
                o[j] = m + n;
            }
            break;
        }
        case kLoopFSum: {  // in order, as the steps add
            if (!reads(a, 2)) return;
            double acc = bitsF(t[3]);
            const double* x = a.floats();
            for (int64_t j = next; j < end; j++) acc += x[j];
            __builtin_memcpy(&t[3], &acc, 8);
            break;
        }
        case kLoopWCopy: {
            if (!reads(a, 0) || !writes(out, 0)) return;
            if (a.kind() == 3 && out.kind() == 3) {
                uint8_t* o = out.bytes();
                const uint8_t* x = a.bytes();
                for (int64_t j = next; j < end; j++) o[j] = x[j];
            } else if (a.kind() == 1 && out.kind() == 1) {
                int64_t* o = out.ints();
                const int64_t* x = a.ints();
                for (int64_t j = next; j < end; j++) o[j] = x[j];
            } else {
                for (int64_t j = next; j < end; j++)
                    if (!storeWhole(out, j, a.whole(j))) {
                        done = j - next;
                        break;
                    }
            }
            break;
        }
        case kLoopWFill: {
            if (!writes(out, 0)) return;
            const int64_t v = static_cast<int64_t>(t[7]);
            if (out.kind() == 3) {
                if (static_cast<uint64_t>(v) > 255) return;
                uint8_t* o = out.bytes();
                for (int64_t j = next; j < end; j++) o[j] = static_cast<uint8_t>(v);
            } else {
                int64_t* o = out.ints();
                for (int64_t j = next; j < end; j++) o[j] = v;
            }
            break;
        }
        case kLoopWSum: {  // an overflow stops it before the item
            // The items at j * stride + offset (words 7 and 8, 2026-11-13: a stride of 0 is 1), the
            // last of them one that may be read.
            const int64_t stride = t[7] == 0 ? 1 : static_cast<int64_t>(t[7]), offset = static_cast<int64_t>(t[8]);
            int64_t last;
            if (stride < 1 || offset < 0 || __builtin_mul_overflow(end - 1, stride, &last) || __builtin_add_overflow(last, offset, &last)) return;
            if (!a.ok() || (a.kind() != 1 && a.kind() != 3) || static_cast<uint64_t>(last) >= a.h[1]) return;
            int64_t acc = static_cast<int64_t>(t[3]);
            if (a.kind() == 3) {
                const uint8_t* x = a.bytes() + offset;
                if (left <= INT64_MAX / 255 && acc <= INT64_MAX - 255 * left) {
                    // No byte can take the total past the largest int: summed with no check of each,
                    // which Clang does several at a time.
                    int64_t s = 0;
                    if (stride == 1) for (int64_t j = next; j < end; j++) s += x[j];
                    else if (stride == 4) for (int64_t j = next; j < end; j++) s += x[j * 4];
                    else for (int64_t j = next; j < end; j++) s += x[j * stride];
                    acc += s;
                } else {
                    // The sum kept apart until it fits: an overflow leaves the total as it was before
                    // the item, for the loop's own steps to throw at it (2026-11-13: it was left
                    // wrapped round, and the steps went on from there).
                    for (int64_t j = next; j < end; j++) {
                        int64_t s;
                        if (__builtin_add_overflow(acc, static_cast<int64_t>(x[j * stride]), &s)) {
                            done = j - next;
                            break;
                        }
                        acc = s;
                    }
                }
            } else {
                const int64_t* x = a.ints() + offset;
                for (int64_t j = next; j < end; j++) {
                    int64_t s;
                    if (__builtin_add_overflow(acc, x[j * stride], &s)) {
                        done = j - next;
                        break;
                    }
                    acc = s;
                }
            }
            t[3] = static_cast<uint64_t>(acc);
            break;
        }
        case kLoopWMap: {
            if (!reads(a, 0) || !writes(out, 0)) return;
            if (a.kind() == 3 && out.kind() == 3 && small(t[7]) && small(t[8]) && small(t[10]) && small(t[12])) {
                // Bytes to bytes, a block at a time: worked out whole, stored where every item
                // of it fits; the block an item does not fit in done an item at a time, to it.
                const uint8_t* x = a.bytes();
                uint8_t* o = out.bytes();
                // Each clamp a min and a max of constants (a bound of no effect where it has none),
                // so that no branch is left in the block's loop.
                const int32_t sign = static_cast<int32_t>(t[7]), c = static_cast<int32_t>(t[8]);
                int32_t lo[2], hi[2];
                for (int s = 0; s < 2; s++) {
                    const uint64_t op = t[9 + 2 * s];
                    const int32_t k = static_cast<int32_t>(t[10 + 2 * s]);
                    hi[s] = op == 1 ? k : INT32_MAX;
                    lo[s] = op == 2 ? k : INT32_MIN;
                }
                int64_t j = next;
                while (j < end) {
                    const int64_t n = end - j < 64 ? end - j : 64;
                    int32_t v[64];
                    int32_t bad = 0;
                    for (int64_t q = 0; q < n; q++) {
                        int32_t r = sign * static_cast<int32_t>(x[j + q]) + c;
                        r = r < hi[0] ? r : hi[0];
                        r = r > lo[0] ? r : lo[0];
                        r = r < hi[1] ? r : hi[1];
                        r = r > lo[1] ? r : lo[1];
                        v[q] = r;
                        bad |= static_cast<int32_t>(static_cast<uint32_t>(r) > 255u);
                    }
                    if (bad != 0) {
                        int64_t q = 0;
                        while (static_cast<uint32_t>(v[q]) <= 255u) {
                            o[j + q] = static_cast<uint8_t>(v[q]);
                            q++;
                        }
                        done = j + q - next;
                        break;
                    }
                    for (int64_t q = 0; q < n; q++) o[j + q] = static_cast<uint8_t>(v[q]);
                    j += n;
                }
                break;
            }
            for (int64_t j = next; j < end; j++) {
                bool fine = true;
                const int64_t v = wholeMap(ctx, a.whole(j), fine);
                if (!fine || !storeWhole(out, j, v)) {
                    done = j - next;
                    break;
                }
            }
            break;
        }
        case kLoopWLookup: {  // out = table b at a's item
            if (!reads(a, 0) || !b.ok() || (b.kind() != 1 && b.kind() != 3) || !writes(out, 0)) return;
            for (int64_t j = next; j < end; j++) {
                const int64_t at = a.whole(j);
                if (static_cast<uint64_t>(at) >= b.h[1] || !storeWhole(out, j, b.whole(at))) {
                    done = j - next;
                    break;
                }
            }
            break;
        }
        case kLoopWMixItems: {
            // Each item set to a value made of bytes: one mix of byte fields of the item read, a
            // byte field of it, or a constant (regions.cpp, mixItems, says the words from 96).
            if (!a.ok() || !out.ok()) return;
            const Slot* g = reinterpret_cast<const Slot*>(ctx->globals) + t[97];
            if (g->kind != kKindObj || reinterpret_cast<uint64_t>(g->p) != t[98]) return;  // the class mksm checks
            const bool into = (t[96] & 1) != 0;
            const uint64_t size = t[100] & 0xFF, rw = (t[100] >> 8) & 0xFFFFFF, ww = t[100] >> 32;
            const uint64_t constant = t[101], kinds = t[102];
            const int64_t shift = static_cast<int64_t>(t[103] & 0xFF);
            const int nr = static_cast<int>((t[103] >> 8) & 0xFF);
            const int32_t c = static_cast<int32_t>(static_cast<int64_t>(t[104]));
            MixEnd mixEnd;
            mixEnd.shift = static_cast<int32_t>(shift);
            mixEnd.divisor = static_cast<int32_t>(t[109] == 0 ? 1 : t[109]);
            mixEnd.divideFirst = t[110] != 0;
            if (mixEnd.divisor < 1) return;
            if (size == 0 || size > 8 || rw == 0 || ww < size || nr > 4 || shift > 31) return;
            int32_t weight[4] = {0, 0, 0, 0};
            uint64_t from[4] = {0, 0, 0, 0};
            for (int k = 0; k < nr; k++) {
                weight[k] = static_cast<int32_t>(static_cast<int64_t>(t[105 + k]) >> 32);
                from[k] = t[105 + k] & 0xFFFFFF;
                if (from[k] >= rw) return;
            }
            // Float mixes (kinds 0x60 + j, 2026-11-13: greyscale and saturation with float weights):
            // lane j's words from 128 + 16 j.
            const uint64_t floats = t[111];
            if (floats > 4) return;
            FloatMix fm[4];
            for (uint64_t j = 0; j < floats; j++)
                if (!fm[j].read(t + 128 + 16 * j, rw)) return;
            int64_t at = 0, first, last;
            // Read at its base plus the counter; written there too, or (into another list) at
            // that list's own base plus the counter (2026-11-15: words 56 to 60).
            int64_t outAt = 0, oFirst, oLast;
            if (!itemBase(ctx, at)) return;
            if (!into) outAt = at;
            else if (!itemBase(ctx, outAt, 56)) return;
            if (__builtin_add_overflow(at, next, &first) || __builtin_add_overflow(at, end - 1, &last) || first < 0 || static_cast<uint64_t>(last) >= a.h[1] ||
                __builtin_add_overflow(outAt, next, &oFirst) || __builtin_add_overflow(outAt, end - 1, &oLast) || oFirst < 0 ||
                static_cast<uint64_t>(oLast) >= out.h[2])
                return;
            if (into && a.h[0] == out.h[0]) return;  // another list: not the same one
            const uint8_t* x = reinterpret_cast<const uint8_t*>(a.h[0]) + static_cast<uint64_t>(first) * rw;
            uint8_t* o = reinterpret_cast<uint8_t*>(out.h[0]) + static_cast<uint64_t>(oFirst) * ww;
            int32_t least = c, most = c;
            for (int k = 0; k < nr; k++) (weight[k] < 0 ? least : most) += weight[k] * 255;
            mixEnd.prepare(least, most);
            const bool alwaysByte = nr == 0 || (mixEnd(least) >= 0 && mixEnd(most) <= 255);
            // Bytes of tables at bytes of the item (kinds from 0x80: the table's list, the byte's
            // offset): each table bytes with 256 of them at least, so that any byte is a place in it.
            const uint8_t* table[8] = {};
            bool tables = false;
            for (uint64_t lane = 0; lane < size; lane++) {
                const uint64_t kind = (kinds >> (8 * lane)) & 0xFF;
                if (kind < 0x80) continue;
                const uint64_t k = (kind >> 3) & 0xF;
                if (k >= 6 || (kind & 7) >= rw) return;
                const uint64_t* h = ctx->lists + 4 * k;
                if (h[3] != 3 || h[1] < 256) return;
                table[lane] = reinterpret_cast<const uint8_t*>(h[0]);
                tables = true;
            }
            if (tables && size == 4 && rw == 4 && ww == 4 && constant == 0 && ((kinds & 0x80808080u) == 0x80808080u) && (kinds >> 32) == 0) {
                // Every byte a table's: its own loop.
                const uint8_t *t0 = table[0], *t1 = table[1], *t2 = table[2], *t3 = table[3];
                const uint64_t o0 = kinds & 7, o1 = (kinds >> 8) & 7, o2 = (kinds >> 16) & 7, o3 = (kinds >> 24) & 7;
                for (int64_t j = 0; j < left; j++) {
                    const uint8_t* p = x + j * 4;
                    uint8_t* d = o + j * 4;
                    const uint8_t v0 = t0[p[o0]], v1 = t1[p[o1]], v2 = t2[p[o2]], v3 = t3[p[o3]];
                    d[0] = v0;
                    d[1] = v1;
                    d[2] = v2;
                    d[3] = v3;
                }
                break;
            }
            // A greyscale's: three bytes read with weights, the mix three times and the fourth byte kept.
            // The mix three times and the fourth byte the item's own or a constant (a greyscale).
            const uint64_t k0 = kinds & 0xFF, k1 = (kinds >> 8) & 0xFF, k2 = (kinds >> 16) & 0xFF;
            if (floats == 3 && size == 4 && rw == 4 && ww == 4 && (k0 & 0xFC) == 0x60 && (k1 & 0xFC) == 0x60 && (k2 & 0xFC) == 0x60 && (kinds >> 24) == 5 &&
                fm[k0 & 3].n == 3 && fm[k1 & 3].n == 3 && fm[k2 & 3].n == 3) {
                // Three float mixes of three bytes each, the fourth byte the item's own (a colour
                // matrix): its own loop, each mix's words held apart (in registers).
                const FloatMix m0 = fm[k0 & 3], m1 = fm[k1 & 3], m2 = fm[k2 & 3];
                auto rgb = [](const FloatMix& m) { return m.at[0] == 0 && m.at[1] == 1 && m.at[2] == 2; };
                if (rgb(m0) && rgb(m1) && rgb(m2) && m0.c == m1.c && m1.c == m2.c && m0.lo == m1.lo && m1.lo == m2.lo && m0.hi == m1.hi && m1.hi == m2.hi) {
                    // Each lane a sum of the first three bytes, the same constant and bounds (a
                    // colour matrix: greyscale, saturation): each byte made a float once.
#pragma clang fp contract(off)
                    const double a0 = m0.w[0], a1 = m0.w[1], a2 = m0.w[2], b0 = m1.w[0], b1 = m1.w[1], b2 = m1.w[2], c0 = m2.w[0], c1 = m2.w[1], c2 = m2.w[2];
                    const double add = m0.c, lo = m0.lo, hi = m0.hi;
                    auto held = [&](double s) {
                        s = s + add;
                        const double m = s > lo ? s : lo;
                        return static_cast<uint8_t>(static_cast<int64_t>(m < hi ? m : hi));
                    };
                    int64_t j = 0;
                    // Two pixels at a time (2026-11-15), one in each half of a register: each
                    // sum in the same order, held the same way (maxpd and minpd pick as the
                    // comparisons do, NaN included), cut to a whole number in 32 bits, which the
                    // bounds keep it in.
                    if (lo > -2147483648.0 && lo < 2147483647.0 && hi > -2147483648.0 && hi < 2147483647.0) {
                        const __m128d va0 = _mm_set1_pd(a0), va1 = _mm_set1_pd(a1), va2 = _mm_set1_pd(a2), vb0 = _mm_set1_pd(b0), vb1 = _mm_set1_pd(b1),
                                      vb2 = _mm_set1_pd(b2), vc0 = _mm_set1_pd(c0), vc1 = _mm_set1_pd(c1), vc2 = _mm_set1_pd(c2);
                        const __m128d vadd = _mm_set1_pd(add), vlo = _mm_set1_pd(lo), vhi = _mm_set1_pd(hi);
                        const __m128i zero = _mm_setzero_si128(), ones = made128(_mm_cmpeq_epi32(zero, zero));
                        const __m128i low = _mm_srli_epi32(ones, 24), top = _mm_slli_epi32(ones, 24);
#define HELD_TWO(s) _mm_cvttpd_epi32(_mm_min_pd(_mm_max_pd(_mm_add_pd((s), vadd), vlo), vhi))
                        auto bits = [](double d) { return __builtin_bit_cast(uint64_t, d); };
                        if (bits(a0) == bits(b0) && bits(b0) == bits(c0) && bits(a1) == bits(b1) && bits(b1) == bits(c1) && bits(a2) == bits(b2) &&
                            bits(b2) == bits(c2)) {
                            // The three the same sum (a greyscale): worked out once, four pixels at a
                            // time, and the grey written three times.
                            for (; j + 4 <= left; j += 4) {
                                const __m128i px = _mm_loadu_si128(reinterpret_cast<const __m128i*>(x + j * 4));
                                const __m128i wideLo = _mm_unpacklo_epi8(px, zero), wideHi = _mm_unpackhi_epi8(px, zero);
                                const __m128i p0 = _mm_unpacklo_epi16(wideLo, zero), p1 = _mm_unpackhi_epi16(wideLo, zero);
                                const __m128i p2 = _mm_unpacklo_epi16(wideHi, zero), p3 = _mm_unpackhi_epi16(wideHi, zero);
                                const __m128i rg01 = _mm_unpacklo_epi32(p0, p1), ba01 = _mm_unpackhi_epi32(p0, p1);
                                const __m128i rg23 = _mm_unpacklo_epi32(p2, p3), ba23 = _mm_unpackhi_epi32(p2, p3);
                                __m128d s01 = _mm_mul_pd(va0, _mm_cvtepi32_pd(rg01)), s23 = _mm_mul_pd(va0, _mm_cvtepi32_pd(rg23));
                                s01 = _mm_add_pd(s01, _mm_mul_pd(va1, _mm_cvtepi32_pd(_mm_srli_si128(rg01, 8))));
                                s23 = _mm_add_pd(s23, _mm_mul_pd(va1, _mm_cvtepi32_pd(_mm_srli_si128(rg23, 8))));
                                s01 = _mm_add_pd(s01, _mm_mul_pd(va2, _mm_cvtepi32_pd(ba01)));
                                s23 = _mm_add_pd(s23, _mm_mul_pd(va2, _mm_cvtepi32_pd(ba23)));
                                const __m128i v = _mm_and_si128(_mm_unpacklo_epi64(HELD_TWO(s01), HELD_TWO(s23)), low);
                                const __m128i grey = _mm_or_si128(_mm_or_si128(v, _mm_slli_epi32(v, 8)), _mm_slli_epi32(v, 16));
                                _mm_storeu_si128(reinterpret_cast<__m128i*>(o + j * 4), _mm_or_si128(grey, _mm_and_si128(px, top)));
                            }
                        }
                        for (; j + 2 <= left; j += 2) {
                            const __m128i px = _mm_loadl_epi64(reinterpret_cast<const __m128i*>(x + j * 4));
                            const __m128i wide = _mm_unpacklo_epi8(px, zero);
                            const __m128i first = _mm_unpacklo_epi16(wide, zero), second = _mm_unpackhi_epi16(wide, zero);
                            const __m128i rg = _mm_unpacklo_epi32(first, second), ba = _mm_unpackhi_epi32(first, second);
                            const __m128d r = _mm_cvtepi32_pd(rg), g = _mm_cvtepi32_pd(_mm_srli_si128(rg, 8)), b = _mm_cvtepi32_pd(ba);
                            __m128d s0 = _mm_mul_pd(va0, r), s1 = _mm_mul_pd(vb0, r), s2 = _mm_mul_pd(vc0, r);
                            s0 = _mm_add_pd(s0, _mm_mul_pd(va1, g)), s1 = _mm_add_pd(s1, _mm_mul_pd(vb1, g)), s2 = _mm_add_pd(s2, _mm_mul_pd(vc1, g));
                            s0 = _mm_add_pd(s0, _mm_mul_pd(va2, b)), s1 = _mm_add_pd(s1, _mm_mul_pd(vb2, b)), s2 = _mm_add_pd(s2, _mm_mul_pd(vc2, b));
                            const __m128i v0 = _mm_and_si128(HELD_TWO(s0), low), v1 = _mm_and_si128(HELD_TWO(s1), low), v2 = _mm_and_si128(HELD_TWO(s2), low);
                            const __m128i made = _mm_or_si128(_mm_or_si128(v0, _mm_slli_epi32(v1, 8)), _mm_or_si128(_mm_slli_epi32(v2, 16), _mm_and_si128(px, top)));
                            _mm_storel_epi64(reinterpret_cast<__m128i*>(o + j * 4), made);
                        }
                    }
                    for (; j < left; j++) {
                        const uint8_t* p = x + j * 4;
                        uint8_t* d = o + j * 4;
                        const double r = p[0], g = p[1], b = p[2];
                        const uint8_t keep = p[3];
                        double s0 = a0 * r, s1 = b0 * r, s2 = c0 * r;
                        const double t0 = a1 * g, t1 = b1 * g, t2 = c1 * g;
                        s0 = s0 + t0, s1 = s1 + t1, s2 = s2 + t2;
                        const double u0 = a2 * b, u1 = b2 * b, u2 = c2 * b;
                        s0 = s0 + u0, s1 = s1 + u1, s2 = s2 + u2;
                        const uint32_t v = static_cast<uint32_t>(held(s0)) | static_cast<uint32_t>(held(s1)) << 8 | static_cast<uint32_t>(held(s2)) << 16 |
                                           static_cast<uint32_t>(keep) << 24;
                        __builtin_memcpy(d, &v, 4);
                    }
                    break;
                }
                for (int64_t j = 0; j < left; j++) {
                    const uint8_t* p = x + j * 4;
                    uint8_t* d = o + j * 4;
                    const uint8_t keep = p[3];
                    const uint8_t v0 = m0.of3(p), v1 = m1.of3(p), v2 = m2.of3(p);
                    d[0] = v0;
                    d[1] = v1;
                    d[2] = v2;
                    d[3] = keep;
                }
                break;
            }
            const uint64_t fourth = kinds >> 24;
            const bool grey = (constant & 0xFFFFFF) == 0 && rw == 4 && ww == 4 && size == 4 && nr >= 1 && nr <= 3 && (kinds & 0xFFFFFF) == (1u | 1u << 8 | 1u << 16) &&
                              (fourth == 5 || (fourth == 0)) && from[0] < 4 && from[1] < 4 && from[2] < 4;
            const uint8_t fixedFourth = static_cast<uint8_t>(constant >> 24);
            if (grey && alwaysByte) {
                // Its own loop, nothing to decide in it, which Clang does several at a time.
                const int32_t w0 = weight[0], w1 = nr > 1 ? weight[1] : 0, w2 = nr > 2 ? weight[2] : 0;
                const uint64_t r0 = from[0], r1 = nr > 1 ? from[1] : 0, r2 = nr > 2 ? from[2] : 0;
                const bool keepFourth = fourth == 5;
                int64_t j = 0;
                // Four pixels at a time (2026-11-15), where the mix ends in a shift alone: each
                // pixel's bytes made 16-bit, times the weight at each byte (the weights of a byte
                // read twice added), pairs added (pmaddwd) and the two halves of each pixel added,
                // shifted; the grey three times and the fourth byte the pixel's own or the
                // constant. The same sums as one at a time, in 32 bits.
                int32_t at[4] = {0, 0, 0, 0};
                at[r0] += w0, at[r1] += w1, at[r2] += w2;
                // A division too where it is a multiply (MixEnd's quick: what is divided from 0 to
                // 65535): the high 32 bits of each lane times the magic number, two lanes at a time.
                const bool divides = mixEnd.divisor != 1;
                bool wide = !divides || mixEnd.quick;
                for (int k = 0; k < 4; k++) wide = wide && at[k] >= -32768 && at[k] <= 32767;
                if (wide) {
                    const __m128i weights = _mm_setr_epi16(static_cast<int16_t>(at[0]), static_cast<int16_t>(at[1]), static_cast<int16_t>(at[2]),
                                                           static_cast<int16_t>(at[3]), static_cast<int16_t>(at[0]), static_cast<int16_t>(at[1]),
                                                           static_cast<int16_t>(at[2]), static_cast<int16_t>(at[3]));
                    const __m128i zero = _mm_setzero_si128(), added = _mm_set1_epi32(c), count = _mm_cvtsi32_si128(mixEnd.shift);
                    const __m128i magic = _mm_set1_epi32(static_cast<int32_t>(static_cast<uint32_t>(mixEnd.magic)));
                    const __m128i highs = _mm_slli_epi64(made128(_mm_cmpeq_epi32(zero, zero)), 32);
                    const __m128i fourthMask = _mm_set1_epi32(keepFourth ? static_cast<int32_t>(0xFF000000u) : 0);
                    const __m128i fourthFixed = _mm_set1_epi32(keepFourth ? 0 : static_cast<int32_t>(static_cast<uint32_t>(fixedFourth) << 24));
                    for (; j + 4 <= left; j += 4) {
                        const __m128i px = _mm_loadu_si128(reinterpret_cast<const __m128i*>(x + j * 4));
                        const __m128i lo = _mm_madd_epi16(_mm_unpacklo_epi8(px, zero), weights);
                        const __m128i hi = _mm_madd_epi16(_mm_unpackhi_epi8(px, zero), weights);
                        const __m128 lf = _mm_castsi128_ps(lo), hf = _mm_castsi128_ps(hi);
                        const __m128i firsts = _mm_castps_si128(_mm_shuffle_ps(lf, hf, _MM_SHUFFLE(2, 0, 2, 0)));
                        const __m128i seconds = _mm_castps_si128(_mm_shuffle_ps(lf, hf, _MM_SHUFFLE(3, 1, 3, 1)));
                        const __m128i sum = _mm_add_epi32(_mm_add_epi32(firsts, seconds), added);
                        __m128i v;
                        if (!divides) {
                            v = _mm_sra_epi32(sum, count);
                        } else {
                            const __m128i n = mixEnd.divideFirst ? sum : _mm_sra_epi32(sum, count);
                            const __m128i even = _mm_srli_epi64(_mm_mul_epu32(n, magic), 32);
                            const __m128i odd = _mm_and_si128(_mm_mul_epu32(_mm_srli_epi64(n, 32), magic), highs);
                            const __m128i q = _mm_or_si128(even, odd);
                            v = mixEnd.divideFirst ? _mm_sra_epi32(q, count) : q;
                        }
                        const __m128i grey = _mm_or_si128(_mm_or_si128(v, _mm_slli_epi32(v, 8)), _mm_slli_epi32(v, 16));
                        const __m128i out = _mm_or_si128(_mm_or_si128(grey, _mm_and_si128(px, fourthMask)), fourthFixed);
                        _mm_storeu_si128(reinterpret_cast<__m128i*>(o + j * 4), out);
                    }
                }
                for (; j < left; j++) {
                    const uint8_t* p = x + j * 4;
                    uint8_t* d = o + j * 4;
                    const uint8_t keep = keepFourth ? p[3] : fixedFourth;
                    const uint8_t v = static_cast<uint8_t>(mixEnd(w0 * p[r0] + w1 * p[r1] + w2 * p[r2] + c));
                    d[0] = v;
                    d[1] = v;
                    d[2] = v;
                    d[3] = keep;
                }
                break;
            }
            for (int64_t j = 0; j < left; j++) {
                const uint8_t* p = x + static_cast<uint64_t>(j) * rw;
                int32_t v = c;
                for (int k = 0; k < nr; k++) v += weight[k] * p[from[k]];
                v = mixEnd(v);
                if (!alwaysByte && static_cast<uint32_t>(v) > 255u) {
                    done = j;
                    break;
                }
                uint8_t* d = o + static_cast<uint64_t>(j) * ww;
                if (grey) {
                    const uint8_t keep = fourth == 5 ? p[3] : fixedFourth;
                    d[0] = d[1] = d[2] = static_cast<uint8_t>(v);
                    d[3] = keep;
                    continue;
                }
                uint64_t bits = constant;
                for (uint64_t b = 0; b < size; b++) {
                    const uint64_t kind = (kinds >> (8 * b)) & 0xFF;
                    if (kind == 0) continue;
                    const uint64_t byte = kind == 1 ? static_cast<uint64_t>(v) & 0xFF : kind >= 0x80 ? table[b][p[kind & 7]] : kind >= 0x60 ? fm[kind & 3](p) : p[kind - 2];
                    bits |= byte << (8 * b);  // as packb lays it on the constant
                }
                for (uint64_t b = 0; b < size; b++) d[b] = static_cast<uint8_t>(bits >> (8 * b));
            }
            break;
        }
        case kLoopWCopyItems: {
            // Items of a value type copied from one list looked at once to another, or along the
            // same one, each at a base plus the counter (2026-11-14: copy, crop, a grid's part):
            // words 50 to 52 the base written at, 63 to 65 the base read at (50, 63: 0 none, 1 an
            // operand), 55 the items' width, 56 what hidxsm is given (the bytes it reads, the
            // value's type number from bit 32), 57 what hsetsm is given (the bytes it stores in
            // bits 24 to 31, the type number). Moved at once: the bytes the steps would move.
            if (!a.ok() || !out.ok()) return;
            int64_t to = 0, from = 0, toFirst, toLast, fromFirst, fromLast;
            if (t[50] == 1 && !operand(ctx, t[51], t[52], 0, to)) return;
            if (t[63] == 1 && !operand(ctx, t[64], t[65], 0, from)) return;
            if (__builtin_add_overflow(to, next, &toFirst) || __builtin_add_overflow(to, end - 1, &toLast) || __builtin_add_overflow(from, next, &fromFirst) ||
                __builtin_add_overflow(from, end - 1, &fromLast) || toFirst < 0 || fromFirst < 0 || static_cast<uint64_t>(toLast) >= out.h[2] ||
                static_cast<uint64_t>(fromLast) >= a.h[1])
                return;
            const uint64_t width = t[55], read = t[56], stored = t[57];
            const uint64_t size = (stored >> 24) & 0xFF;
            // The value hidxsm makes is the type hsetsm stores (where not, the steps leave for the
            // VM at the first item), and every byte stored is one read.
            if ((read >> 32) != (stored >> 32) || size == 0 || size > (read & 0xFF) || size > width) return;
            uint8_t* o = reinterpret_cast<uint8_t*>(out.h[0]) + static_cast<uint64_t>(toFirst) * width;
            const uint8_t* x = reinterpret_cast<const uint8_t*>(a.h[0]) + static_cast<uint64_t>(fromFirst) * width;
            const uint64_t bytes = static_cast<uint64_t>(left) * width;
            // Along the same list, the items written after those read and among them: the steps
            // copy each over one still to be read, which one move would not.
            if (o > x && o < x + bytes) return;
            if (size == width) {
                // Forward, as the steps go (from a place after the one written, where they overlap).
                uint64_t n = bytes;
                moveBytes(o, x, n);
            } else {
                for (int64_t j = 0; j < left; j++)
                    for (uint64_t k = 0; k < size; k++) o[static_cast<uint64_t>(j) * width + k] = x[static_cast<uint64_t>(j) * width + k];
            }
            break;
        }
        case kLoopWAddItems: {
            // Items of a value type added one by one at the end of a list, from a list looked at
            // once at a base plus the counter (2026-11-15: to.add(from[i]), a grid's part and copy):
            // words 63 to 65 the base read at (63: 0 none, 1 an operand), 55 the slot of the list
            // added to, 56 the items' class, 57 the slot pushsm writes its true to. As many as the
            // list has room for moved at once, and its end moved on; the rest left to the steps
            // (pushsm leaves for the VM's add, which makes room). The list added to is no list
            // looked at once (regions.cpp, lookAtListsOnce): its items are never those read here
            // but where the same list is read through another name, and then only those it had.
            if (!a.ok()) return;
            int64_t from = 0, fromFirst, fromLast;
            if (t[63] == 1 && !operand(ctx, t[64], t[65], 0, from)) return;
            if (__builtin_add_overflow(from, next, &fromFirst) || __builtin_add_overflow(from, end - 1, &fromLast) || fromFirst < 0 ||
                static_cast<uint64_t>(fromLast) >= a.h[1])
                return;
            const uint8_t* fromList = reinterpret_cast<const uint8_t*>(a.h[3]);
            const uint8_t* fromPack = packOf(fromList, kStoreItems);
            uint8_t* pack;
            uint8_t* at = roomToAdd(base[t[55]], ctx, kStoreItems, pack);
            if (at == nullptr || fromPack == nullptr) return;
            const int64_t cls = static_cast<int64_t>(t[56]);
            if (word<int64_t>(pack + kPackItemClass) != cls || word<const void*>(pack + kPackItemType) != nullptr || word<uint32_t>(pack + kPackTypeSlot) != 0 ||
                word<int64_t>(fromPack + kPackItemClass) != cls || word<const void*>(fromPack + kPackItemType) != nullptr ||
                word<uint32_t>(fromPack + kPackTypeSlot) != 0)
                return;
            const uint64_t width = word<uint32_t>(pack + kPackWidth);
            if (width == 0 || width > 8 || word<uint32_t>(fromPack + kPackWidth) != width) return;
            const uint64_t room = static_cast<uint64_t>(word<const uint8_t*>(pack + kPackBlock + 16) - at) / width;
            done = static_cast<uint64_t>(left) < room ? left : static_cast<int64_t>(room);
            if (done == 0) return;
            const uint8_t* x = reinterpret_cast<const uint8_t*>(a.h[0]) + static_cast<uint64_t>(fromFirst) * width;
            uint8_t* o = at;
            uint64_t n = static_cast<uint64_t>(done) * width;
            moveBytes(o, x, n);
            uint8_t* const newEnd = at + static_cast<uint64_t>(done) * width;
            __builtin_memcpy(pack + kPackBlock + 8, &newEnd, 8);
            const uint64_t count = word<uint64_t>(pack + kPackCount) + static_cast<uint64_t>(done);
            __builtin_memcpy(pack + kPackCount, &count, 8);
            storeSlot(base, static_cast<int64_t>(t[57]), kKindBool, 1);
            break;
        }
        case kLoopWCountItems: {
            // Byte fields of each item counted into a list of ints (regions.cpp, countItems: a
            // histogram, a palette's bins): `counts[K + key] += v` for each store in order, the
            // key a byte field of the item or the key the stores share (fields shifted and or'd),
            // v a constant or a byte field; an item skipped where its skip's comparison holds.
            // Words: 50 to 54 where the items are read; 128 how many stores, 129 the items' width,
            // 130 how many terms the shared key has, three each from 131 (offset, right, left);
            // 143 to 145 the skip (comparison, offset, constant); from 146 four a store (K, its own
            // field or -1, 0 a constant v or 1 a field, v or the field). Every place any key can
            // reach within the counts, and no count able to leave an int's range however the
            // items fall.
            static_assert(kLoopWCountItems == 26, "regions.cpp's ah.shape 26");
            if (!a.ok() || !out.ok() || out.kind() != 1) return;
            const uint64_t n = t[128], width = t[129], nt = t[130];
            if (n == 0 || n > 8 || width == 0 || width > 0xFFFF || nt > 4) return;
            int64_t at = 0, first, last;
            if (!itemBase(ctx, at) || __builtin_add_overflow(at, next, &first) || __builtin_add_overflow(at, end - 1, &last) || first < 0 ||
                static_cast<uint64_t>(last) >= a.h[1])
                return;
            uint64_t toff[4] = {0, 0, 0, 0}, tshr[4] = {0, 0, 0, 0}, tshl[4] = {0, 0, 0, 0};
            uint64_t keyMost = 0;  // the largest the shared key can be
            for (uint64_t k = 0; k < nt; k++) {
                toff[k] = t[131 + 3 * k], tshr[k] = t[132 + 3 * k], tshl[k] = t[133 + 3 * k];
                if (toff[k] >= width || tshr[k] > 7 || tshl[k] > 31) return;
                keyMost |= (uint64_t{255} >> tshr[k]) << tshl[k];
            }
            const uint64_t skipOp = t[143], skipOff = t[144];
            const int64_t skipK = static_cast<int64_t>(t[145]);
            if (skipOp > 6 || (skipOp != 0 && skipOff >= width)) return;
            int64_t* counts = out.ints();
            int64_t K[8], v[8];
            int64_t own[8], kind[8];
            int64_t most = 0, least = 0;
            int64_t step = 0;  // how far one item may move a count, all stores together
            uint64_t from = ~uint64_t{0}, to = 0;  // every place a store can reach lies between
            for (uint64_t s = 0; s < n; s++) {
                const uint64_t* w = t + 146 + 4 * s;
                K[s] = static_cast<int64_t>(w[0]), own[s] = static_cast<int64_t>(w[1]), kind[s] = static_cast<int64_t>(w[2]), v[s] = static_cast<int64_t>(w[3]);
                if (own[s] < 0 && nt == 0) return;
                if (own[s] >= static_cast<int64_t>(width) || (kind[s] == 1 && (v[s] < 0 || v[s] >= static_cast<int64_t>(width))) || kind[s] < 0 || kind[s] > 1) return;
                const uint64_t reach = own[s] >= 0 ? 255 : keyMost;
                if (K[s] < 0 || static_cast<uint64_t>(K[s]) + reach >= out.h[1] || static_cast<uint64_t>(K[s]) + reach >= out.h[2]) return;
                step += kind[s] == 1 ? 255 : v[s] < 0 ? -v[s] : v[s];
                from = static_cast<uint64_t>(K[s]) < from ? static_cast<uint64_t>(K[s]) : from;
                to = static_cast<uint64_t>(K[s]) + reach > to ? static_cast<uint64_t>(K[s]) + reach : to;
            }
            // The counts there, once (the stores' ranges overlap: a palette's four, all of them).
            for (uint64_t k = from; k <= to; k++) {
                const int64_t now = counts[k];
                most = now > most ? now : most, least = now < least ? now : least;
            }
            int64_t reachAll;
            if (__builtin_mul_overflow(step, left, &reachAll) || __builtin_add_overflow(most, reachAll, &most) || __builtin_sub_overflow(least, reachAll, &least)) return;
            const uint8_t* x = a.bytes() + static_cast<uint64_t>(first) * width;
            if (n == 4 && nt == 0 && skipOp == 0 && kind[0] == 0 && kind[1] == 0 && kind[2] == 0 && kind[3] == 0 && v[0] == 1 && v[1] == 1 && v[2] == 1 &&
                v[3] == 1) {
                // Four ones, each at its own field (a histogram): their places held apart.
                int64_t *q0 = counts + K[0], *q1 = counts + K[1], *q2 = counts + K[2], *q3 = counts + K[3];
                const uint64_t o0 = static_cast<uint64_t>(own[0]), o1 = static_cast<uint64_t>(own[1]), o2 = static_cast<uint64_t>(own[2]),
                               o3 = static_cast<uint64_t>(own[3]);
                for (int64_t j = 0; j < left; j++) {
                    const uint8_t* p = x + static_cast<uint64_t>(j) * width;
                    q0[p[o0]]++;
                    q1[p[o1]]++;
                    q2[p[o2]]++;
                    q3[p[o3]]++;
                }
                break;
            }
            // The tables hold the key shifted down by its terms' least shift left (a key times four: 2).
            uint64_t low = 31;
            for (uint64_t k = 0; k < nt; k++) low = tshl[k] < low ? tshl[k] : low;
            if (n == 4 && nt >= 1 && nt <= 3 && own[0] < 0 && own[1] < 0 && own[2] < 0 && own[3] < 0 && (keyMost >> low) < 65536) {
                // As below, each term's part of the key and the skip read from tables of the
                // byte's 256 values (a stencil's stack holds them: under 2 KB), no shift or
                // comparison for each pixel.
                uint16_t part[3][256];
                uint8_t skipped[256];
                for (uint64_t k = 0; k < 3; k++)
                    for (uint64_t b = 0; b < 256; b++) part[k][b] = k < nt ? static_cast<uint16_t>((b >> tshr[k]) << (tshl[k] - low)) : 0;
                for (int64_t b = 0; b < 256; b++)
                    skipped[b] = skipOp == 0 ? 0 : skipOp == 1 ? b < skipK : skipOp == 2 ? b <= skipK : skipOp == 3 ? b > skipK : skipOp == 4 ? b >= skipK : skipOp == 5 ? b == skipK : b != skipK;
                const uint64_t o0 = toff[0], o1 = nt > 1 ? toff[1] : 0, o2 = nt > 2 ? toff[2] : 0, so = skipOp != 0 ? skipOff : 0;
                int64_t *q0 = counts + K[0], *q1 = counts + K[1], *q2 = counts + K[2], *q3 = counts + K[3];
                const uint64_t fo0 = kind[0] == 1 ? static_cast<uint64_t>(v[0]) : 0, fo1 = kind[1] == 1 ? static_cast<uint64_t>(v[1]) : 0;
                const uint64_t fo2 = kind[2] == 1 ? static_cast<uint64_t>(v[2]) : 0, fo3 = kind[3] == 1 ? static_cast<uint64_t>(v[3]) : 0;
                const int64_t mk0 = kind[0] == 1 ? 0xFF : 0, mk1 = kind[1] == 1 ? 0xFF : 0, mk2 = kind[2] == 1 ? 0xFF : 0, mk3 = kind[3] == 1 ? 0xFF : 0;
                const int64_t ck0 = kind[0] == 1 ? 0 : v[0], ck1 = kind[1] == 1 ? 0 : v[1], ck2 = kind[2] == 1 ? 0 : v[2], ck3 = kind[3] == 1 ? 0 : v[3];
                for (int64_t j = 0; j < left; j++) {
                    const uint8_t* p = x + static_cast<uint64_t>(j) * width;
                    if (skipped[p[so]]) continue;
                    const uint64_t key = (static_cast<uint64_t>(part[0][p[o0]]) | part[1][p[o1]] | part[2][p[o2]]) << low;
                    q0[key] += (static_cast<int64_t>(p[fo0]) & mk0) + ck0;
                    q1[key] += (static_cast<int64_t>(p[fo1]) & mk1) + ck1;
                    q2[key] += (static_cast<int64_t>(p[fo2]) & mk2) + ck2;
                    q3[key] += (static_cast<int64_t>(p[fo3]) & mk3) + ck3;
                }
                break;
            }
            if (n == 4 && nt >= 1 && nt <= 3 && own[0] < 0 && own[1] < 0 && own[2] < 0 && own[3] < 0) {
                // Four stores at the one shared key (a palette's sums and count): no loop over
                // them. A term past the key's own ones reads byte 0 and adds nothing (shifted out).
                const uint64_t o0 = toff[0], o1 = nt > 1 ? toff[1] : 0, o2 = nt > 2 ? toff[2] : 0;
                const uint64_t r0 = tshr[0], r1 = nt > 1 ? tshr[1] : 8, r2 = nt > 2 ? tshr[2] : 8;
                const uint64_t l0 = tshl[0], l1 = nt > 1 ? tshl[1] : 0, l2 = nt > 2 ? tshl[2] : 0;
                int64_t *q0 = counts + K[0], *q1 = counts + K[1], *q2 = counts + K[2], *q3 = counts + K[3];
                // A store's v: the byte at its offset where it is a field (mask 0xFF), else the constant.
                const uint64_t fo0 = kind[0] == 1 ? static_cast<uint64_t>(v[0]) : 0, fo1 = kind[1] == 1 ? static_cast<uint64_t>(v[1]) : 0;
                const uint64_t fo2 = kind[2] == 1 ? static_cast<uint64_t>(v[2]) : 0, fo3 = kind[3] == 1 ? static_cast<uint64_t>(v[3]) : 0;
                const int64_t mk0 = kind[0] == 1 ? 0xFF : 0, mk1 = kind[1] == 1 ? 0xFF : 0, mk2 = kind[2] == 1 ? 0xFF : 0, mk3 = kind[3] == 1 ? 0xFF : 0;
                const int64_t ck0 = kind[0] == 1 ? 0 : v[0], ck1 = kind[1] == 1 ? 0 : v[1], ck2 = kind[2] == 1 ? 0 : v[2], ck3 = kind[3] == 1 ? 0 : v[3];
                for (int64_t j = 0; j < left; j++) {
                    const uint8_t* p = x + static_cast<uint64_t>(j) * width;
                    if (skipOp != 0) {
                        const int64_t f = p[skipOff];
                        const bool holds = skipOp == 1 ? f < skipK : skipOp == 2 ? f <= skipK : skipOp == 3 ? f > skipK : skipOp == 4 ? f >= skipK : skipOp == 5 ? f == skipK : f != skipK;
                        if (holds) continue;
                    }
                    const uint64_t key = (static_cast<uint64_t>(p[o0]) >> r0) << l0 | (static_cast<uint64_t>(p[o1]) >> r1) << l1 | (static_cast<uint64_t>(p[o2]) >> r2) << l2;
                    q0[key] += (static_cast<int64_t>(p[fo0]) & mk0) + ck0;
                    q1[key] += (static_cast<int64_t>(p[fo1]) & mk1) + ck1;
                    q2[key] += (static_cast<int64_t>(p[fo2]) & mk2) + ck2;
                    q3[key] += (static_cast<int64_t>(p[fo3]) & mk3) + ck3;
                }
                break;
            }
            for (int64_t j = 0; j < left; j++) {
                const uint8_t* p = x + static_cast<uint64_t>(j) * width;
                if (skipOp != 0) {
                    const int64_t f = p[skipOff];
                    const bool holds = skipOp == 1 ? f < skipK : skipOp == 2 ? f <= skipK : skipOp == 3 ? f > skipK : skipOp == 4 ? f >= skipK : skipOp == 5 ? f == skipK : f != skipK;
                    if (holds) continue;
                }
                uint64_t key = 0;
                for (uint64_t k = 0; k < nt; k++) key |= (static_cast<uint64_t>(p[toff[k]]) >> tshr[k]) << tshl[k];
                for (uint64_t s = 0; s < n; s++)
                    counts[K[s] + static_cast<int64_t>(own[s] >= 0 ? p[own[s]] : key)] += kind[s] == 1 ? static_cast<int64_t>(p[v[s]]) : v[s];
            }
            break;
        }
        case kLoopWBlendNormal: {
            // The normal blend of a row (regions.cpp, blendNormal, says the formula and the
            // words): each backdrop pixel set from its source pixel, a see-through source's left;
            // every operation in the steps' order, so the same pixels. Refused for a NaN opacity
            // (min and max then not the same either way round).
            static_assert(kLoopWBlendNormal == 28, "regions.cpp's ah.shape 28");
            if (!a.ok() || !out.ok() || a.h[0] == out.h[0]) return;
            const Slot* g = reinterpret_cast<const Slot*>(ctx->globals) + t[97];
            if (g->kind != kKindObj || reinterpret_cast<uint64_t>(g->p) != t[98]) return;  // the class mksm checks
            const uint64_t tk = t[130];
            if (tk >= 6) return;
            const uint64_t* ht = ctx->lists + 4 * tk;
            if (ht[3] != 2 || ht[1] < 256) return;
            const double* u = reinterpret_cast<const double*>(ht[0]);
            const double op = bitsF(t[129]);
            if (op != op) return;
            int64_t sb = 0, db = 0, sFirst, sLast, dFirst, dLast;
            if (!itemBase(ctx, sb) || !itemBase(ctx, db, 56)) return;
            if (__builtin_add_overflow(sb, next, &sFirst) || __builtin_add_overflow(sb, end - 1, &sLast) || __builtin_add_overflow(db, next, &dFirst) ||
                __builtin_add_overflow(db, end - 1, &dLast) || sFirst < 0 || dFirst < 0 || static_cast<uint64_t>(sLast) >= a.h[1] ||
                static_cast<uint64_t>(dLast) >= out.h[1] || static_cast<uint64_t>(dLast) >= out.h[2])
                return;
            const uint8_t* sp = a.bytes() + static_cast<uint64_t>(sFirst) * 4;
            uint8_t* dp = out.bytes() + static_cast<uint64_t>(dFirst) * 4;
            {
#pragma clang fp contract(off)
                auto held = [](double v) {
                    const double m = 0.0 > v ? 0.0 : v;  // max(0, v), then min(1, m): as minf and maxf
                    const double c = 1.0 < m ? 1.0 : m;
                    return static_cast<uint8_t>(static_cast<int64_t>(c * 255.0 + 0.5));
                };
                for (int64_t j = 0; j < left; j++, sp += 4, dp += 4) {
                    const double sa = u[sp[3]] * op;
                    if (sa <= 0.0) continue;
                    const double ba = u[dp[3]];
                    const double ao = sa + ba * (1.0 - sa);
                    uint8_t made[4];
                    for (int c = 0; c < 3; c++) {
                        const double cs = u[sp[c]], cb = u[dp[c]];
                        const double cr = (1.0 - ba) * cs + ba * cs;
                        made[c] = held((sa * cr + ba * (1.0 - sa) * cb) / ao);
                    }
                    made[3] = held(ao);
                    dp[0] = made[0], dp[1] = made[1], dp[2] = made[2], dp[3] = made[3];
                }
            }
            break;
        }
        case kLoopFGather4: {
            // Four floats a pixel each plus a gathered float times a weight (regions.cpp,
            // gather4: scaling's across pass): out[o + p] = out[o + p] + line[4 idx[i] + p] * w[i]
            // for p from 0 to 3, o the out base plus four times the counter, i the index and
            // weight lists' base plus the counter. Words 50 to 52 the out base, 63 to 66 the other
            // base, 129 the weight list. An index whose four floats are not in the line stops it
            // there, for the steps to throw.
            static_assert(kLoopFGather4 == 27, "regions.cpp's ah.shape 27");
            const uint64_t wk = t[129];
            if (wk >= 6 || !out.ok() || out.kind() != 2 || !a.ok() || a.kind() != 2 || !b.ok() || b.kind() != 1) return;
            const uint64_t* hw = ctx->lists + 4 * wk;
            if (hw[3] != 2 || hw[0] == out.h[0] || a.h[0] == out.h[0]) return;
            int64_t ob = 0, ib = 0;
            if (t[50] == 1 && !operand(ctx, t[51], t[52], 0, ob)) return;
            if (t[63] == 1 && !operand(ctx, t[64], t[65], 0, ib)) return;
            if (!small(t[66]) || ob < -(int64_t{1} << 50) || ob > (int64_t{1} << 50) || ib < -(int64_t{1} << 50) || ib > (int64_t{1} << 50) ||
                next > (int64_t{1} << 50) || end > (int64_t{1} << 50))
                return;
            ib += static_cast<int64_t>(t[66]);
            const int64_t oFirst = ob + 4 * next, oLast = ob + 4 * (end - 1) + 3, iFirst = ib + next, iLast = ib + end - 1;
            if (oFirst < 0 || iFirst < 0 || static_cast<uint64_t>(oLast) >= out.h[1] || static_cast<uint64_t>(oLast) >= out.h[2] ||
                static_cast<uint64_t>(iLast) >= b.h[1] || static_cast<uint64_t>(iLast) >= hw[1])
                return;
            double* o = out.floats() + oFirst;
            const double* line = a.floats();
            const uint64_t lineCount = a.h[1];
            const int64_t* idx = b.ints() + iFirst;
            const double* w = reinterpret_cast<const double*>(hw[0]) + iFirst;
            {
#pragma clang fp contract(off)
                for (int64_t j = 0; j < left; j++, o += 4) {
                    const int64_t c = idx[j];
                    if (c < 0 || c > (int64_t{1} << 58) || static_cast<uint64_t>(4 * c + 3) >= lineCount) {
                        done = j;
                        break;
                    }
                    const double wt = w[j];
                    const double* q = line + 4 * c;
                    o[0] = o[0] + q[0] * wt;
                    o[1] = o[1] + q[1] * wt;
                    o[2] = o[2] + q[2] * wt;
                    o[3] = o[3] + q[3] * wt;
                }
            }
            break;
        }
        case kLoopWToFloats:
        case kLoopWFromFloats: {
            // Colours made four floats a pixel, or back (2026-11-15: numeric.ww's __premultiplied
            // and __unpremultiplied; regions.cpp, colorFloats, says what each works out and the
            // words). Each base the counter (times four for the floats) plus an operand and a
            // constant: words 50 to 53 the list written's, 63 to 66 the list read's.
            const bool to = t[0] == kLoopWToFloats;
            const Items& floats = to ? out : a;
            const Items& items = to ? a : out;
            if (!floats.ok() || floats.kind() != 2 || !items.ok() || items.kind() <= 3) return;
            int64_t ob = 0, ab = 0;
            if (t[50] == 1 && !operand(ctx, t[51], t[52], 0, ob)) return;
            if (t[63] == 1 && !operand(ctx, t[64], t[65], 0, ab)) return;
            if (!small(t[53]) || !small(t[66]) || ob < -(int64_t{1} << 50) || ob > (int64_t{1} << 50) || ab < -(int64_t{1} << 50) || ab > (int64_t{1} << 50) ||
                next > (int64_t{1} << 50) || end > (int64_t{1} << 50))
                return;
            ob += static_cast<int64_t>(t[53]), ab += static_cast<int64_t>(t[66]);
            const int64_t fb = to ? ob : ab, ib = to ? ab : ob;
            const int64_t fFirst = fb + 4 * next, fLast = fb + 4 * (end - 1) + 3, iFirst = ib + next, iLast = ib + end - 1;
            if (fFirst < 0 || iFirst < 0 || static_cast<uint64_t>(fLast) >= floats.h[to ? 2 : 1] || static_cast<uint64_t>(iLast) >= items.h[to ? 1 : 2]) return;
            double* f = floats.floats() + fFirst;
            uint8_t* p = items.bytes() + static_cast<uint64_t>(iFirst) * 4;
            if (to) {
                for (int64_t j = 0; j < left; j++, p += 4, f += 4) {
                    const double al = static_cast<double>(p[3]);
                    const double m = al / 255.0;
                    f[0] = static_cast<double>(p[0]) * m;
                    f[1] = static_cast<double>(p[1]) * m;
                    f[2] = static_cast<double>(p[2]) * m;
                    f[3] = al;
                }
                break;
            }
            const Slot* g = reinterpret_cast<const Slot*>(ctx->globals) + t[97];
            if (g->kind != kKindObj || reinterpret_cast<uint64_t>(g->p) != t[98]) return;  // the class mksm checks
            auto part = [](double v) {
                v = v + 0.5;
                v = v > 0.0 ? v : 0.0;
                return static_cast<uint8_t>(static_cast<int64_t>(v < 255.0 ? v : 255.0));
            };
            for (int64_t j = 0; j < left; j++, p += 4, f += 4) {
                // Where any is NaN, the steps from there (as the VM's min and max have it).
                if (f[0] != f[0] || f[1] != f[1] || f[2] != f[2] || f[3] != f[3]) {
                    done = j;
                    break;
                }
                double al = f[3] > 0.0 ? f[3] : 0.0;
                al = al < 255.0 ? al : 255.0;
                const int64_t cut = static_cast<int64_t>(al + 0.5);
                const double gate = static_cast<double>(cut < 1 ? cut : 1);
                const double m = gate * (255.0 / (al > 0.5 ? al : 0.5));
                const uint8_t r = part(f[0] * m), gr = part(f[1] * m), b = part(f[2] * m), alpha = part(al);
                p[0] = r, p[1] = gr, p[2] = b, p[3] = alpha;
            }
            break;
        }
        case kLoopFMulAdd: {
            // A tap of a convolution (2026-11-15: __mulAdd): out[ob + j] = a[ab + j] * c1, or
            // out[ob + j] + that where word 55 says, each base the counter plus an operand (words
            // 50 to 52 out's, 63 to 65 a's; 50, 63: 0 none, 1 an operand). Forward, one item at a
            // time, as the steps go: the same where the two are one list.
            if (!out.ok() || !a.ok() || out.kind() != 2 || a.kind() != 2) return;
            int64_t ob = 0, ab = 0, oFirst, oLast, aFirst, aLast;
            if (t[50] == 1 && !operand(ctx, t[51], t[52], 0, ob)) return;
            if (t[63] == 1 && !operand(ctx, t[64], t[65], 0, ab)) return;
            if (__builtin_add_overflow(ob, next, &oFirst) || __builtin_add_overflow(ob, end - 1, &oLast) || __builtin_add_overflow(ab, next, &aFirst) ||
                __builtin_add_overflow(ab, end - 1, &aLast) || oFirst < 0 || aFirst < 0 || static_cast<uint64_t>(oLast) >= out.h[2] ||
                static_cast<uint64_t>(oLast) >= out.h[1] || static_cast<uint64_t>(aLast) >= a.h[1])
                return;
            double* o = out.floats() + oFirst;
            const double* x = a.floats() + aFirst;
            if (t[55] != 0) {
#pragma clang fp contract(off)
                for (int64_t j = 0; j < left; j++) {
                    const double m = x[j] * c1;
                    o[j] = o[j] + m;
                }
            } else {
                for (int64_t j = 0; j < left; j++) o[j] = x[j] * c1;
            }
            break;
        }
        case kLoopWSameItems: {
            // Items of two lists looked at once compared, each at a base plus the counter, until
            // two are not the same (2026-11-14: an Image's ==): words 50 to 52 the first's base, 63
            // to 65 the second's (50, 63: 0 none, 1 an operand), 55 how many bytes of each. Those
            // the same moved past; the first two that differ left to the loop's own steps.
            if (!a.ok() || !b.ok()) return;
            int64_t atA = 0, atB = 0, firstA, lastA, firstB, lastB;
            if (t[50] == 1 && !operand(ctx, t[51], t[52], 0, atA)) return;
            if (t[63] == 1 && !operand(ctx, t[64], t[65], 0, atB)) return;
            if (__builtin_add_overflow(atA, next, &firstA) || __builtin_add_overflow(atA, end - 1, &lastA) || __builtin_add_overflow(atB, next, &firstB) ||
                __builtin_add_overflow(atB, end - 1, &lastB) || firstA < 0 || firstB < 0 || static_cast<uint64_t>(lastA) >= a.h[1] ||
                static_cast<uint64_t>(lastB) >= b.h[1])
                return;
            const uint8_t* packA = packOf(reinterpret_cast<const uint8_t*>(a.h[3]), kStoreItems);
            const uint8_t* packB = packOf(reinterpret_cast<const uint8_t*>(b.h[3]), kStoreItems);
            if (packA == nullptr || packB == nullptr) return;
            const uint64_t width = word<uint32_t>(packA + kPackWidth), size = t[55];
            if (width == 0 || width != word<uint32_t>(packB + kPackWidth) || size == 0 || size > width) return;
            const uint8_t* x = reinterpret_cast<const uint8_t*>(a.h[0]) + static_cast<uint64_t>(firstA) * width;
            const uint8_t* y = reinterpret_cast<const uint8_t*>(b.h[0]) + static_cast<uint64_t>(firstB) * width;
            int64_t j = 0;
            if (size == width) {
                // Whole items one after another: eight bytes at a time to the first that differ.
                const uint64_t bytes = static_cast<uint64_t>(left) * width;
                uint64_t k = 0;
                while (k + 8 <= bytes && word<uint64_t>(x + k) == word<uint64_t>(y + k)) k += 8;
                while (k < bytes && x[k] == y[k]) k++;
                j = static_cast<int64_t>(k / width);
            } else {
                for (; j < left; j++) {
                    bool same = true;
                    for (uint64_t k = 0; k < size; k++) same &= x[static_cast<uint64_t>(j) * width + k] == y[static_cast<uint64_t>(j) * width + k];
                    if (!same) break;
                }
            }
            done = j;
            break;
        }
        case kLoopWFillItems: {
            // Items of a list looked at once at base + counter set to one value (2026-11-13:
            // image[x, y] = c): word 55 the items' width, 56 and 57 where the value is (1 a slot,
            // 2 a captured variable, and which), 58 what hsetsm is given (the value's type number
            // in the high half, how many of its bytes in bits 24 to 31).
            if (!out.ok()) return;
            int64_t at, first, last;
            if (!itemBase(ctx, at) || __builtin_add_overflow(at, next, &first) || __builtin_add_overflow(at, end - 1, &last) || first < 0 ||
                static_cast<uint64_t>(last) >= out.h[2])
                return;
            const Slot* from = t[56] == 1 ? &base[t[57]] : t[56] == 2 ? word<const Slot*>(ctx->upvalues[t[57]] + kUpvalueLocation) : nullptr;
            if (from == nullptr) return;
            const uint64_t head = word<uint64_t>(reinterpret_cast<const uint8_t*>(from)), what = t[58], width = t[55];
            const uint64_t size = (what >> 24) & 0xFF;
            if (size == 0 || size > 8 || size > width) return;
            uint64_t bits;
            if ((head & 0xFF) == kKindSmall && (head >> 32) == (what >> 32)) {
                bits = static_cast<uint64_t>(from->i);
            } else if ((head & 0xFFFFFFFF) == kKindRef && from->p[kObjType] == kTypePlaces && t[59] != 0 &&
                       word<uint64_t>(from->p + kPlaceClass) == t[59] && word<const void*>(from->p + kPlaceType) == nullptr) {
                // A value in a place of a frame's, of the class the loop's ownsm takes (word 59).
                bits = 0;
                for (uint64_t k = 0; k < size; k++) bits |= static_cast<uint64_t>(from->p[kPlaceData + k]) << (8 * k);
            } else {
                return;
            }
            uint8_t* o = reinterpret_cast<uint8_t*>(out.h[0]) + static_cast<uint64_t>(first) * width;
            if (width == 4 && size == 4) {
                const uint32_t v = static_cast<uint32_t>(bits);
                for (int64_t j = 0; j < left; j++) __builtin_memcpy(o + j * 4, &v, 4);
            } else {
                for (int64_t j = 0; j < left; j++)
                    for (uint64_t k = 0; k < size; k++) o[static_cast<uint64_t>(j) * width + k] = static_cast<uint8_t>(bits >> (8 * k));
            }
            break;
        }
        case kLoopWField: {
            // A byte field of each item into bytes or ints at the same index (pixels.map { it.red }).
            if (!a.ok() || !out.ok() || (out.kind() != 1 && out.kind() != 3)) return;
            const uint64_t f = t[55], width = f >> 32, offset = (f >> 8) & 0xFFFFFF;
            if ((f & 0xFF) != 1 || width == 0 || offset >= width || static_cast<uint64_t>(end) > a.h[1] || static_cast<uint64_t>(end) > out.h[2]) return;
            const uint8_t* x = reinterpret_cast<const uint8_t*>(a.h[0]) + offset;
            if (out.kind() == 3) {
                uint8_t* o = out.bytes();
                if (width == 4) for (int64_t j = next; j < end; j++) o[j] = x[j * 4];
                else for (int64_t j = next; j < end; j++) o[j] = x[static_cast<uint64_t>(j) * width];
            } else {
                int64_t* o = out.ints();
                for (int64_t j = next; j < end; j++) o[j] = x[static_cast<uint64_t>(j) * width];
            }
            break;
        }
        case kLoopWIterSumField:
        case kLoopWSumField: {
            // A total of a field of items of a list looked at once (2026-11-13: image[x, y].red), the
            // items at base + counter: base from words 50 to 54 (0 none, 1 an operand, 2 one times
            // another, each two words); words 55 to 58 each field's item width (high half), offset
            // (bits 8 on) and size (1 a byte, 8 an int); word 59 how many fields, more than one
            // bytes alone, added together for each item. An overflow stops it before the item.
            if (!a.ok()) return;
            int64_t at;
            if (!itemBase(ctx, at)) return;
            int64_t first, last;
            if (__builtin_add_overflow(at, next, &first) || __builtin_add_overflow(at, end - 1, &last) || first < 0 || static_cast<uint64_t>(last) >= a.h[1]) return;
            const uint64_t width = t[55] >> 32, offset = (t[55] >> 8) & 0xFFFFFF, size = t[55] & 0xFF;
            if (width == 0 || offset + size > width) return;
            const uint8_t* x = reinterpret_cast<const uint8_t*>(a.h[0]) + static_cast<uint64_t>(first) * width + offset;
            int64_t acc = static_cast<int64_t>(t[3]);
            const uint64_t fields = t[59];
            if (fields > 1) {
                // Bytes of each item added together, then to the total.
                if (fields > 4) return;
                uint64_t at[4] = {0, 0, 0, 0};
                for (uint64_t k = 0; k < fields; k++) {
                    const uint64_t f = t[55 + k];
                    if ((f >> 32) != width || (f & 0xFF) != 1 || ((f >> 8) & 0xFFFFFF) >= width) return;
                    at[k] = (f >> 8) & 0xFFFFFF;
                }
                const uint8_t* p = reinterpret_cast<const uint8_t*>(a.h[0]) + static_cast<uint64_t>(first) * width;
                const int64_t most = 255 * static_cast<int64_t>(fields);
                if (left <= INT64_MAX / most && acc <= INT64_MAX - most * left) {
                    int64_t s = 0;
                    if (width == 4 && fields == 4 && at[0] + at[1] + at[2] + at[3] == 6 && at[0] != at[1] && at[0] != at[2] && at[0] != at[3] &&
                        at[1] != at[2] && at[1] != at[3] && at[2] != at[3])
                        for (int64_t j = 0; j < left * 4; j++) s += p[j];  // every byte of every item, once
                    else
                        for (int64_t j = 0; j < left; j++)
                            for (uint64_t k = 0; k < fields; k++) s += p[static_cast<uint64_t>(j) * width + at[k]];
                    acc += s;
                } else {
                    for (int64_t j = 0; j < left; j++) {
                        int64_t item = 0;
                        for (uint64_t k = 0; k < fields; k++) item += p[static_cast<uint64_t>(j) * width + at[k]];
                        int64_t s;
                        if (__builtin_add_overflow(acc, item, &s)) {
                            done = j;
                            break;
                        }
                        acc = s;
                    }
                }
                t[3] = static_cast<uint64_t>(acc);
                break;
            }
            if (size == 1) {
                if (left <= INT64_MAX / 255 && acc <= INT64_MAX - 255 * left) {
                    int64_t s = 0;
                    if (width == 4) for (int64_t j = 0; j < left; j++) s += x[j * 4];
                    else for (int64_t j = 0; j < left; j++) s += x[static_cast<uint64_t>(j) * width];
                    acc += s;
                } else {
                    for (int64_t j = 0; j < left; j++) {
                        int64_t s;
                        if (__builtin_add_overflow(acc, static_cast<int64_t>(x[static_cast<uint64_t>(j) * width]), &s)) {
                            done = j;
                            break;
                        }
                        acc = s;
                    }
                }
            } else if (size == 8) {
                for (int64_t j = 0; j < left; j++) {
                    int64_t s;
                    if (__builtin_add_overflow(acc, word<int64_t>(x + static_cast<uint64_t>(j) * width), &s)) {
                        done = j;
                        break;
                    }
                    acc = s;
                }
            } else {
                return;
            }
            t[3] = static_cast<uint64_t>(acc);
            break;
        }
        case kLoopWMix: {
            // Bytes of an item of K bytes set to a mix of bytes of it (2026-11-13: a greyscale):
            // out[j*K + w_m] = (sum of weight_k * a[j*K + r_k], + c) >> shift, each a byte. Word 7:
            // K, shift, how many read, how many written (a byte each), then the four written
            // offsets; word 8: c; words 9 to 12: each read's weight (high half) and offset. Every
            // offset below K, so that no item reads what another writes; weights and c small
            // enough that nothing can overflow 32 bits (the loop's steps check 64). A block of
            // items worked out whole, then stored; one holding a mix that is no byte done an item
            // at a time to it, for the loop's own steps to throw at it.
            if (!a.ok() || a.kind() != 3 || !out.ok() || out.kind() != 3) return;
            const uint64_t w7 = t[7];
            const int64_t K = static_cast<int64_t>(w7 & 0xFF), shift = static_cast<int64_t>((w7 >> 8) & 0xFF);
            const int nr = static_cast<int>((w7 >> 16) & 0xFF), nw = static_cast<int>((w7 >> 24) & 0xFF);
            if (K < 1 || shift > 31 || nr < 1 || nr > 4 || nw < 1 || nw > 4) return;
            int32_t weight[4] = {0, 0, 0, 0};
            int64_t from[4] = {0, 0, 0, 0}, to[4] = {0, 0, 0, 0};
            for (int k = 0; k < nr; k++) {
                weight[k] = static_cast<int32_t>(static_cast<int64_t>(t[9 + k]) >> 32);
                from[k] = static_cast<int64_t>(t[9 + k] & 0xFF);
                if (from[k] >= K) return;
            }
            for (int m = 0; m < nw; m++) {
                to[m] = static_cast<int64_t>((w7 >> (32 + 8 * m)) & 0xFF);
                if (to[m] >= K) return;
            }
            const int32_t c = static_cast<int32_t>(static_cast<int64_t>(t[8]));
            MixEnd mixEnd;
            mixEnd.shift = static_cast<int32_t>(shift);
            mixEnd.divisor = static_cast<int32_t>(t[61] == 0 ? 1 : t[61]);
            mixEnd.divideFirst = t[62] != 0;
            if (mixEnd.divisor < 1) return;
            int64_t rows;
            if (__builtin_mul_overflow(end, K, &rows) || static_cast<uint64_t>(rows) > a.h[1] || static_cast<uint64_t>(rows) > out.h[2]) return;
            if (a.h[0] != out.h[0]) {  // two lists: none of one in the other
                const uint64_t a0 = a.h[0], o0 = out.h[0];
                if (a0 < o0 + static_cast<uint64_t>(rows) && o0 < a0 + static_cast<uint64_t>(rows)) return;
            }
            const uint8_t* x = a.bytes();
            uint8_t* o = out.bytes();
            // Three bytes read of four, written to three (a greyscale): written out for Clang.
            const bool rgb = K == 4 && nr == 3 && from[0] == 0 && from[1] == 1 && from[2] == 2 && nw == 3 && to[0] == 0 && to[1] == 1 && to[2] == 2;
            // Where every mix is a byte whatever the bytes (the least and the most it can be, each
            // a byte), each item worked out and stored in one pass, with no check.
            int32_t least = c, most = c;
            for (int k = 0; k < nr; k++) (weight[k] < 0 ? least : most) += weight[k] * 255;
            mixEnd.prepare(least, most);
            if (mixEnd(least) >= 0 && mixEnd(most) <= 255) {
                if (rgb) {
                    const int32_t w0 = weight[0], w1 = weight[1], w2 = weight[2];
                    for (int64_t q = next; q < end; q++) {
                        const uint8_t* p = x + q * 4;
                        const uint8_t g = static_cast<uint8_t>(mixEnd(w0 * p[0] + w1 * p[1] + w2 * p[2] + c));
                        uint8_t* d = o + q * 4;
                        d[0] = g;
                        d[1] = g;
                        d[2] = g;
                    }
                } else {
                    for (int64_t q = next; q < end; q++) {
                        const uint8_t* p = x + q * K;
                        int32_t s = c;
                        for (int k = 0; k < nr; k++) s += weight[k] * p[from[k]];
                        const uint8_t g = static_cast<uint8_t>(mixEnd(s));
                        for (int m = 0; m < nw; m++) o[q * K + to[m]] = g;
                    }
                }
                break;
            }
            int64_t j = next;
            while (j < end) {
                const int64_t n = end - j < 64 ? end - j : 64;
                int32_t v[64];
                int32_t bad = 0;
                if (rgb) {
                    const int32_t w0 = weight[0], w1 = weight[1], w2 = weight[2];
                    const uint8_t* p = x + j * 4;
                    for (int64_t q = 0; q < n; q++) {
                        const int32_t s = mixEnd(w0 * p[q * 4] + w1 * p[q * 4 + 1] + w2 * p[q * 4 + 2] + c);
                        v[q] = s;
                        bad |= static_cast<int32_t>(static_cast<uint32_t>(s) > 255u);
                    }
                } else {
                    for (int64_t q = 0; q < n; q++) {
                        const uint8_t* p = x + (j + q) * K;
                        int32_t s = c;
                        for (int k = 0; k < nr; k++) s += weight[k] * p[from[k]];
                        s = mixEnd(s);
                        v[q] = s;
                        bad |= static_cast<int32_t>(static_cast<uint32_t>(s) > 255u);
                    }
                }
                int64_t good = n;
                if (bad != 0)
                    for (int64_t q = 0; q < n; q++)
                        if (static_cast<uint32_t>(v[q]) > 255u) {
                            good = q;
                            break;
                        }
                if (rgb) {
                    uint8_t* d = o + j * 4;
                    for (int64_t q = 0; q < good; q++) {
                        const uint8_t g = static_cast<uint8_t>(v[q]);
                        d[q * 4] = g;
                        d[q * 4 + 1] = g;
                        d[q * 4 + 2] = g;
                    }
                } else {
                    for (int64_t q = 0; q < good; q++)
                        for (int m = 0; m < nw; m++) o[(j + q) * K + to[m]] = static_cast<uint8_t>(v[q]);
                }
                if (good < n) {
                    done = j + good - next;
                    break;
                }
                j += n;
            }
            break;
        }
        default: return;
    }
    if (t[70] != 0 && done > 0) heldUpdates(ctx, next, done, true);
    t[1] = static_cast<uint64_t>(next + done);
    t[2] = static_cast<uint64_t>(left - done);
    if (iterator != nullptr) {
        const uint64_t at = static_cast<uint64_t>(next + done);
        __builtin_memcpy(iterator + kIterIndex, &at, 8);
    }
}
#undef reads
#undef writes
STENCIL(tloop) {
    runLoop(base, ctx);
    NEXT;
}
// The template's words set: a constant (word ww_slot = ww_ki), a register stored, or loaded.
STENCIL(ctxk) {
    ctx->t[ww_slot] = static_cast<uint64_t>(ww_ki);
    NEXT;
}
// Word ww_slot of a loop template's = the float slot ww_slot2 holds, a slot the loop never writes
// (a float mix's weight: no register kept for it); a NaN where it holds no float, which the
// template refuses (FloatMix).
STENCIL(ctxslot) {
    const Slot& s = (*slot2At(base));
    ctx->t[ww_slot] = s.kind == kKindFloat ? static_cast<uint64_t>(s.i) : 0x7FF8000000000000ull;
    NEXT;
}
#define CTXI(a, ...)                                                            \
    STENCIL(ctxsi_##a) {                                                        \
        ctx->t[ww_slot] = static_cast<uint64_t>(i##a);                        \
        NEXT;                                                                   \
    }                                                                           \
    STENCIL(ctxli_##a) {                                                        \
        i##a = static_cast<int64_t>(ctx->t[ww_slot]);                         \
        NEXT;                                                                   \
    }
I_A(CTXI, x)
#define CTXF(a, ...)                                                            \
    STENCIL(ctxsf_##a) {                                                        \
        { const double d_ = f##a; __builtin_memcpy(&ctx->t[ww_slot], &d_, 8); }                              \
        NEXT;                                                                   \
    }                                                                           \
    STENCIL(ctxlf_##a) {                                                        \
        { double d_; __builtin_memcpy(&d_, &ctx->t[ww_slot], 8); f##a = d_; }                              \
        NEXT;                                                                   \
    }
F_W(CTXF, x)
// A value kept in cell ww_slot of the region's own (regions.cpp, spillOne): stored, and loaded
// again where it is wanted.
#define HOMEI(a, ...)                                                           \
    STENCIL(homesi_##a) {                                                       \
        ctx->homes[ww_slot] = static_cast<uint64_t>(i##a);                    \
        NEXT;                                                                   \
    }                                                                           \
    STENCIL(homeli_##a) {                                                       \
        i##a = static_cast<int64_t>(ctx->homes[ww_slot]);                     \
        NEXT;                                                                   \
    }
I_A(HOMEI, x)
#define HOMEF(a, ...)                                                           \
    STENCIL(homesf_##a) {                                                       \
        { const double d_ = f##a; __builtin_memcpy(&ctx->homes[ww_slot], &d_, 8); }                          \
        NEXT;                                                                   \
    }                                                                           \
    STENCIL(homelf_##a) {                                                       \
        { double d_; __builtin_memcpy(&d_, &ctx->homes[ww_slot], 8); f##a = d_; }                          \
        NEXT;                                                                   \
    }
F_W(HOMEF, x)
