// The register stencils of native code (M9.5, docs/native-plan.html): each a small function
// compiled by Clang when jade.exe is built, never called as itself, but read out of the object
// file by tools/stencils and copied, end to end, into a region's machine code as it runs.
//
// Every stencil has the same parameters: the frame's slots, then the region's registers, ints
// i0 to i5 and floats f0 to f7, which `preserve_none` keeps in the processor's registers. A
// stencil does one thing to them and goes on to the next with `musttail`, which Clang makes a
// plain jump; a register it does not touch is passed through as it is.
//
// Its gaps are references to the names below, filled in when it is copied:
//   jade_next, jade_jump, jade_exit   where it goes on: the stencil after it, a jump's target,
//                                     the region's way out to the VM
//   jade_kf, jade_ki                  a float or an int constant
//   jade_slot                         a slot of the frame
//   jade_ret                          what the region gives back: where the VM goes on
//   jade_attention                    where the VM's flag for a collection or another thread is
// Each is reached through a word placed after the region's code (a 32-bit offset from the
// instruction), so that no stencil needs an absolute address.
//
// A stencil's name says what it does and to which registers: addf_4_4_5 is f4 = f4 + f5.

#include <cstdint>
#include <cstring>

namespace {
struct Slot {  // a frame's slot, as the VM keeps it: a kind and an 8-byte payload
    uint8_t kind;
    uint8_t pad[7];
    union {
        int64_t i;
        double d;
    };
};
constexpr uint8_t kInt = 2, kFloat = 3;
}  // namespace

#define PARAMS                                                                                                         \
    Slot *base, int64_t i0, int64_t i1, int64_t i2, int64_t i3, int64_t i4, int64_t i5, double f0, double f1, double f2, \
        double f3, double f4, double f5, double f6, double f7
#define ARGS base, i0, i1, i2, i3, i4, i5, f0, f1, f2, f3, f4, f5, f6, f7
#define JADE_STENCIL extern "C" __attribute__((preserve_none)) int64_t

JADE_STENCIL jade_next(PARAMS);
JADE_STENCIL jade_jump(PARAMS);
JADE_STENCIL jade_exit(PARAMS);
extern "C" const double jade_kf;
extern "C" const int64_t jade_ki;
extern "C" const int64_t jade_slot;
extern "C" const int64_t jade_ret;
extern "C" const volatile uint8_t* const jade_attention;

#define STENCIL(name) JADE_STENCIL st_##name(PARAMS)
#define NEXT [[clang::musttail]] return jade_next(ARGS)
#define JUMP [[clang::musttail]] return jade_jump(ARGS)
#define EXIT [[clang::musttail]] return jade_exit(ARGS)

// Each register of a kind, for the variants: three levels, as a macro may not use itself.
#define F_A(M, ...) M(0, __VA_ARGS__) M(1, __VA_ARGS__) M(2, __VA_ARGS__) M(3, __VA_ARGS__) M(4, __VA_ARGS__) M(5, __VA_ARGS__) M(6, __VA_ARGS__) M(7, __VA_ARGS__)
#define F_B(M, ...) M(0, __VA_ARGS__) M(1, __VA_ARGS__) M(2, __VA_ARGS__) M(3, __VA_ARGS__) M(4, __VA_ARGS__) M(5, __VA_ARGS__) M(6, __VA_ARGS__) M(7, __VA_ARGS__)
#define F_C(M, ...) M(0, __VA_ARGS__) M(1, __VA_ARGS__) M(2, __VA_ARGS__) M(3, __VA_ARGS__) M(4, __VA_ARGS__) M(5, __VA_ARGS__) M(6, __VA_ARGS__) M(7, __VA_ARGS__)
#define I_A(M, ...) M(0, __VA_ARGS__) M(1, __VA_ARGS__) M(2, __VA_ARGS__) M(3, __VA_ARGS__) M(4, __VA_ARGS__) M(5, __VA_ARGS__)
#define I_B(M, ...) M(0, __VA_ARGS__) M(1, __VA_ARGS__) M(2, __VA_ARGS__) M(3, __VA_ARGS__) M(4, __VA_ARGS__) M(5, __VA_ARGS__)

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

// fa = fb; fa = K
#define FMOV_B(b, a) STENCIL(movf_##a##_##b) { f##a = f##b; NEXT; }
#define FMOV_A_(a, ...) F_B(FMOV_B, a)
F_A(FMOV_A_, x)
#define FSETK(a, ...) STENCIL(setfk_##a) { f##a = jade_kf; NEXT; }
F_A(FSETK, x)

