// The register stencils of native code (M9.5, docs/native-plan.html): each a small function
// compiled by Clang when jade.exe is built, never called as itself, but read out of the object
// file by tools/stencils and copied, end to end, into a region's machine code as it runs.
//
// Every stencil has the same parameters: the frame's slots, the region's context (the VM's flag,
// a collection or another thread waiting; the core of a parallel section it runs on and the
// object lent to it), then the region's registers, ints i0 to i9 and floats f0 to f7, which
// `preserve_none` keeps in the processor's registers (all twelve of the processor's it gives for
// ints, all eight for floats). A stencil does one thing to them and goes on to the next with
// `musttail`, which Clang makes a plain jump; a register it does not touch is passed through as
// it is. i9 is the region's flag register: a comparison's result, which the jumps and `and`
// after it read (src/native/regions.cpp gives out i0 to i8).
//
// Its gaps are references to the names below, filled in when it is copied:
//   jade_next, jade_jump, jade_exit   where it goes on: the stencil after it, a jump's target,
//                                     the way out to the VM this step has
//   jade_kf, jade_ki                  a float or an int constant
//   jade_slot, jade_slot2             slots of the frame
//   jade_ret                          what the region gives back: where the VM goes on
// Each is reached through a word placed after the region's code (a 32-bit offset from the
// instruction), so that no stencil needs an absolute address.
//
// A stencil's name says what it does and to which registers: addf_4_4_5 is f4 = f4 + f5. The
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
using namespace jade::native::layout;
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
    uint64_t t[16];
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
INLINE void storeSlot(Slot* base, int64_t slot, uint8_t kind, uint64_t bits) {
    Slot s;
    __builtin_memset(&s, 0, sizeof s);
    s.kind = kind;
    __builtin_memcpy(&s.i, &bits, 8);
    __builtin_memcpy(&base[slot], &s, sizeof s);
}
}  // namespace

#define PARAMS                                                                                                         \
    Slot *base, Ctx *ctx, int64_t i0, int64_t i1, int64_t i2, int64_t i3, int64_t i4, int64_t i5, int64_t i6,    \
        int64_t i7, int64_t i8, int64_t i9, double f0, double f1, double f2, double f3, double f4, double f5, double f6, \
        double f7
#define ARGS base, ctx, i0, i1, i2, i3, i4, i5, i6, i7, i8, i9, f0, f1, f2, f3, f4, f5, f6, f7
#define JADE_STENCIL extern "C" __attribute__((preserve_none)) int64_t

JADE_STENCIL jade_next(PARAMS);
JADE_STENCIL jade_jump(PARAMS);
JADE_STENCIL jade_exit(PARAMS);
extern "C" const double jade_kf;
extern "C" const int64_t jade_ki;
extern "C" const int64_t jade_slot;
extern "C" const int64_t jade_slot2;
extern "C" const int64_t jade_ret;

#define STENCIL(name) JADE_STENCIL st_##name(PARAMS)
#define NEXT [[clang::musttail]] return jade_next(ARGS)
#define JUMP [[clang::musttail]] return jade_jump(ARGS)
#define EXIT [[clang::musttail]] return jade_exit(ARGS)

// Each register of a kind, for the variants: three levels, as a macro may not use itself.
#define F_A(M, ...) M(0, __VA_ARGS__) M(1, __VA_ARGS__) M(2, __VA_ARGS__) M(3, __VA_ARGS__) M(4, __VA_ARGS__) M(5, __VA_ARGS__) M(6, __VA_ARGS__) M(7, __VA_ARGS__)
#define F_B(M, ...) M(0, __VA_ARGS__) M(1, __VA_ARGS__) M(2, __VA_ARGS__) M(3, __VA_ARGS__) M(4, __VA_ARGS__) M(5, __VA_ARGS__) M(6, __VA_ARGS__) M(7, __VA_ARGS__)
#define F_C(M, ...) M(0, __VA_ARGS__) M(1, __VA_ARGS__) M(2, __VA_ARGS__) M(3, __VA_ARGS__) M(4, __VA_ARGS__) M(5, __VA_ARGS__) M(6, __VA_ARGS__) M(7, __VA_ARGS__)
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
F_A(FBIN_A, addf, +)
F_A(FBIN_A, subf, -)
F_A(FBIN_A, mulf, *)
F_A(FBIN_A, divf, /)