// fa = ib, an int made a float
#define CVT_B(b, a) STENCIL(cvtif_##a##_##b) { f##a = static_cast<double>(i##b); NEXT; }
#define CVT_A(a, ...) I_B(CVT_B, a)
F_A(CVT_A, x)

// Jumps on a float and a constant: where `fa op K`, to the jump's target.
#define FJUMPK(a, ...)                                                          \
    STENCIL(jgtfk_##a) { if (f##a > jade_kf) JUMP; NEXT; }                      \
    STENCIL(jlefk_##a) { if (f##a <= jade_kf) JUMP; NEXT; }                     \
    STENCIL(jltfk_##a) { if (f##a < jade_kf) JUMP; NEXT; }                      \
    STENCIL(jgefk_##a) { if (f##a >= jade_kf) JUMP; NEXT; }
F_A(FJUMPK, x)

// ---- Ints ----

// ia = K; ia = ib
#define ISETK(a, ...) STENCIL(setik_##a) { i##a = jade_ki; NEXT; }
I_A(ISETK, x)
#define IMOV_B(b, a) STENCIL(movi_##a##_##b) { i##a = i##b; NEXT; }
#define IMOV_A(a, ...) I_B(IMOV_B, a)
I_A(IMOV_A, x)

// ia = ia + K, an overflow leaving for the VM, which reports it
#define IADDK(a, ...)                                                           \
    STENCIL(addik_##a) {                                                        \
        int64_t z;                                                              \
        if (__builtin_add_overflow(i##a, jade_ki, &z)) EXIT;                    \
        i##a = z;                                                               \
        NEXT;                                                                   \
    }
I_A(IADDK, x)
// ia = ia + 1, the commonest: an increment, with no constant to read.
#define IINC(a, ...)                                                            \
    STENCIL(inci_##a) {                                                         \
        int64_t z;                                                              \
        if (__builtin_add_overflow(i##a, int64_t{1}, &z)) EXIT;                 \
        i##a = z;                                                               \
        NEXT;                                                                   \
    }
I_A(IINC, x)

// Jumps on two ints: where `ia op ib`, to the jump's target.
#define IJUMP_B(b, a)                                                           \
    STENCIL(jgeii_##a##_##b) { if (i##a >= i##b) JUMP; NEXT; }                  \
    STENCIL(jltii_##a##_##b) { if (i##a < i##b) JUMP; NEXT; }                   \
    STENCIL(jeqii_##a##_##b) { if (i##a == i##b) JUMP; NEXT; }                  \
    STENCIL(jneii_##a##_##b) { if (i##a != i##b) JUMP; NEXT; }
#define IJUMP_A(a, ...) I_B(IJUMP_B, a)
I_A(IJUMP_A, x)

// ---- Slots, and going on ----

// A register read from its slot, as a region begins; written back as it leaves (kind and
// payload in one store, as the VM writes a slot).
#define ILOAD(a, ...)                                                           \
    STENCIL(loadi_##a) { i##a = base[jade_slot].i; NEXT; }                      \
    STENCIL(storei_##a) {                                                       \
        Slot s;                                                                 \
        std::memset(&s, 0, sizeof s);                                           \
        s.kind = kInt;                                                          \
        s.i = i##a;                                                             \
        std::memcpy(&base[jade_slot], &s, sizeof s);                            \
        NEXT;                                                                   \
    }
I_A(ILOAD, x)
#define FLOAD(a, ...)                                                           \
    STENCIL(loadf_##a) { f##a = base[jade_slot].d; NEXT; }                      \
    STENCIL(storef_##a) {                                                       \
        Slot s;                                                                 \
        std::memset(&s, 0, sizeof s);                                           \
        s.kind = kFloat;                                                        \
        s.d = f##a;                                                             \
        std::memcpy(&base[jade_slot], &s, sizeof s);                            \
        NEXT;                                                                   \
    }
F_A(FLOAD, x)

// A jump; a look at the VM's flag at a loop's end; the region's end.
STENCIL(jump) { JUMP; }
STENCIL(safepoint) {
    if (*jade_attention != 0) EXIT;
    NEXT;
}
// A loop's end: the VM's flag looked at, then back to the loop's start.
STENCIL(loopback) {
    if (*jade_attention != 0) EXIT;
    JUMP;
}
STENCIL(ret) { return jade_ret; }