// fa = fb op K, fa = K op fb
#define FBINK_B(b, a, name, op) STENCIL(name##k_##a##_##b) { f##a = f##b op jade_kf; NEXT; } \
                                STENCIL(name##kr_##a##_##b) { f##a = jade_kf op f##b; NEXT; }
#define FBINK_A(a, name, op) F_B(FBINK_B, a, name, op)
F_A(FBINK_A, addf, +)
F_A(FBINK_A, subf, -)
F_A(FBINK_A, mulf, *)
F_A(FBINK_A, divf, /)

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
F_A(FUN_A, x)
#define FSETK(a, ...) STENCIL(setfk_##a) { f##a = jade_kf; NEXT; }
F_A(FSETK, x)

// fa = ib, an int made a float; ia = fb made an int, its whole part, or its floor, ceiling or
// nearest (half away from 0), leaving for the VM where it does not fit (or is NaN).
// (Into a register cleared first: cvtsi2sd keeps the register's upper half, and so would wait
// for whatever last wrote it, a chain of the loop's float work perhaps. Written out, as the
// compiler drops a clearing it sees no use for.)
#define CVT_B(b, a)                                                             \
    STENCIL(cvtif_##a##_##b) {                                                  \
        __asm__("xorps %0, %0\n\tcvtsi2sdq %1, %0" : "=&x"(f##a) : "r"(i##b));  \
        NEXT;                                                                   \
    }
#define CVT_A(a, ...) I_B(CVT_B, a)
F_A(CVT_A, x)
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
    STENCIL(jgtfk_##a) { if (f##a > jade_kf) JUMP; NEXT; }                      \
    STENCIL(jlefk_##a) { if (f##a <= jade_kf) JUMP; NEXT; }
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
    STENCIL(jnltfk_##a) { if (!(f##a < jade_kf)) JUMP; NEXT; }                  \
    STENCIL(jnlefk_##a) { if (!(f##a <= jade_kf)) JUMP; NEXT; }                 \
    STENCIL(jngtfk_##a) { if (!(f##a > jade_kf)) JUMP; NEXT; }                  \
    STENCIL(jngefk_##a) { if (!(f##a >= jade_kf)) JUMP; NEXT; }                 \
    STENCIL(jneqfk_##a) { if (!(f##a == jade_kf)) JUMP; NEXT; }
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
    STENCIL(ltfk_##a) { i9 = f##a < jade_kf; NEXT; }                            \
    STENCIL(lefk_##a) { i9 = f##a <= jade_kf; NEXT; }                           \
    STENCIL(gtfk_##a) { i9 = f##a > jade_kf; NEXT; }                            \
    STENCIL(gefk_##a) { i9 = f##a >= jade_kf; NEXT; }                           \
    STENCIL(eqfk_##a) { i9 = f##a == jade_kf; NEXT; }
F_A(FCMPK, x)

// ---- Ints ----

// ia = K; ia = ib; ia = ia - 1 (a range's count, never below 0); ia = ia + ib wrapping round (a
// range's next value, as the VM steps it).
#define ISETK(a, ...)                                                           \
    STENCIL(setik_##a) { i##a = jade_ki; NEXT; }                                \
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
        if (builtin(i##b, jade_ki, &z)) EXIT;                                   \
        i##a = z;                                                               \
        NEXT;                                                                   \
    }                                                                           \
    STENCIL(name##kr_##a##_##b) {                                               \
        int64_t z;                                                              \
        if (builtin(jade_ki, i##b, &z)) EXIT;                                   \
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
    STENCIL(negi_##a##_##b) {                                                   \
        if (i##b == INT64_MIN) EXIT;                                            \
        i##a = -i##b;                                                           \
        NEXT;                                                                   \
    }                                                                           \
    STENCIL(mini_##a##_##b) { if (i##b < i##a) i##a = i##b; NEXT; }             \
    STENCIL(maxi_##a##_##b) { if (i##b > i##a) i##a = i##b; NEXT; }             \
    STENCIL(divik_##a##_##b) { i##a = i##b / jade_ki; NEXT; }                   \
    STENCIL(modik_##a##_##b) { i##a = i##b % jade_ki; NEXT; }                   \
    STENCIL(andik_##a##_##b) { i##a = i##b & jade_ki; NEXT; }                   \
    STENCIL(orik_##a##_##b) { i##a = i##b | jade_ki; NEXT; }                    \
    STENCIL(shlik_##a##_##b) {                                                  \
        i##a = static_cast<int64_t>(static_cast<uint64_t>(i##b) << jade_ki);    \
        NEXT;                                                                   \
    }                                                                           \
    STENCIL(shrik_##a##_##b) { i##a = i##b >> jade_ki; NEXT; }
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
    STENCIL(jnltik_##a) { if (!(i##a < jade_ki)) JUMP; NEXT; }                  \
    STENCIL(jnleik_##a) { if (!(i##a <= jade_ki)) JUMP; NEXT; }                 \
    STENCIL(jngtik_##a) { if (!(i##a > jade_ki)) JUMP; NEXT; }                  \
    STENCIL(jngeik_##a) { if (!(i##a >= jade_ki)) JUMP; NEXT; }                 \
    STENCIL(jneqik_##a) { if (!(i##a == jade_ki)) JUMP; NEXT; }
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
    STENCIL(ltik_##a) { i9 = i##a < jade_ki; NEXT; }                            \
    STENCIL(leik_##a) { i9 = i##a <= jade_ki; NEXT; }                           \
    STENCIL(gtik_##a) { i9 = i##a > jade_ki; NEXT; }                            \
    STENCIL(geik_##a) { i9 = i##a >= jade_ki; NEXT; }                           \
    STENCIL(eqik_##a) { i9 = i##a == jade_ki; NEXT; }
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
    if (base[jade_slot].kind != kKindBool) EXIT;
    i9 = static_cast<uint8_t>(base[jade_slot].i) != 0;
    NEXT;
}

// ---- Lists and bytes ----

// fa = list's float item ic (or item K): a packed list of floats, none null, the item in
// range; anything else the VM's.
#define IDXF_B(c, a)                                                            \
    STENCIL(idxf_##a##_##c) {                                                   \
        const uint8_t* list = objectIn(base[jade_slot], kTypeList);             \
        if (list == nullptr || !mayRead(list, ctx)) EXIT;                       \
        const uint8_t* pack = packOf(list, kStoreFloats);                       \
        if (pack == nullptr || static_cast<uint64_t>(i##c) >= word<uint64_t>(pack + kPackCount)) EXIT; \
        f##a = word<double>(word<const uint8_t*>(pack + kPackBlock) + i##c * 8); \
        NEXT;                                                                   \
    }
#define IDXF_A(a, ...) I_B(IDXF_B, a)
F_A(IDXF_A, x)
#define IDXFK(a, ...)                                                           \
    STENCIL(idxfk_##a) {                                                        \
        const uint8_t* list = objectIn(base[jade_slot], kTypeList);             \
        if (list == nullptr || !mayRead(list, ctx)) EXIT;                       \
        const uint8_t* pack = packOf(list, kStoreFloats);                       \
        if (pack == nullptr || static_cast<uint64_t>(jade_ki) >= word<uint64_t>(pack + kPackCount)) EXIT; \
        f##a = word<double>(word<const uint8_t*>(pack + kPackBlock) + jade_ki * 8); \
        NEXT;                                                                   \
    }
F_A(IDXFK, x)

// ia = item ic of a whole number's list: a packed list of ints, none null, or bytes; the item in
// range. Which it was the region tells by the object, where it writes the item to its slot
// (storew: a byte from bytes, an int from a list).
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
    const uint8_t* pack = packOf(o, kStoreInts);
    if (pack == nullptr || static_cast<uint64_t>(index) >= word<uint64_t>(pack + kPackCount)) return false;
    out = word<int64_t>(word<const uint8_t*>(pack + kPackBlock) + index * 8);
    return true;
}
#define IDXW_B(c, a)                                                            \
    STENCIL(idxw_##a##_##c) {                                                   \
        int64_t v;                                                              \
        if (!wholeItem(base[jade_slot], i##c, ctx, v)) EXIT;                    \
        i##a = v;                                                               \
        NEXT;                                                                   \
    }
#define IDXW_A(a, ...) I_B(IDXW_B, a)
I_A(IDXW_A, x)
#define IDXWK(a, ...)                                                           \
    STENCIL(idxwk_##a) {                                                        \
        int64_t v;                                                              \
        if (!wholeItem(base[jade_slot], jade_ki, ctx, v)) EXIT;                 \
        i##a = v;                                                               \
        NEXT;                                                                   \
    }
I_A(IDXWK, x)

// Item ic of a list (or bytes) in slot jade_slot set to a value, as the VM's SetIndex does at
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
INLINE uint8_t* byteToChange(const Slot& s, int64_t index, const Ctx* ctx) {
    if (s.kind != kKindObj) return nullptr;
    const uint8_t* o = s.p;
    if (o[kObjType] != kTypeBytes || o[kObjCore] != ctx->core) return nullptr;
    uint8_t* first = word<uint8_t*>(o + kBytesData);
    const uint8_t* end = word<const uint8_t*>(o + kBytesData + 8);
    if (static_cast<uint64_t>(index) >= static_cast<uint64_t>(end - first)) return nullptr;
    return first + index;
}
#define SETF_B(c, v)                                                            \
    STENCIL(setf_##v##_##c) {                                                   \
        uint8_t* at = listToChange(base[jade_slot], i##c, kStoreFloats, ctx);   \
        if (at == nullptr) EXIT;                                                \
        __builtin_memcpy(at, &f##v, 8);                                              \
        NEXT;                                                                   \
    }
#define SETF_A(v, ...) I_B(SETF_B, v)
F_A(SETF_A, x)
#define SETW_B(c, v)                                                            \
    STENCIL(setwi_##v##_##c) {                                                  \
        const Slot& s = base[jade_slot];                                        \
        if (s.kind == kKindObj && s.p[kObjType] == kTypeBytes) {                \
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
        uint8_t* at = byteToChange(base[jade_slot], i##c, ctx);                 \
        if (at == nullptr) EXIT;                                                \
        *at = static_cast<uint8_t>(i##v);                                       \
        NEXT;                                                                   \
    }
#define SETW_A(v, ...) I_B(SETW_B, v)
I_A(SETW_A, x)

// ia (or fa) = a field of item ic of a list of a value type's items, in slot jade_slot, read where
// it is (`items[i].x`, M9.5 B1): the list's items of the class jade_ki, none null, the item in
// range, the field jade_slot2 bytes into it; a byte (idxpb), an int (idxpi) or a float (idxpf).
// Anything else the VM's.
INLINE const uint8_t* itemField(const Slot& s, int64_t index, const Ctx* ctx) {
    if (s.kind != kKindObj) return nullptr;
    const uint8_t* o = s.p;
    if (o[kObjType] != kTypeList || !mayRead(o, ctx)) return nullptr;
    const uint8_t* pack = packOf(o, kStoreItems);
    if (pack == nullptr || word<int64_t>(pack + kPackItemClass) != jade_ki) return nullptr;
    if (static_cast<uint64_t>(index) >= word<uint64_t>(pack + kPackCount)) return nullptr;
    return word<const uint8_t*>(pack + kPackBlock) + index * static_cast<int64_t>(word<uint32_t>(pack + kPackWidth)) + jade_slot2;
}
#define IDXP_B(c, a)                                                            \
    STENCIL(idxpb_##a##_##c) {                                                  \
        const uint8_t* at = itemField(base[jade_slot], i##c, ctx);              \
        if (at == nullptr) EXIT;                                                \
        i##a = *at;                                                             \
        NEXT;                                                                   \
    }                                                                           \
    STENCIL(idxpi_##a##_##c) {                                                  \
        const uint8_t* at = itemField(base[jade_slot], i##c, ctx);              \
        if (at == nullptr) EXIT;                                                \
        i##a = word<int64_t>(at);                                               \
        NEXT;                                                                   \
    }
#define IDXP_A(a, ...) I_B(IDXP_B, a)
I_A(IDXP_A, x)
#define IDXPF_B(c, a)                                                           \
    STENCIL(idxpf_##a##_##c) {                                                  \
        const uint8_t* at = itemField(base[jade_slot], i##c, ctx);              \
        if (at == nullptr) EXIT;                                                \
        f##a = word<double>(at);                                                \
        NEXT;                                                                   \
    }
#define IDXPF_A(a, ...) I_B(IDXPF_B, a)
F_A(IDXPF_A, x)

// Slot jade_slot2 = a Ref to item ic of the list of a value type's items in slot jade_slot, none
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
INLINE const uint8_t* itemsList(const Slot& s, const Ctx* ctx) {
    if (s.kind != kKindObj) return nullptr;
    const uint8_t* o = s.p;
    if (o[kObjType] != kTypeList || !mayRead(o, ctx)) return nullptr;
    return packOf(o, kStoreItems) != nullptr ? o : nullptr;
}
#define IREF(c, ...)                                                            \
    STENCIL(iref_##c) {                                                         \
        const uint8_t* list = itemsList(base[jade_slot], ctx);                  \
        if (list == nullptr) EXIT;                                              \
        const uint8_t* pack = word<const uint8_t*>(list + kListPack);           \
        if (static_cast<uint64_t>(i##c) >= word<uint64_t>(pack + kPackCount)) EXIT; \
        putValue(&base[jade_slot2], kKindRef | static_cast<uint64_t>(i##c) << 32, reinterpret_cast<uint64_t>(list)); \
        NEXT;                                                                   \
    }
I_A(IREF, x)
// Slot (jade_slot2 >> 8 & 0xFFFFFF) = a copy of item ic of the list in slot jade_slot, a value kept
// in the value itself (a Color): the list's items of the class jade_ki, kept with no type of their
// own, none null, the item in range; the item's first (jade_slot2 & 0xFF) bytes, and the type's
// number (jade_slot2 >> 32). Anything else the VM's.
#define IDXSM(c, ...)                                                           \
    STENCIL(idxsm_##c) {                                                        \
        const uint8_t* list = itemsList(base[jade_slot], ctx);                  \
        if (list == nullptr) EXIT;                                              \
        const uint8_t* pack = word<const uint8_t*>(list + kListPack);           \
        if (word<int64_t>(pack + kPackItemClass) != jade_ki || word<const void*>(pack + kPackItemType) != nullptr || \
            word<uint32_t>(pack + kPackTypeSlot) != 0 || static_cast<uint64_t>(i##c) >= word<uint64_t>(pack + kPackCount)) \
            EXIT;                                                               \
        const uint64_t what = static_cast<uint64_t>(jade_slot2);                \
        const uint32_t size = static_cast<uint32_t>(what & 0xFF), id = static_cast<uint32_t>(what >> 32); \
        const uint8_t* from = word<const uint8_t*>(pack + kPackBlock) + i##c * static_cast<int64_t>(word<uint32_t>(pack + kPackWidth)); \
        uint64_t bits = 0;                                                      \
        for (uint32_t k = 0; k < size; k++) bits |= static_cast<uint64_t>(from[k]) << (8 * k); \
        putValue(&base[(what >> 8) & 0xFFFFFF], kKindSmall | static_cast<uint64_t>(id) << 32, bits); \
        NEXT;                                                                   \
    }
I_A(IDXSM, x)
// KeepLocal where slot jade_slot is in memory: nothing, unless it holds a Ref to a place of the
// frame's, which the VM copies.
STENCIL(keepl) {
    const Slot& s = base[jade_slot];
    if ((word<uint64_t>(reinterpret_cast<const uint8_t*>(&s)) & 0xFF) == kKindRef && s.p[kObjType] == kTypePlaces) EXIT;
    NEXT;
}
// ia (or fa) = a field of the value of a value type in slot jade_slot: a Small value of the type
// numbered (jade_slot2 >> 32), or a Ref, none deep, to an item of a list of the class jade_ki's
// items; the field (jade_slot2 & 0xFFFFFFFF) bytes into it. Anything else the VM's.
INLINE const uint8_t* valueField(const Slot& s, const Ctx* ctx) {
    // Its kind, how deep, where in the item and the index or type number, in one word.
    const uint64_t head = word<uint64_t>(reinterpret_cast<const uint8_t*>(&s));
    const uint32_t offset = static_cast<uint32_t>(jade_slot2);
    if ((head & 0xFF) == kKindSmall) {
        if ((head >> 32) != static_cast<uint64_t>(jade_slot2) >> 32) return nullptr;
        return reinterpret_cast<const uint8_t*>(&s.i) + offset;
    }
    if ((head & 0xFFFFFFFF) != kKindRef) return nullptr;  // a Ref none deep, at its item's start
    const uint8_t* o = s.p;
    if (o[kObjType] != kTypeList || !mayRead(o, ctx)) return nullptr;
    const uint8_t* pack = packOf(o, kStoreItems);
    if (pack == nullptr || word<int64_t>(pack + kPackItemClass) != jade_ki) return nullptr;
    const uint64_t index = head >> 32;
    if (index >= word<uint64_t>(pack + kPackCount)) return nullptr;
    return word<const uint8_t*>(pack + kPackBlock) + index * word<uint32_t>(pack + kPackWidth) + offset;
}
#define VALP(a, ...)                                                            \
    STENCIL(valpb_##a) {                                                        \
        const uint8_t* at = valueField(base[jade_slot], ctx);                   \
        if (at == nullptr) EXIT;                                                \
        i##a = *at;                                                             \
        NEXT;                                                                   \
    }                                                                           \
    STENCIL(valpi_##a) {                                                        \
        const uint8_t* at = valueField(base[jade_slot], ctx);                   \
        if (at == nullptr) EXIT;                                                \
        i##a = word<int64_t>(at);                                               \
        NEXT;                                                                   \
    }
I_A(VALP, x)
#define VALPF(a, ...)                                                           \
    STENCIL(valpf_##a) {                                                        \
        const uint8_t* at = valueField(base[jade_slot], ctx);                   \
        if (at == nullptr) EXIT;                                                \
        f##a = word<double>(at);                                                \
        NEXT;                                                                   \
    }
F_A(VALPF, x)

// ---- Slots, and going on ----

// A register read from its slot, as a region begins; written back as it leaves (kind and
// payload in one store, as the VM writes a slot): an int, a byte, a float, the flag register's
// bool; a whole number read from a list or bytes (storew: a byte where the object in jade_slot2
// is bytes, an int otherwise); a slot copied whole from another (copyslot: what a pushed object
// stands for).
#define ILOAD(a, ...)                                                           \
    STENCIL(loadi_##a) { i##a = base[jade_slot].i; NEXT; }                      \
    STENCIL(storei_##a) {                                                       \
        storeSlot(base, jade_slot, kKindInt, static_cast<uint64_t>(i##a));      \
        NEXT;                                                                   \
    }                                                                           \
    STENCIL(storeby_##a) {                                                      \
        storeSlot(base, jade_slot, kKindByte, static_cast<uint64_t>(i##a));     \
        NEXT;                                                                   \
    }                                                                           \
    STENCIL(storew_##a) {                                                       \
        const Slot& o = base[jade_slot2];                                       \
        const bool bytes = o.kind == kKindObj && o.p[kObjType] == kTypeBytes;   \
        storeSlot(base, jade_slot, bytes ? kKindByte : kKindInt, static_cast<uint64_t>(i##a)); \
        NEXT;                                                                   \
    }
I_A(ILOAD, x)
#define FLOAD(a, ...)                                                           \
    STENCIL(loadf_##a) { f##a = base[jade_slot].d; NEXT; }                      \
    STENCIL(storef_##a) {                                                       \
        uint64_t bits;                                                          \
        __builtin_memcpy(&bits, &f##a, 8);                                           \
        storeSlot(base, jade_slot, kKindFloat, bits);                           \
        NEXT;                                                                   \
    }
F_A(FLOAD, x)
STENCIL(storeb) {
    storeSlot(base, jade_slot, kKindBool, i9 != 0);
    NEXT;
}
STENCIL(copyslot) {
    __builtin_memcpy(&base[jade_slot], &base[jade_slot2], sizeof(Slot));
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
STENCIL(ret) { return jade_ret; }

// A slot the region has not written, checked to hold what an untyped instruction is taken to
// read in it: a whole number, or a float. Where it does not, the VM does the instruction.
STENCIL(chki) {
    const uint8_t k = base[jade_slot].kind;
    if (k != kKindInt && k != kKindByte) EXIT;
    NEXT;
}
STENCIL(chkf) {
    if (base[jade_slot].kind != kKindFloat) EXIT;
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
        i##a = static_cast<int64_t>(static_cast<uint64_t>(i##a) + static_cast<uint64_t>(jade_ki)); \
        NEXT;                                                                   \
    }
I_A(IADDWK, x)
// A slot checked to hold an int (not a byte): what the VM's range asks of its bounds.
STENCIL(chkint) {
    if (base[jade_slot].kind != kKindInt) EXIT;
    NEXT;
}
// The object a whole number was read from checked to be no bytes: the number an int, not a byte.
STENCIL(chklist) {
    const Slot& o = base[jade_slot];
    if (o.kind == kKindObj && o.p[kObjType] == kTypeBytes) EXIT;
    NEXT;
}

// fa = a number read from a slot, an int made a float: the other operand of a float's
// arithmetic, whatever it is (an int beside a float makes a float); anything else the VM's.
#define NUMF(a, ...)                                                            \
    STENCIL(numf_##a) {                                                         \
        const Slot& v = base[jade_slot];                                        \
        if (v.kind == kKindFloat) f##a = v.d;                                   \
        else if (v.kind == kKindInt) f##a = static_cast<double>(v.i);           \
        else EXIT;                                                              \
        NEXT;                                                                   \
    }
F_A(NUMF, x)
// A slot checked to hold a byte (what the region takes a slot entered as a byte to hold).
STENCIL(chkbyk) {
    if (base[jade_slot].kind != kKindByte) EXIT;
    NEXT;
}

// ---- Fields ----

// A field of the object of a class in slot jade_slot, where the cache word at jade_ki (an
// address: the place's cache, which the VM fills) says it is, its layout the one the cache
// saw; read where the object's core may read it (or change it, `change`: its own core's).
// Null where not: the VM's.
INLINE uint8_t* fieldAt(const Slot& s, const Ctx* ctx, bool change, uint64_t& seen) {
    if (s.kind != kKindObj || s.p[kObjType] != kTypeInstance) return nullptr;
    seen = *reinterpret_cast<const volatile uint64_t*>(jade_ki);
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
        const uint8_t* at = fieldAt(base[jade_slot], ctx, false, seen);         \
        if (at == nullptr || !numberField(at, seen, kKindFloat, bits)) EXIT;    \
        __builtin_memcpy(&f##a, &bits, 8);                                           \
        NEXT;                                                                   \
    }                                                                           \
    STENCIL(setpf_##a) {                                                        \
        uint64_t bits;                                                          \
        __builtin_memcpy(&bits, &f##a, 8);                                           \
        if (!setNumberField(base[jade_slot], ctx, kKindFloat, kTypeIdFloat, bits)) EXIT; \
        NEXT;                                                                   \
    }
F_A(GETPF, x)
#define GETPI(a, ...)                                                           \
    STENCIL(getpi_##a) {                                                        \
        uint64_t seen, bits;                                                    \
        const uint8_t* at = fieldAt(base[jade_slot], ctx, false, seen);         \
        if (at == nullptr || !numberField(at, seen, kKindInt, bits)) EXIT;      \
        i##a = static_cast<int64_t>(bits);                                      \
        NEXT;                                                                   \
    }                                                                           \
    STENCIL(setpi_##a) {                                                        \
        if (!setNumberField(base[jade_slot], ctx, kKindInt, kTypeIdInt, static_cast<uint64_t>(i##a))) EXIT; \
        NEXT;                                                                   \
    }
I_A(GETPI, x)
// ia = the size of the list or bytes in slot jade_slot, as `size` gives it (a while loop's
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
        if (!sizeOf(base[jade_slot], ctx, v)) EXIT;                             \
        i##a = v;                                                               \
        NEXT;                                                                   \
    }
I_A(SIZEW, x)

// A field of any kind copied to slot jade_slot2, as the VM copies it (no value held elsewhere).
STENCIL(getpv) {
    uint64_t seen;
    const uint8_t* at = fieldAt(base[jade_slot], ctx, false, seen);
    if (at == nullptr) EXIT;
    if ((seen & 0x80) != 0) {
        uint64_t bits;
        __builtin_memcpy(&bits, at, 8);
        storeSlot(base, jade_slot2, static_cast<uint8_t>(seen & 3), bits);
        NEXT;
    }
    if (at[0] == kKindRef || at[0] == kKindUndefined) EXIT;
    __builtin_memcpy(&base[jade_slot2], at, sizeof(Slot));
    NEXT;
}

// ---- A for over a list ----

// The iterator in slot jade_slot over a list: the iterator, and the list in `list`.
INLINE uint8_t* listIterator(const Slot& s, const uint8_t*& list) {
    if (s.kind != kKindObj || s.p[kObjType] != kTypeIterator) return nullptr;
    const Slot& source = *reinterpret_cast<const Slot*>(s.p + kIterSource);
    if (source.kind != kKindObj || source.p[kObjType] != kTypeList) return nullptr;
    list = source.p;
    return const_cast<uint8_t*>(s.p);
}
// The next item of a list kept as values, to slot jade_slot2; to the jump's target where there
// are no more.
STENCIL(iterv) {
    const uint8_t* list;
    uint8_t* it = listIterator(base[jade_slot], list);
    if (it == nullptr || word<const uint8_t*>(list + kListPack) != nullptr) EXIT;
    const Slot* first = word<const Slot*>(list + kListItems);
    const Slot* last = word<const Slot*>(list + kListItems + 8);
    const uint64_t index = word<uint64_t>(it + kIterIndex);
    if (index >= static_cast<uint64_t>(last - first)) JUMP;
    __builtin_memcpy(&base[jade_slot2], first + index, sizeof(Slot));
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
        const uint8_t* pack = packedIterator(base[jade_slot], kStoreFloats, ctx, it); \
        if (pack == nullptr) EXIT;                                              \
        const uint64_t index = word<uint64_t>(it + kIterIndex);                 \
        if (index >= word<uint64_t>(pack + kPackCount)) JUMP;                   \
        f##a = word<double>(word<const uint8_t*>(pack + kPackBlock) + index * 8); \
        const uint64_t next = index + 1;                                        \
        __builtin_memcpy(it + kIterIndex, &next, 8);                                 \
        NEXT;                                                                   \
    }
F_A(ITERF, x)
#define ITERI(a, ...)                                                           \
    STENCIL(iteri_##a) {                                                        \
        uint8_t* it;                                                            \
        const uint8_t* pack = packedIterator(base[jade_slot], kStoreInts, ctx, it); \
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
// jade_slot2, as IterNextRef gives it (M9.5 B1).
STENCIL(iterref) {
    const uint8_t* list;
    uint8_t* it = listIterator(base[jade_slot], list);
    if (it == nullptr || list == ctx->band || packOf(list, kStoreItems) == nullptr) EXIT;
    const uint64_t index = word<uint64_t>(it + kIterIndex);
    if (index >= word<uint64_t>(word<const uint8_t*>(list + kListPack) + kPackCount)) JUMP;
    putValue(&base[jade_slot2], kKindRef | index << 32, reinterpret_cast<uint64_t>(list));
    const uint64_t next = index + 1;
    __builtin_memcpy(it + kIterIndex, &next, 8);
    NEXT;
}
// The next item of a list of the class jade_ki's items kept in the value itself (a Color), none
// null and none with a type of its own: a copy of its first (jade_slot2 & 0xFF) bytes, of the
// type numbered (jade_slot2 >> 32), to slot (jade_slot2 >> 8 & 0xFFFFFF) (M9.5 B1).
STENCIL(itersm) {
    uint8_t* it;
    const uint8_t* pack = packedIterator(base[jade_slot], kStoreItems, ctx, it);
    if (pack == nullptr || word<int64_t>(pack + kPackItemClass) != jade_ki || word<const void*>(pack + kPackItemType) != nullptr ||
        word<uint32_t>(pack + kPackTypeSlot) != 0)
        EXIT;
    const uint64_t index = word<uint64_t>(it + kIterIndex);
    if (index >= word<uint64_t>(pack + kPackCount)) JUMP;
    const uint64_t what = static_cast<uint64_t>(jade_slot2);
    const uint8_t* from = word<const uint8_t*>(pack + kPackBlock) + index * word<uint32_t>(pack + kPackWidth);
    uint64_t bits = 0;
    for (uint32_t k = 0; k < (what & 0xFF); k++) bits |= static_cast<uint64_t>(from[k]) << (8 * k);
    putValue(&base[(what >> 8) & 0xFFFFFF], kKindSmall | (what >> 32) << 32, bits);
    const uint64_t next = index + 1;
    __builtin_memcpy(it + kIterIndex, &next, 8);
    NEXT;
}

// null, true or false pushed: written to slot jade_slot as the VM writes them.
STENCIL(setnull) {
    storeSlot(base, jade_slot, 0, 0);
    NEXT;
}
STENCIL(settrue) {
    storeSlot(base, jade_slot, kKindBool, 1);
    NEXT;
}
STENCIL(setfalse) {
    storeSlot(base, jade_slot, kKindBool, 0);
    NEXT;
}

// ---- Lists looked at once, as a region starts ----

// A list's or bytes' items, looked at once where nothing in the region can change where they are
// or how many (no instruction the VM's in it; and the region is entered afresh after each way
// out): in the context's words 4k to 4k+3, its items' address, how many may be read, how many
// changed (0 where the object may not be, or is not what the region reads as), and what they are
// (1 ints, 2 floats, 3 bytes). slot: the object; jade_ki: k.
INLINE void lookAt(const Slot& s, Ctx* ctx, int64_t k, bool floats) {
    uint64_t* h = ctx->lists + 4 * k;
    h[0] = h[1] = h[2] = h[3] = 0;
    if (s.kind != kKindObj) return;
    const uint8_t* o = s.p;
    if (o[kObjType] == kTypeList) {
        const uint8_t* pack = packOf(o, floats ? kStoreFloats : kStoreInts);
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
    lookAt(base[jade_slot], ctx, jade_ki, true);
    NEXT;
}
STENCIL(lookw) {
    lookAt(base[jade_slot], ctx, jade_ki, false);
    NEXT;
}
// Items read and written through what was looked at (k in jade_slot): the index checked
// against the count, nothing else.
#define HIDXF_B(c, a)                                                           \
    STENCIL(hidxf_##a##_##c) {                                                  \
        const uint64_t* h = ctx->lists + 4 * jade_slot;                         \
        if (static_cast<uint64_t>(i##c) >= h[1]) EXIT;                          \
        f##a = reinterpret_cast<const double*>(h[0])[i##c];                     \
        NEXT;                                                                   \
    }                                                                           \
    STENCIL(hsetf_##a##_##c) {                                                  \
        const uint64_t* h = ctx->lists + 4 * jade_slot;                         \
        if (static_cast<uint64_t>(i##c) >= h[2]) EXIT;                          \
        reinterpret_cast<double*>(h[0])[i##c] = f##a;                           \
        NEXT;                                                                   \
    }
#define HIDXF_A(a, ...) I_B(HIDXF_B, a)
F_A(HIDXF_A, x)
#define HIDXFK(a, ...)                                                          \
    STENCIL(hidxfk_##a) {                                                       \
        const uint64_t* h = ctx->lists + 4 * jade_slot;                         \
        if (static_cast<uint64_t>(jade_ki) >= h[1]) EXIT;                       \
        f##a = reinterpret_cast<const double*>(h[0])[jade_ki];                  \
        NEXT;                                                                   \
    }
F_A(HIDXFK, x)
#define HIDXW_B(c, a)                                                           \
    STENCIL(hidxw_##a##_##c) {                                                  \
        const uint64_t* h = ctx->lists + 4 * jade_slot;                         \
        if (static_cast<uint64_t>(i##c) >= h[1]) EXIT;                          \
        i##a = h[3] == 3 ? reinterpret_cast<const uint8_t*>(h[0])[i##c] : reinterpret_cast<const int64_t*>(h[0])[i##c]; \
        NEXT;                                                                   \
    }                                                                           \
    STENCIL(hsetwi_##a##_##c) {                                                 \
        const uint64_t* h = ctx->lists + 4 * jade_slot;                         \
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
        const uint64_t* h = ctx->lists + 4 * jade_slot;                         \
        if (static_cast<uint64_t>(i##c) >= h[2] || h[3] != 3) EXIT;             \
        reinterpret_cast<uint8_t*>(h[0])[i##c] = static_cast<uint8_t>(i##a);    \
        NEXT;                                                                   \
    }
#define HIDXW_A(a, ...) I_B(HIDXW_B, a)
I_A(HIDXW_A, x)
#define HIDXWK(a, ...)                                                          \
    STENCIL(hidxwk_##a) {                                                       \
        const uint64_t* h = ctx->lists + 4 * jade_slot;                         \
        if (static_cast<uint64_t>(jade_ki) >= h[1]) EXIT;                       \
        i##a = h[3] == 3 ? reinterpret_cast<const uint8_t*>(h[0])[jade_ki] : reinterpret_cast<const int64_t*>(h[0])[jade_ki]; \
        NEXT;                                                                   \
    }
I_A(HIDXWK, x)

// A list of a value type's items looked at once (M9.5 B1), as lookAt: in the context's words 4k
// to 4k+3, its items' address, how many may be read, how many changed, and the list itself (what
// a Ref read later must be into). Only a list of the class jade_ki's items, none null, each
// (jade_slot2 >> 32) bytes wide; anything else leaves all four 0, and every step through them
// leaves for the VM. jade_slot: the list; jade_slot2 & 0xFF: k.
INLINE void lookAtItems(const Slot& s, Ctx* ctx) {
    uint64_t* h = ctx->lists + 4 * (static_cast<uint64_t>(jade_slot2) & 0xFF);
    h[0] = h[1] = h[2] = h[3] = 0;
    if (s.kind == kKindObj && s.p[kObjType] == kTypeList) {
        const uint8_t* o = s.p;
        const uint8_t* pack = packOf(o, kStoreItems);
        if (pack != nullptr && word<int64_t>(pack + kPackItemClass) == jade_ki &&
            word<uint32_t>(pack + kPackWidth) == static_cast<uint64_t>(jade_slot2) >> 32) {
            const uint64_t count = word<uint64_t>(pack + kPackCount);
            h[0] = word<uint64_t>(pack + kPackBlock);
            if (mayRead(o, ctx)) h[1] = count;
            if (o[kObjCore] == ctx->core && o[kObjFrozen] == 0) h[2] = count;
            h[3] = reinterpret_cast<uint64_t>(o);
        }
    }
}
STENCIL(looki) {
    lookAtItems(base[jade_slot], ctx);
    NEXT;
}
// The same for the list the iterator in slot jade_slot goes through (a for over it).
STENCIL(lookit) {
    const Slot& s = base[jade_slot];
    if (s.kind == kKindObj && s.p[kObjType] == kTypeIterator) {
        lookAtItems(*reinterpret_cast<const Slot*>(s.p + kIterSource), ctx);
    } else {
        uint64_t* h = ctx->lists + 4 * (static_cast<uint64_t>(jade_slot2) & 0xFF);
        h[0] = h[1] = h[2] = h[3] = 0;
    }
    NEXT;
}
// Through a list of items looked at once (k in jade_slot): ia (or fa) = item ic's field, the
// field (jade_slot2 & 0xFFFFFFFF) bytes into an item (jade_slot2 >> 32) bytes wide; the index
// checked against the count, nothing else.
#define HIDXP_B(c, a)                                                           \
    STENCIL(hidxpb_##a##_##c) {                                                 \
        const uint64_t* h = ctx->lists + 4 * jade_slot;                         \
        if (static_cast<uint64_t>(i##c) >= h[1]) EXIT;                          \
        i##a = reinterpret_cast<const uint8_t*>(h[0])[i##c * (static_cast<uint64_t>(jade_slot2) >> 32) + static_cast<uint32_t>(jade_slot2)]; \
        NEXT;                                                                   \
    }                                                                           \
    STENCIL(hidxpi_##a##_##c) {                                                 \
        const uint64_t* h = ctx->lists + 4 * jade_slot;                         \
        if (static_cast<uint64_t>(i##c) >= h[1]) EXIT;                          \
        i##a = word<int64_t>(reinterpret_cast<const uint8_t*>(h[0]) + i##c * (static_cast<uint64_t>(jade_slot2) >> 32) + static_cast<uint32_t>(jade_slot2)); \
        NEXT;                                                                   \
    }
#define HIDXP_A(a, ...) I_B(HIDXP_B, a)
I_A(HIDXP_A, x)
#define HIDXPF_B(c, a)                                                          \
    STENCIL(hidxpf_##a##_##c) {                                                 \
        const uint64_t* h = ctx->lists + 4 * jade_slot;                         \
        if (static_cast<uint64_t>(i##c) >= h[1]) EXIT;                          \
        f##a = word<double>(reinterpret_cast<const uint8_t*>(h[0]) + i##c * (static_cast<uint64_t>(jade_slot2) >> 32) + static_cast<uint32_t>(jade_slot2)); \
        NEXT;                                                                   \
    }
#define HIDXPF_A(a, ...) I_B(HIDXPF_B, a)
F_A(HIDXPF_A, x)
// Slot jade_slot2 = a Ref to item ic of a list of items looked at once (k in jade_slot).
#define HIREF(c, ...)                                                           \
    STENCIL(hiref_##c) {                                                        \
        const uint64_t* h = ctx->lists + 4 * jade_slot;                         \
        if (static_cast<uint64_t>(i##c) >= h[1]) EXIT;                          \
        putValue(&base[jade_slot2], kKindRef | static_cast<uint64_t>(i##c) << 32, h[3]); \
        NEXT;                                                                   \
    }
I_A(HIREF, x)
// ia (or fa) = a field of the value in slot jade_slot, as valp: a Small value of the type numbered
// (jade_slot2 >> 32), or a Ref, none deep, to an item of the list looked at once as k (jade_ki);
// the field (jade_slot2 & 0xFFFF) bytes into an item ((jade_slot2 >> 16) & 0xFFFF) bytes wide.
INLINE const uint8_t* lookedField(const Slot& s, const Ctx* ctx) {
    const uint64_t head = word<uint64_t>(reinterpret_cast<const uint8_t*>(&s));
    const uint64_t what = static_cast<uint64_t>(jade_slot2);
    if ((head & 0xFF) == kKindSmall) {
        if ((head >> 32) != what >> 32) return nullptr;
        return reinterpret_cast<const uint8_t*>(&s.i) + (what & 0xFFFF);
    }
    const uint64_t* h = ctx->lists + 4 * jade_ki;
    if ((head & 0xFFFFFFFF) != kKindRef || s.p != reinterpret_cast<const uint8_t*>(h[3]) || (head >> 32) >= h[1]) return nullptr;
    return reinterpret_cast<const uint8_t*>(h[0]) + (head >> 32) * ((what >> 16) & 0xFFFF) + (what & 0xFFFF);
}
#define HVALP(a, ...)                                                           \
    STENCIL(hvalpb_##a) {                                                       \
        const uint8_t* at = lookedField(base[jade_slot], ctx);                  \
        if (at == nullptr) EXIT;                                                \
        i##a = *at;                                                             \
        NEXT;                                                                   \
    }                                                                           \
    STENCIL(hvalpi_##a) {                                                       \
        const uint8_t* at = lookedField(base[jade_slot], ctx);                  \
        if (at == nullptr) EXIT;                                                \
        i##a = word<int64_t>(at);                                               \
        NEXT;                                                                   \
    }
I_A(HVALP, x)
#define HVALPF(a, ...)                                                          \
    STENCIL(hvalpf_##a) {                                                       \
        const uint8_t* at = lookedField(base[jade_slot], ctx);                  \
        if (at == nullptr) EXIT;                                                \
        f##a = word<double>(at);                                                \
        NEXT;                                                                   \
    }
F_A(HVALPF, x)

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
    kLoopWCopy, kLoopWFill, kLoopWSum, kLoopWMap, kLoopWLookup,
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

INLINE void runLoop(Slot* base, Ctx* ctx) {
    uint64_t* t = ctx->t;
    // A variable the loop reads and never writes, checked as its steps check it each time round
    // (14: its slot, or all ones; 15: 1 a float, 2 an int, 3 an int or a byte).
    if (t[14] != ~uint64_t{0}) {
        const uint8_t k = base[t[14]].kind;
        if (t[15] == 1 ? k != kKindFloat : t[15] == 2 ? k != kKindInt : (k != kKindInt && k != kKindByte)) return;
    }
    const int64_t next = static_cast<int64_t>(t[1]), left = static_cast<int64_t>(t[2]);
    if (next < 0 || left <= 0 || next > INT64_MAX - left) return;
    const int64_t end = next + left;
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
            if (!reads(a, 0)) return;
            int64_t acc = static_cast<int64_t>(t[3]);
            if (a.kind() == 3) {
                const uint8_t* x = a.bytes();
                for (int64_t j = next; j < end; j++)
                    if (__builtin_add_overflow(acc, static_cast<int64_t>(x[j]), &acc)) {
                        done = j - next;
                        break;
                    }
            } else {
                const int64_t* x = a.ints();
                for (int64_t j = next; j < end; j++) {
                    int64_t s;
                    if (__builtin_add_overflow(acc, x[j], &s)) {
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
        default: return;
    }
    t[1] = static_cast<uint64_t>(next + done);
    t[2] = static_cast<uint64_t>(left - done);
}
#undef reads
#undef writes
STENCIL(tloop) {
    runLoop(base, ctx);
    NEXT;
}
// The template's words set: a constant (word jade_slot = jade_ki), a register stored, or loaded.
STENCIL(ctxk) {
    ctx->t[jade_slot] = static_cast<uint64_t>(jade_ki);
    NEXT;
}
#define CTXI(a, ...)                                                            \
    STENCIL(ctxsi_##a) {                                                        \
        ctx->t[jade_slot] = static_cast<uint64_t>(i##a);                        \
        NEXT;                                                                   \
    }                                                                           \
    STENCIL(ctxli_##a) {                                                        \
        i##a = static_cast<int64_t>(ctx->t[jade_slot]);                         \
        NEXT;                                                                   \
    }
I_A(CTXI, x)
#define CTXF(a, ...)                                                            \
    STENCIL(ctxsf_##a) {                                                        \
        __builtin_memcpy(&ctx->t[jade_slot], &f##a, 8);                              \
        NEXT;                                                                   \
    }                                                                           \
    STENCIL(ctxlf_##a) {                                                        \
        __builtin_memcpy(&f##a, &ctx->t[jade_slot], 8);                              \
        NEXT;                                                                   \
    }
F_A(CTXF, x)
