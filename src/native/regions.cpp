// Native code's regions (M9.5, docs/native-plan.html, steps 3 and 4): the loops of a function's
// run code made into machine code as the run code is finished. The VM goes into one at an
// instruction whose address in the code is VM::nativeEnter's, and the region gives back where
// the VM goes on.
//
// What a region does, it does as the VM would: the values it keeps in registers are written back
// to their slots at every way out, and anything the stencils do not do leaves for the VM at the
// instruction that needs it, which then does it as it always does, error and message alike.
// That is so of the instructions the stencils have no form for at all (a call, a field): such an
// instruction is an island in the region, which the VM runs, coming back in at the instruction
// after it, where the region knows only what memory holds (each way there within the region
// writes back and loads afresh, as leaving and coming back would). And of those whose stencils
// do only what the VM does at once (a list's item, a division), which leave where that is not so
// (an item out of range): the VM finishes the time round, and comes back in at the loop's start.
//
// Made in three passes. The analysis follows each slot's kind through the loop, from where the
// region is entered: an int, a float, a byte, a whole number read from a list (an int or a byte,
// as the list is), the comparison in the flag register, or a copy of another slot (an object
// pushed, which the region reads where it is). The lowering turns each instruction into steps,
// stencils with the registers they read and write. Then which registers are read before they are
// written, from each way in, says what that way in loads.

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <set>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

#include "../bytecode.h"
#include "../value.h"
#include "native.h"

namespace jade::native {

namespace {

// ---- Reading the run code ----

struct Ins {
    uint32_t at;        // where it starts in `run`: its address in the code first (kThreaded bytes)
    Op op;
    const uint8_t* o;   // its operands
    uint32_t next;      // where the instruction after it starts
};

uint8_t u8(const Ins& i, int k) { return i.o[k]; }
uint16_t u16(const Ins& i, int k) { return static_cast<uint16_t>(i.o[k] | (i.o[k + 1] << 8)); }
int32_t i32(const Ins& i, int k) {
    int32_t v;
    std::memcpy(&v, i.o + k, 4);
    return v;
}

[[noreturn]] void refuse(const std::string& why) { throw std::runtime_error(why); }
// An instruction the region leaves to the VM.
struct Island {};
[[noreturn]] void island() { throw Island{}; }

// ---- Kinds ----

// What a slot may hold where the region is: bits. Entry: what it held as the region was entered
// (untouched on some way here), in memory, and in its register where the region reads it.
constexpr uint16_t kEntry = 1, kInt = 2, kFloat = 4, kBool = 8, kByte = 16;
constexpr uint16_t kWhole = 32;   // an int or a byte read from the list or bytes in slot `src`
constexpr uint16_t kCopy = 64;    // a copy of slot `src` as it was entered, read there
constexpr uint16_t kCopyI = 128;  // the same, its payload in an int register as well (a whole number)
constexpr uint16_t kCopyF = 256;  // the same, in a float register
constexpr uint16_t kWholes = kInt | kByte | kWhole | kCopyI;
constexpr uint16_t kCopies = kCopy | kCopyI | kCopyF;

struct K {
    uint16_t bits = kEntry;
    int16_t src = -1;      // for kWhole and the copies; -2 where two meet
    bool known = false;    // an int whose value is known on every way here: `value`
    int64_t value = 0;
    bool operator==(const K& o) const { return bits == o.bits && src == o.src && known == o.known && (!known || value == o.value); }
};
K constantInt(int64_t v) {
    K k{kInt};
    k.known = true;
    k.value = v;
    return k;
}

struct State {
    bool known = false;
    int top = -1;
    std::map<int, K> kinds;  // slots the region has written on some way here; the rest kEntry
    int flag = -1;           // the slot whose bool the flag register holds, or -1
    K kind(int slot) const {
        auto f = kinds.find(slot);
        return f == kinds.end() ? K{} : f->second;
    }
};

State fresh(int top) {
    State s;
    s.known = true;
    s.top = top;
    return s;
}

// Joining two ways into one place: the same top, every slot's kinds together.
bool join(State& into, const State& from) {
    if (!into.known) {
        into = from;
        into.known = true;
        return true;
    }
    if (into.top != from.top) refuse("two depths of the stack meet");
    bool changed = false;
    std::set<int> slots;
    for (const auto& [s, k] : into.kinds) slots.insert(s);
    for (const auto& [s, k] : from.kinds) slots.insert(s);
    for (int s : slots) {
        const K a = into.kind(s), b = from.kind(s);
        K j{static_cast<uint16_t>(a.bits | b.bits), a.src};
        const bool aSrc = (a.bits & (kWhole | kCopies)) != 0, bSrc = (b.bits & (kWhole | kCopies)) != 0;
        if (!aSrc) j.src = b.src;
        else if (bSrc && a.src != b.src) j.src = -2;
        j.known = a.known && b.known && a.value == b.value;
        j.value = j.known ? a.value : 0;
        if (!(j == a)) {
            into.kinds[s] = j;
            changed = true;
        }
    }
    const int flag = into.flag == from.flag ? into.flag : -1;
    if (flag != into.flag) {
        into.flag = flag;
        changed = true;
    }
    return changed;
}

bool sameState(const State& a, const State& b) {
    if (a.known != b.known || a.top != b.top || a.flag != b.flag) return false;
    std::set<int> slots;
    for (const auto& [s, k] : a.kinds) slots.insert(s);
    for (const auto& [s, k] : b.kinds) slots.insert(s);
    for (int s : slots)
        if (!(a.kind(s) == b.kind(s))) return false;
    return true;
}

// A state as memory holds it, once everything the region wrote is written back: ints, floats
// and bytes as they were (their registers loaded afresh where they are read); a copy, a whole
// number of a list and a bool known only as memory holds them.
State asInMemory(const State& st) {
    State m = st;
    m.flag = -1;
    for (auto& [slot, k] : m.kinds) {
        const uint16_t w = static_cast<uint16_t>(k.bits & ~kEntry);
        if (w == kInt || w == kFloat || w == kByte) continue;
        k = K{};
    }
    return m;
}

// The class a slot is read as where the region knows what it holds: an int (or a byte) or a
// float; -1 where it does not (as entered); -2 where two kinds meet.
int classOf(const K& k) {
    const uint16_t b = static_cast<uint16_t>(k.bits & ~kEntry);
    if (b == 0) return -1;
    if ((b & ~kWholes) == 0) return 0;
    if ((b & ~(kFloat | kCopyF)) == 0) return 1;
    return -2;
}

// The typed instructions, by what they are.
struct Typed {
    enum Kind { None, Arith, Compare, Jump } kind = None;
    bool isFloat = false;
    char op = 0;     // + - * / % for Arith; l (<) e (<=) g (>) G (>=) = (==) for the others
    int form = 0;    // 0: both slots, 1: B slot and C a constant, 2: B a constant and C a slot
};
Typed typed(Op op) {
    Typed t;
    const int o = static_cast<int>(op);
    auto in = [&](Op first, int count) { return o >= static_cast<int>(first) && o < static_cast<int>(first) + count; };
    if (in(Op::RAddISS, 9)) {
        t.kind = Typed::Arith;
        const int k = o - static_cast<int>(Op::RAddISS);
        t.op = "+-*"[k / 3], t.form = k % 3;
    } else if (in(Op::RAddFSS, 12)) {
        t.kind = Typed::Arith, t.isFloat = true;
        const int k = o - static_cast<int>(Op::RAddFSS);
        t.op = "+-*/"[k / 3], t.form = k % 3;
    } else if (in(Op::RLtISS, 10) || in(Op::RLtFSS, 10)) {
        t.kind = Typed::Compare;
        t.isFloat = in(Op::RLtFSS, 10);
        const int k = o - static_cast<int>(t.isFloat ? Op::RLtFSS : Op::RLtISS);
        t.op = "legG="[k / 2], t.form = k % 2;
    } else if (in(Op::RJumpLtISS, 10) || in(Op::RJumpLtFSS, 10)) {
        t.kind = Typed::Jump;
        t.isFloat = in(Op::RJumpLtFSS, 10);
        const int k = o - static_cast<int>(t.isFloat ? Op::RJumpLtFSS : Op::RJumpLtISS);
        t.op = "legG="[k / 2], t.form = k % 2;
    }
    return t;
}

// The untyped arithmetic, comparisons and jumps on slots, which the region does where it knows
// (or guesses, and checks) what the operands are: as Typed.
Typed untyped(Op op) {
    Typed t;
    auto set = [&](Typed::Kind k, char c, int form) { t.kind = k, t.op = c, t.form = form; };
    switch (op) {
        case Op::RAddSS: set(Typed::Arith, '+', 0); break;
        case Op::RAddSK: set(Typed::Arith, '+', 1); break;
        case Op::RAddKS: set(Typed::Arith, '+', 2); break;
        case Op::RSubSS: set(Typed::Arith, '-', 0); break;
        case Op::RSubSK: set(Typed::Arith, '-', 1); break;
        case Op::RSubKS: set(Typed::Arith, '-', 2); break;
        case Op::RMulSS: set(Typed::Arith, '*', 0); break;
        case Op::RMulSK: set(Typed::Arith, '*', 1); break;
        case Op::RMulKS: set(Typed::Arith, '*', 2); break;
        case Op::RDivSS: set(Typed::Arith, '/', 0); break;
        case Op::RDivSK: set(Typed::Arith, '/', 1); break;
        case Op::RDivKS: set(Typed::Arith, '/', 2); break;
        case Op::RModSS: set(Typed::Arith, '%', 0); break;
        case Op::RModSK: set(Typed::Arith, '%', 1); break;
        case Op::RLtSS: set(Typed::Compare, 'l', 0); break;
        case Op::RLtSK: set(Typed::Compare, 'l', 1); break;
        case Op::RLeSS: set(Typed::Compare, 'e', 0); break;
        case Op::RLeSK: set(Typed::Compare, 'e', 1); break;
        case Op::RGtSS: set(Typed::Compare, 'g', 0); break;
        case Op::RGtSK: set(Typed::Compare, 'g', 1); break;
        case Op::RGeSS: set(Typed::Compare, 'G', 0); break;
        case Op::RGeSK: set(Typed::Compare, 'G', 1); break;
        case Op::REqSS: set(Typed::Compare, '=', 0); break;
        case Op::REqSK: set(Typed::Compare, '=', 1); break;
        case Op::RJumpLtSS: set(Typed::Jump, 'l', 0); break;
        case Op::RJumpLtSK: set(Typed::Jump, 'l', 1); break;
        case Op::RJumpLeSS: set(Typed::Jump, 'e', 0); break;
        case Op::RJumpLeSK: set(Typed::Jump, 'e', 1); break;
        case Op::RJumpGtSS: set(Typed::Jump, 'g', 0); break;
        case Op::RJumpGtSK: set(Typed::Jump, 'g', 1); break;
        case Op::RJumpGeSS: set(Typed::Jump, 'G', 0); break;
        case Op::RJumpGeSK: set(Typed::Jump, 'G', 1); break;
        case Op::RJumpEqSS: set(Typed::Jump, '=', 0); break;
        case Op::RJumpEqSK: set(Typed::Jump, '=', 1); break;
        default: break;
    }
    return t;
}

const char* compareName(char op) {
    switch (op) {
        case 'l': return "lt";
        case 'e': return "le";
        case 'g': return "gt";
        case 'G': return "ge";
        default: return "eq";
    }
}

// ---- Steps ----

// A register: a slot's value as an int (or a byte) or as a float. Slots below 0 are scratch
// registers the steps of one instruction use.
enum Cls : uint8_t { CI, CF };
struct Reg {
    int slot;
    Cls cls;
    bool operator<(const Reg& o) const { return slot != o.slot ? slot < o.slot : cls < o.cls; }
    bool operator==(const Reg& o) const { return slot == o.slot && cls == o.cls; }
};
constexpr int kScratchF = -1, kScratchI = -2;
constexpr int kAnyNumber = 2;  // an operand's class: an int or a float, read as a float

// A stencil to be copied: its name's stem and the registers that make the rest of its name,
// what it reads and writes, what fills its gaps, and where it may go.
struct LOp {
    std::string stem;
    std::vector<Reg> named;
    std::vector<Reg> uses, defs;
    int64_t ki = 0;
    double kf = 0;
    int64_t slot = 0, slot2 = 0;
    int jumpTo = -1;     // an instruction of the region
    int jumpExit = -1;   // or a way out (an index into the instruction's exits)
    int exit = -1;       // where it leaves for the VM when it cannot do what it does (an exit's index)
};

// A way out: where the VM goes on, and the state there, which says what is written back.
struct ExitPoint {
    uint32_t at;
    State state;
    std::vector<LOp> stores;
    std::string bad;  // what cannot be written back, where something cannot
    bool miss = false;  // a quick way that could not (counted by the VM: past a limit it stops going in)
    // Or a way to an instruction the VM comes back in at, within the region: what leaving would
    // write back, then what that instruction loads, then on to it.
    int toEntry = -1;
};

// What an instruction does, from the state it starts in.
struct Edge {
    int to = -1;  // an instruction of the region
    State state;
};
struct Result {
    std::vector<Edge> edges;   // the instructions of the region it goes on to
    std::vector<LOp> ops;
    std::vector<ExitPoint> exits;
    bool island = false;       // the VM's: the region leaves for the VM here
    bool fallsThrough = true;  // whether its last step goes on to the next instruction
    int fallExit = -1;         // where it does, and that is a way in: the exit written on the way
    std::vector<int> resumes;  // where the VM, having done its part, comes back in
    State after;               // the state it leaves, going on to the next instruction
};

// ---- A region ----

struct Region {
    const FunctionProto& f;
    std::vector<Ins> ins;                      // the loop's instructions, in order
    std::unordered_map<uint32_t, int> index;   // where an instruction starts -> its place in `ins`
    std::unordered_map<uint32_t, int> tops;    // the VM's top where each run instruction starts (-1: not known)
    std::map<int, uint16_t> hints;             // what each slot is read and written as by the typed instructions
    std::vector<State> in;                     // the state each instruction starts in
    std::set<int> entries;                     // where the VM comes in
    std::set<int> resumed;                     // of them, those after an island: known only as memory holds them
    std::map<int, int64_t> headConstants;      // slots known as the loop starts (a range's step)
    std::map<int, State> comeBack;             // what a way in after an island starts knowing, this round
    std::map<int, State> nextComeBack;         // and for the next round, from this round's islands and ways
    std::vector<Result> results;               // each instruction's, in its state

    explicit Region(const FunctionProto& fn) : f(fn) {}

    // A way to an instruction the VM comes back in at, with what is known there.
    void cameBy(int e, const State& st) {
        if (st.top != topAt(e)) refuse("a way in at two depths of the stack");
        join(nextComeBack[e], st);
    }

    // What is known after island `i`, which the VM ran, where it comes back in: what was known
    // before it, as memory holds it, but for the slots it may have written, its operands and its
    // results (and a local it sets), and anything above the stack's top there.
    State afterIsland(const State& before, int i, int top) const {
        State st = asInMemory(before);
        const Ins& x = ins[static_cast<size_t>(i)];
        const int low = std::min(before.top, top) - 1;
        for (auto k = st.kinds.begin(); k != st.kinds.end();) {
            if (k->first >= low) k = st.kinds.erase(k);
            else ++k;
        }
        if (static_cast<int>(x.op) > static_cast<int>(Op::Count_)) st.kinds.erase(u8(x, 0));  // the run code's: its result's slot first
        switch (x.op) {
            case Op::SetLocal:
            case Op::StoreLocal:
            case Op::OwnLocal:
            case Op::KeepLocal:
            case Op::SetPlace: st.kinds.erase(u16(x, 0)); break;
            default: break;
        }
        st.top = top;
        return st;
    }

    const Value& constant(int k) const {
        if (k < 0 || static_cast<size_t>(k) >= f.constants.size()) refuse("a constant out of range");
        return f.constants[static_cast<size_t>(k)];
    }
    int topAt(int i) const {
        auto t = tops.find(ins[static_cast<size_t>(i)].at);
        return t == tops.end() ? -1 : t->second;
    }

    // What each slot is read and written as by the typed instructions anywhere in the loop, and
    // across moves between slots: the guess, checked where it is made, for an untyped
    // instruction reading a slot the region has not written.
    void learnHints() {
        auto note = [&](int slot, uint16_t kind) { hints[slot] |= kind; };
        for (const Ins& x : ins) {
            const Typed t = typed(x.op);
            const uint16_t kind = t.isFloat ? kFloat : kInt;
            if (t.kind == Typed::Arith) {
                if (t.form != 2) note(u8(x, 1), kind);
                if (t.form != 1) note(u8(x, 2), kind);
                note(u8(x, 0), kind);
            } else if (t.kind == Typed::Compare) {
                note(u8(x, 1), kind);
                if (t.form == 0) note(u8(x, 2), kind);
            } else if (t.kind == Typed::Jump) {
                note(u8(x, 0), kind);
                if (t.form == 0) note(u8(x, 1), kind);
            }
        }
        for (bool changed = true; changed;) {
            changed = false;
            for (const Ins& x : ins) {
                if (x.op != Op::MoveS) continue;
                const int to = u8(x, 0), from = u8(x, 1);
                const uint16_t both = static_cast<uint16_t>(hints[to] | hints[from]);
                if (both != hints[to] || both != hints[from]) {
                    hints[to] = hints[from] = both;
                    changed = true;
                }
            }
        }
    }

    // How the value written to `slot` at instruction `i` is read next on the way the code goes,
    // by a typed instruction: as an int or a float. Sound where it answers, the checker having
    // given the value that type; -1 where it does not know. `top` is the stack's top after `i`.
    // `guessing`: as well by an untyped instruction, from what it reads beside it or what it is
    // (Math.floor, an int's min): a guess, for a step that checks what it reads itself.
    int useClass(int i, int slot, int top, int depth = 0, bool guessing = false) const {
        int j = i + 1;
        for (int steps = 0; steps < 32 && depth < 4 && j >= 0 && j < static_cast<int>(ins.size()); steps++) {
            if (slot >= top) return -1;  // taken off the stack: read by nothing
            const Ins& x = ins[static_cast<size_t>(j)];
            const Typed t = typed(x.op);
            if (t.kind != Typed::None) {
                const int cls = t.isFloat ? CF : CI;
                if (t.kind == Typed::Arith) {
                    if ((t.form != 2 && u8(x, 1) == slot) || (t.form != 1 && u8(x, 2) == slot)) return cls;
                    if (u8(x, 0) == slot) return -1;
                    top = u8(x, 3);
                } else if (t.kind == Typed::Compare) {
                    if (u8(x, 1) == slot || (t.form == 0 && u8(x, 2) == slot)) return cls;
                    if (u8(x, 0) == slot) return -1;
                    top = u8(x, 3);
                } else {
                    if (u8(x, 0) == slot || (t.form == 0 && u8(x, 1) == slot)) return cls;
                    top = u8(x, 2);
                }
                j++;
                continue;
            }
            switch (x.op) {
                case Op::MoveS:
                    if (u8(x, 1) == slot) return useClass(j, u8(x, 0), top, depth + 1, guessing);
                    if (u8(x, 0) == slot) return -1;
                    j++;
                    continue;
                case Op::MoveK:
                    if (u8(x, 0) == slot) return -1;
                    j++;
                    continue;
                case Op::GetLocal:
                    if (top == slot) return -1;
                    if (u16(x, 0) == slot) {
                        // Pushed: how the copy is read; or, where that says nothing, the slot itself later.
                        const int copy = useClass(j, top, top + 1, depth + 1, guessing);
                        if (copy >= 0) return copy;
                    }
                    top++;
                    j++;
                    continue;
                case Op::Const:
                    if (top == slot) return -1;
                    top++;
                    j++;
                    continue;
                case Op::Pop:
                    top--;
                    j++;
                    continue;
                case Op::Jump: {
                    auto to = index.find(static_cast<uint32_t>(i32(x, 0)));
                    if (to == index.end()) return -1;
                    j = to->second;
                    continue;
                }
                case Op::RGetIndexSS:
                case Op::RGetIndexSK:
                    // u8 A, u8 B, u8 C, u8 T: reads B and C as no number it says.
                    if (u8(x, 0) == slot || u8(x, 1) == slot || (x.op == Op::RGetIndexSS && u8(x, 2) == slot)) return -1;
                    top = u8(x, 3);
                    j++;
                    continue;
                case Op::Numeric: {
                    const NumericOp w = static_cast<NumericOp>(u8(x, 0));
                    const bool two = w == NumericOp::MinInt || w == NumericOp::MaxInt || w == NumericOp::MinFloat || w == NumericOp::MaxFloat;
                    if (slot != top - 1 && !(two && slot == top - 2)) {
                        // Not of this slot: past it.
                        if (two) top--;
                        j++;
                        continue;
                    }
                    if (!guessing) return -1;
                    switch (w) {
                        case NumericOp::Sqrt:
                        case NumericOp::Floor:
                        case NumericOp::Ceil:
                        case NumericOp::Round:
                        case NumericOp::AbsFloat:
                        case NumericOp::MinFloat:
                        case NumericOp::MaxFloat: return CF;
                        case NumericOp::AbsInt:
                        case NumericOp::MinInt:
                        case NumericOp::MaxInt:
                        case NumericOp::ToByte: return CI;
                        default: return -1;
                    }
                }
                default: {
                    const Typed u = untyped(x.op);
                    if (u.kind == Typed::Arith || u.kind == Typed::Compare) {
                        const bool readsB = u.form != 2 && u8(x, 1) == slot, readsC = u.form != 1 && u8(x, 2) == slot;
                        if (guessing && (readsB || readsC)) {
                            // What the other operand is: a constant's kind, or a slot's guess.
                            const bool otherConst = readsB ? u.form == 1 : u.form == 2;
                            const int other = readsB ? u8(x, 2) : u8(x, 1);
                            if (otherConst) {
                                const Value& v = constant(other);
                                return v.isFloat() ? CF : (v.isInt() || v.isByte()) ? CI : -1;
                            }
                            auto h = hints.find(other);
                            return h == hints.end() ? -1 : h->second == kInt ? CI : h->second == kFloat ? CF : -1;
                        }
                        if (u8(x, 0) == slot || readsB || readsC) return -1;
                        top = u8(x, 3);
                        j++;
                        continue;
                    }
                    return -1;
                }
            }
        }
        return -1;
    }

    // Written back at a way out: each slot below the top the region wrote, as what it holds.
    // Copies and whole numbers first, which read slots the others may write.
    std::vector<LOp> stores(const State& st, uint32_t at, std::string& bad) const {
        // What cannot be written back is noted, not refused at once: a way the analysis finds
        // on its first time round may be one it does not keep (Region::analyse).
        auto refuse = [&](const std::string& why) { if (bad.empty()) bad = why; };
        std::vector<LOp> first, rest;
        for (const auto& [slot, k] : st.kinds) {
            if (slot >= st.top || k.bits == kEntry) continue;
            uint16_t w = static_cast<uint16_t>(k.bits & ~kEntry);
            // A copy of a slot as entered, beside a value the region made in that slot on another
            // way: of the variable's type either way, in the register either way, so written as
            // what the region made.
            if ((w & kCopyI) && (w & ~kCopyI) != 0 && ((w & ~kCopyI) == kInt || (w & ~kCopyI) == kByte)) w = static_cast<uint16_t>(w & ~kCopyI);
            if ((w & kCopyF) && (w & ~kCopyF) == kFloat) w = kFloat;
            LOp l;
            l.slot = slot;
            if (w == kCopy || w == kCopyI || w == kCopyF) {
                if ((k.bits & kEntry) || k.src < 0) refuse("slot " + std::to_string(slot) + ", a copy of " + std::to_string(k.src) + " (" + std::to_string(k.bits) + "), written back on some ways only, at " + std::to_string(at));
                l.stem = "copyslot";
                l.slot2 = k.src;
                first.push_back(l);
            } else if (w == kWhole) {
                if (k.src < 0) refuse("a whole number of two lists");
                l.stem = "storew", l.named = {Reg{slot, CI}}, l.uses = l.named, l.slot2 = k.src;
                first.push_back(l);
            } else if (w == kInt || w == kByte) {
                l.stem = w == kInt ? "storei" : "storeby", l.named = {Reg{slot, CI}}, l.uses = l.named;
                rest.push_back(l);
            } else if (w == kFloat) {
                l.stem = "storef", l.named = {Reg{slot, CF}}, l.uses = l.named;
                rest.push_back(l);
            } else if (w == kBool) {
                if ((k.bits & kEntry) || st.flag != slot) refuse("a bool at a way out that the flag register does not hold");
                l.stem = "storeb";
                rest.push_back(l);
            } else {
                refuse("slot " + std::to_string(slot) + " of two kinds (" + std::to_string(k.bits) + ") at a way out, at " + std::to_string(at));
            }
        }
        first.insert(first.end(), rest.begin(), rest.end());
        return first;
    }

    // ---- What each instruction does ----

    struct Ctx {
        Region& r;
        int i;
        const Ins& x;
        State s;
        Result out;

        // The region leaves for the VM at this instruction, where a step cannot do its part.
        int exitHere() { return exitTo(x.at, s); }
        int exitTo(uint32_t at, const State& st) {
            out.exits.push_back(ExitPoint{at, st, {}, {}, false, -1});
            out.exits.back().stores = r.stores(st, at, out.exits.back().bad);
            return static_cast<int>(out.exits.size()) - 1;
        }
        // A step that may leave for the VM, which then does the instruction (and the rest of the
        // time round).
        void leaves(LOp& l) {
            l.exit = exitHere();
            out.exits.back().miss = true;
        }
        LOp& op(const std::string& stem, std::vector<Reg> named = {}, std::vector<Reg> uses = {}, std::vector<Reg> defs = {}) {
            out.ops.push_back(LOp{});
            LOp& l = out.ops.back();
            l.stem = stem;
            l.named = std::move(named);
            l.uses = std::move(uses);
            l.defs = std::move(defs);
            return l;
        }
        void go(uint32_t target, const State& st, LOp& jumping) {
            auto found = r.index.find(target);
            if (found == r.index.end()) {
                jumping.jumpExit = exitTo(target, st);
            } else if (r.resumed.count(found->second)) {
                jumping.jumpExit = exitTo(target, st);
                out.exits.back().toEntry = found->second;
                r.cameBy(found->second, asInMemory(st));
            } else {
                jumping.jumpTo = found->second;
                out.edges.push_back(Edge{found->second, st});
            }
        }
        void fall(const State& st) {
            if (r.resumed.count(i + 1)) {
                out.fallExit = exitTo(r.ins[static_cast<size_t>(i) + 1].at, st);
                out.exits.back().toEntry = i + 1;
                r.cameBy(i + 1, asInMemory(st));
            } else {
                out.edges.push_back(Edge{i + 1, st});
            }
        }

        // A slot read as an int or a float: its register. Where the region has not written it
        // and only guesses what it holds (`checked`), the slot's kind is checked first, and the
        // VM does the instruction where the guess is wrong.
        Reg read(int slot, Cls cls, bool checked) {
            // A copy of a slot as entered: that slot's register.
            if (const K k = s.kind(slot); k.bits == kCopy && k.src >= 0 && s.kind(k.src).bits == kEntry) slot = k.src;
            const int known = classOf(s.kind(slot));
            if (known >= 0) {
                if (known != cls) island();
                return Reg{slot, cls};
            }
            if (known == -2) island();
            if (checked) {
                LOp& l = op(cls == CI ? "chki" : "chkf");
                l.slot = slot;
                leaves(l);
            }
            return Reg{slot, cls};
        }
        // The class an operand of an untyped instruction is read as: what the region knows, or
        // the typed instructions' guess.
        Cls guess(int slot) {
            if (const K k = s.kind(slot); k.bits == kCopy && k.src >= 0 && s.kind(k.src).bits == kEntry) slot = k.src;
            const int known = classOf(s.kind(slot));
            if (known >= 0) return static_cast<Cls>(known);
            if (known == -2) island();
            auto h = r.hints.find(slot);
            if (h == r.hints.end()) island();
            if (h->second == kInt) return CI;
            if (h->second == kFloat) return CF;
            island();
        }
        void write(int slot, K k) {
            s.kinds[slot] = k;
            if (k.bits == kBool) s.flag = slot;
            else if (s.flag == slot) s.flag = -1;
        }
        // The slot an object is read from: the slot itself, as entered, or the one it copies.
        int objectSlot(int slot) {
            const K k = s.kind(slot);
            if (k.bits == kEntry) return slot;
            if ((k.bits & ~kCopies) == 0 && k.src >= 0) return k.src;
            island();
        }
        // The flag register holding slot `slot`'s bool: as it is, or read from the slot.
        void flagOf(int slot) {
            const K k = s.kind(slot);
            if (k.bits == kBool && s.flag == slot) return;
            if (k.bits != kEntry) island();
            LOp& l = op("loadflag");
            l.slot = slot;
            leaves(l);
            s.flag = slot;
            s.kinds[slot] = K{kBool};
        }
        int64_t intConstant(int k) {
            const Value& v = r.constant(k);
            if (!(v.isInt() || v.isByte())) island();
            return v.as.i;
        }
        // A slot's value as a number of class `cls`, in a register: what the region made there,
        // or what memory holds as entered (the slot's own, or the one a copy is of), checked to be
        // that (`exact`: an int, not a byte, as the VM's quick ways ask).
        Reg number(int slot, Cls cls, bool exact) {
            const K k = s.kind(slot);
            int from = -1;
            if ((k.bits == kCopy || k.bits == kCopyI || k.bits == kCopyF) && k.src >= 0 && s.kind(k.src).bits == kEntry) from = k.src;
            else if (k.bits == kEntry) from = slot;
            if (from >= 0) {
                LOp& l = op(cls == CF ? "chkf" : exact ? "chkint" : "chki");
                l.slot = from;
                leaves(l);
                return Reg{from, cls};
            }
            if (classOf(k) != cls) island();
            if (exact && cls == CI && k.bits == kWhole && k.src >= 0) {
                // Read from a list or bytes: an int where the object is no bytes.
                LOp& l = op("chklist");
                l.slot = k.src;
                leaves(l);
                return Reg{slot, cls};
            }
            if (exact && cls == CI && (k.bits & ~kEntry) != kInt) island();
            return Reg{slot, cls};
        }
    };

    Result transfer(int i, const State& start) {
        const Ins& x = ins[static_cast<size_t>(i)];
        Ctx c{*this, i, x, start, {}};
        try {
            step(c);
        } catch (const Island&) {
            static const bool trace = std::getenv("JADE_TRACE_ISLANDS") != nullptr;
            if (trace) std::fprintf(stderr, "  island: %s at %u (%s)\n", opName(x.op), x.at, f.name.c_str());
            Result out;
            out.island = true;
            out.fallsThrough = false;
            out.exits.push_back(ExitPoint{x.at, start, {}, {}, false, -1});
            out.exits.back().stores = stores(start, x.at, out.exits.back().bad);
            out.ops.push_back(LOp{});
            out.ops.back().stem = "jump";
            out.ops.back().jumpExit = 0;
            // The VM comes back in after it, or where it jumps.
            out.resumes.push_back(i + 1);
            if (const int t = targetOperand(x.op); t > 0 && static_cast<int>(kOpBytes) + (t - 1) + 4 <= static_cast<int>(x.next - x.at - kThreaded)) {
                auto to = index.find(static_cast<uint32_t>(i32(x, t - 1)));
                if (to != index.end()) out.resumes.push_back(to->second);
            }
            return out;
        }
        c.out.after = c.s;
        return std::move(c.out);
    }

    void step(Ctx& c) {
        const Ins& x = c.x;
        State& s = c.s;
        const Typed t = typed(x.op);
        if (t.kind != Typed::None) return arithmetic(c, t, false);
        const Typed u = untyped(x.op);
        if (u.kind != Typed::None) return arithmetic(c, u, true);
        switch (x.op) {
            case Op::MoveS:
            case Op::GetLocal: {
                const int from = x.op == Op::MoveS ? u8(x, 1) : u16(x, 0);
                const int to = x.op == Op::MoveS ? u8(x, 0) : s.top;
                move(c, from, to);
                if (x.op == Op::GetLocal) s.top++;
                c.fall(s);
                return;
            }
            case Op::MoveK:
            case Op::Const: {
                const int to = x.op == Op::MoveK ? u8(x, 0) : s.top;
                const Value& v = constant(x.op == Op::MoveK ? u8(x, 1) : u16(x, 0));
                if (v.isFloat()) {
                    c.op("setfk", {Reg{to, CF}}, {}, {Reg{to, CF}}).kf = v.as.d;
                    c.write(to, K{kFloat});
                } else if (v.isInt()) {
                    c.op("setik", {Reg{to, CI}}, {}, {Reg{to, CI}}).ki = v.as.i;
                    c.write(to, constantInt(v.as.i));
                } else {
                    island();
                }
                if (x.op == Op::Const) s.top++;
                c.fall(s);
                return;
            }
            case Op::SetTop:
                s.top = u8(x, 0);
                c.fall(s);
                return;
            case Op::Pop:
                s.top--;
                c.fall(s);
                return;
            case Op::RNot: {  // u8 A, u8 B, u8 T, u8 S: the comparison's answer turned round
                c.flagOf(u8(x, 1));
                c.op("notflag");
                c.write(u8(x, 0), K{kBool});
                s.top = u8(x, 2);
                c.fall(s);
                return;
            }
            case Op::RJumpIfFalse: {  // u8 B, u8 T, i32 target, i32 slow
                c.flagOf(u8(x, 0));
                State after = s;
                after.top = u8(x, 1);
                c.go(static_cast<uint32_t>(i32(x, 2)), after, c.op("jfalse"));
                c.fall(after);
                return;
            }
            case Op::And:
            case Op::Or: {
                c.flagOf(s.top - 1);
                c.go(static_cast<uint32_t>(i32(x, 0)), s, c.op(x.op == Op::And ? "jfalse" : "jtrue"));  // keeping the value
                State popped = s;
                popped.top--;
                c.fall(popped);
                return;
            }
            case Op::Jump:
                c.go(static_cast<uint32_t>(i32(x, 0)), s, c.op("jump"));
                c.out.fallsThrough = false;
                return;
            case Op::Loop: {
                LOp& l = c.op("loopback");
                c.go(static_cast<uint32_t>(i32(x, 0)), s, l);
                // The VM's flag: the VM does the loop's end, and comes back in at its start.
                l.exit = c.exitHere();
                c.out.fallsThrough = false;
                return;
            }
            case Op::RangeNext: {  // u16 slot, i32 target: the next value, how many are left, the step
                const int next = u16(x, 0), left = next + 1, by = next + 2;
                const Reg n = c.read(next, CI, false), l = c.read(left, CI, false);
                const K step = s.kind(by);
                LOp& done = c.op("jngtik", {l}, {l});
                done.ki = 0;
                c.go(static_cast<uint32_t>(i32(x, 2)), s, done);
                c.op("deci", {l}, {l}, {l});
                const Reg pushed{s.top, CI};
                c.op("movi", {pushed, n}, {n}, {pushed});
                if (step.known) {
                    c.op("addwik", {n}, {n}, {n}).ki = step.value;
                } else {
                    const Reg b = c.read(by, CI, false);
                    c.op("addwi", {n, b}, {n, b}, {n});
                }
                c.write(left, K{kInt});
                c.write(next, K{kInt});
                c.write(s.top, K{kInt});
                s.top++;
                c.fall(s);
                return;
            }
            case Op::RangeLoop: {  // u8 flags: start end [step] -> start count step
                const int flags = u8(x, 0);
                if (flags & 4) island();  // a step given: the VM checks it
                const int top = s.top;
                // As the VM asks: two ints (not bytes), each in its own register (the end's becomes
                // the count).
                const Reg start{top - 2, CI}, end{top - 1, CI};
                for (const Reg& bound : {start, end}) {
                    const Reg from = c.number(bound.slot, CI, true);
                    if (!(from == bound)) c.op("movi", {bound, from}, {from}, {bound});
                }
                c.op(std::string("rcnt") + ((flags & 2) ? "d" : "a") + ((flags & 1) ? "i" : "e"), {end, start}, {end, start}, {end});
                const int64_t by = (flags & 2) ? -1 : 1;
                c.op("setik", {Reg{top, CI}}, {}, {Reg{top, CI}}).ki = by;
                c.write(top - 2, K{kInt});
                c.write(top - 1, K{kInt});
                c.write(top, constantInt(by));
                s.top = top + 1;
                c.fall(s);
                return;
            }
            case Op::RGetIndexSS:
            case Op::RGetIndexSK: {  // u8 A, u8 B, u8 C, u8 T, u8 S, i32 slow
                const int a = u8(x, 0);
                const int list = c.objectSlot(u8(x, 1));
                // As read next; or a guess, which the step checks (an item of another kind leaves).
                int cls = useClass(c.i, a, u8(x, 3));
                if (cls < 0) cls = useClass(c.i, a, u8(x, 3), 0, true);
                if (cls < 0) island();
                const Reg dst{a, static_cast<Cls>(cls)};
                LOp* l;
                if (x.op == Op::RGetIndexSK) {
                    const int64_t k = c.intConstant(u8(x, 2));
                    l = &c.op(cls == CF ? "idxfk" : "idxwk", {dst}, {}, {dst});
                    l->ki = k;
                } else {
                    const Reg index = c.read(u8(x, 2), CI, true);
                    l = &c.op(cls == CF ? "idxf" : "idxw", {dst, index}, {index}, {dst});
                }
                l->slot = list;
                c.leaves(*l);
                c.write(a, cls == CF ? K{kFloat} : K{kWhole, static_cast<int16_t>(list)});
                s.top = u8(x, 3);
                c.fall(s);
                return;
            }
            case Op::SetIndex: {  // list index value -> value
                const int top = s.top;
                const int list = c.objectSlot(top - 3);
                const Reg index = c.read(top - 2, CI, true);
                const K v = s.kind(top - 1);
                const char* stem = (v.bits == kFloat || v.bits == kCopyF) ? "setf" : v.bits == kInt ? "setwi" : v.bits == kByte ? "setwb" : nullptr;
                if (stem == nullptr) island();
                const Reg value{top - 1, stem[3] == 'f' ? CF : CI};
                LOp& l = c.op(stem, {value, index}, {value, index});
                l.slot = list;
                c.leaves(l);
                const Reg result{top - 3, value.cls};
                c.op(value.cls == CF ? "movf" : "movi", {result, value}, {value}, {result});
                c.write(top - 3, value.cls == CF ? K{kFloat} : v);
                s.top = top - 2;
                c.fall(s);
                return;
            }
            case Op::Numeric: numeric(c); return;
            case Op::RNeg: {  // u8 A, u8 B, u8 T, u8 S, i32 slow
                const int a = u8(x, 0);
                const Cls cls = c.guess(u8(x, 1));
                const Reg b = c.read(u8(x, 1), cls, true), dst{a, cls};
                LOp& l = c.op(cls == CF ? "negf" : "negi", {dst, b}, {b}, {dst});
                if (cls == CI) c.leaves(l);
                c.write(a, K{cls == CF ? kFloat : kInt});
                s.top = u8(x, 2);
                c.fall(s);
                return;
            }
            case Op::RShlSK:
            case Op::RShrSK:
            case Op::RBitAndSS:
            case Op::RBitAndSK:
            case Op::RBitOrSS:
            case Op::RBitOrSK: bitwise(c); return;
            default: island();
        }
    }

    // A move of a slot, or its push: what it holds goes with it.
    void move(Ctx& c, int from, int to) {
        const K k = c.s.kind(from);
        if (k.bits == kEntry) {
            // As entered: a copy, read where it is, its payload in a register where it is read
            // next as a number.
            // An int or a float as read next by a typed instruction, or as the typed instructions
            // read and write the slot elsewhere (a guess): checked in memory to be one (an int,
            // not a byte), so that it is one here as it is once the region has written the slot.
            int cls = useClass(c.i, to, to >= c.s.top ? to + 1 : c.s.top);
            bool sure = cls >= 0;
            if (cls < 0) {
                auto h = hints.find(from);
                if (h != hints.end() && (h->second == kInt || h->second == kFloat)) cls = h->second == kFloat ? CF : CI;
            }
            if (cls < 0) {
                c.write(to, K{kCopy, static_cast<int16_t>(from)});
                return;
            }
            if (!(sure && cls == CF)) {
                LOp& l = c.op(cls == CF ? "chkf" : "chkint");
                l.slot = from;
                c.leaves(l);
            }
            const Reg src{from, static_cast<Cls>(cls)}, dst{to, static_cast<Cls>(cls)};
            if (!(src == dst)) c.op(cls == CF ? "movf" : "movi", {dst, src}, {src}, {dst});
            c.write(to, K{static_cast<uint16_t>(cls == CF ? kFloat : kInt)});
            return;
        }
        if ((k.bits & ~kCopies) == 0 && k.src == to) {
            // A slot's value as entered, back into it: as it was, where the region has not
            // written it since.
            if (c.s.kind(to).bits != kEntry) island();
            return;
        }
        if ((k.bits & ~kCopies) == 0 && k.src >= 0) {
            // A copy of a copy: of the same slot.
            if (k.bits == kCopyI || k.bits == kCopyF) {
                const Cls cls = k.bits == kCopyF ? CF : CI;
                c.op(cls == CF ? "movf" : "movi", {Reg{to, cls}, Reg{from, cls}}, {Reg{from, cls}}, {Reg{to, cls}});
            }
            c.write(to, k);
            return;
        }
        const int cls = classOf(k);
        if (cls < 0) island();
        const Reg src{from, static_cast<Cls>(cls)}, dst{to, static_cast<Cls>(cls)};
        if (!(src == dst)) c.op(cls == CF ? "movf" : "movi", {dst, src}, {src}, {dst});
        K moved = k;
        moved.bits = static_cast<uint16_t>(k.bits & ~kEntry);
        c.write(to, moved);
    }

    // The typed instructions, and the untyped ones where the region knows (or guesses) their
    // operands: an int beside a float made a float, as the VM does.
    void arithmetic(Ctx& c, const Typed& t, bool untypedOp) {
        const Ins& x = c.x;
        State& s = c.s;
        const int at = t.kind == Typed::Jump ? 0 : 1;
        const bool bConst = t.form == 2, cConst = t.form == 1;
        const int bOp = u8(x, at), cOp = u8(x, at + 1);
        int bCls, cCls;
        if (!untypedOp) {
            bCls = cCls = t.isFloat ? CF : CI;
        } else {
            // Each operand's class: a constant's, what the region knows, the typed instructions'
            // guess; and where only one is had, the other taken to be the same (checked, as any
            // guess is).
            auto clsOf = [&](bool isConst, int operand) -> int {
                if (isConst) {
                    const Value& v = constant(operand);
                    if (v.isFloat()) return CF;
                    if (v.isInt() || v.isByte()) return CI;
                    island();
                }
                try {
                    return c.guess(operand);
                } catch (const Island&) {
                    return -1;
                }
            };
            bCls = clsOf(bConst, bOp);
            cCls = clsOf(cConst, cOp);
            if (bCls < 0 && cCls < 0) island();
            // Beside a float in +, -, * or /, any number: the answer a float either way.
            if (t.kind == Typed::Arith && t.op != '%') {
                if (bCls < 0 && cCls == CF) bCls = kAnyNumber;
                if (cCls < 0 && bCls == CF) cCls = kAnyNumber;
            }
            if (bCls < 0) bCls = cCls;
            if (cCls < 0) cCls = bCls;
        }
        const bool mixed = bCls != cCls && bCls != kAnyNumber && cCls != kAnyNumber;
        if (mixed && t.kind != Typed::Arith) island();
        const Cls work = (mixed || bCls == kAnyNumber || cCls == kAnyNumber) ? CF : static_cast<Cls>(bCls);
        if (t.op == '%' && work == CF) island();  // fmod: the VM's
        const char* tk = work == CF ? "f" : "i";
        // A slot operand in the class worked on: its register, or an int made a float in the
        // scratch register.
        auto operand = [&](int slot, int cls) -> Reg {
            const Reg scratch{kScratchF, CF};
            if (cls == kAnyNumber) {
                // As memory holds it (the slot's own, or the one it copies), made a float.
                const K k = s.kind(slot);
                const int from = k.bits == kEntry ? slot : (k.bits & ~kCopies) == 0 && k.src >= 0 ? k.src : -1;
                if (from < 0) island();
                LOp& l = c.op("numf", {scratch}, {}, {scratch});
                l.slot = from;
                c.leaves(l);
                return scratch;
            }
            const Reg r = c.read(slot, static_cast<Cls>(cls), untypedOp);
            if (cls == work) return r;
            c.op("cvtif", {scratch, r}, {r}, {scratch});
            return scratch;
        };
        int64_t ki = 0;
        double kf = 0;
        if (bConst || cConst) {
            const Value& v = constant(bConst ? bOp : cOp);
            if (work == CF) kf = v.isFloat() ? v.as.d : static_cast<double>(v.as.i);
            else ki = v.as.i;
        }
        Reg b{}, cr{};
        if (!bConst) b = operand(bOp, bCls);
        if (!cConst) cr = operand(cOp, cCls);
        auto setK = [&](LOp& l) {
            if (work == CF) l.kf = kf;
            else l.ki = ki;
        };
        if (t.kind == Typed::Arith) {
            const int a = u8(x, 0);
            const Reg dst{a, work};
            if (work == CI && (t.op == '/' || t.op == '%')) {
                const char* stem = t.op == '/' ? "div" : "mod";
                if (cConst) {
                    if (ki == 0 || ki == -1) island();  // the VM's rules for them
                    c.op(std::string(stem) + "ik", {dst, b}, {b}, {dst}).ki = ki;
                } else {
                    // Through the scratch register: the divisor checked, then the division.
                    const Reg scratch{kScratchI, CI};
                    c.leaves(c.op("chkdivi", {cr}, {cr}));
                    if (bConst) c.op("setik", {scratch}, {}, {scratch}).ki = ki;
                    else c.op("movi", {scratch, b}, {b}, {scratch});
                    c.op(std::string(stem) + "i", {scratch, cr}, {scratch, cr}, {scratch});
                    c.op("movi", {dst, scratch}, {scratch}, {dst});
                }
                c.write(a, K{kInt});
            } else {
                const char* stem = t.op == '+' ? "add" : t.op == '-' ? "sub" : t.op == '*' ? "mul" : "div";
                const Reg src = bConst ? cr : b;
                LOp* l;
                if (!bConst && !cConst) {
                    l = &c.op(std::string(stem) + tk, {dst, b, cr}, {b, cr}, {dst});
                } else if (work == CI && t.op == '+' && dst == src && ki == 1) {
                    l = &c.op("inci", {dst}, {dst}, {dst});  // b + 1 into b: an increment
                } else {
                    const bool reversed = bConst && (t.op == '-' || t.op == '/');
                    l = &c.op(std::string(stem) + tk + (reversed ? "kr" : "k"), {dst, src}, {src}, {dst});
                    setK(*l);
                }
                if (work == CI) l->exit = c.exitHere();  // an overflow: the VM does the instruction, and says so
                c.write(a, K{work == CF ? kFloat : kInt});
            }
            s.top = u8(x, 3);
            c.fall(s);
            return;
        }
        if (t.kind == Typed::Compare) {
            LOp& l = cConst ? c.op(std::string(compareName(t.op)) + tk + "k", {b}, {b}) : c.op(std::string(compareName(t.op)) + tk + tk, {b, cr}, {b, cr});
            if (cConst) setK(l);
            c.write(u8(x, 0), K{kBool});
            s.top = u8(x, 3);
            c.fall(s);
            return;
        }
        // A jump where the comparison does not hold.
        State after = s;
        after.top = u8(x, 2);
        LOp& l = cConst ? c.op(std::string("jn") + compareName(t.op) + tk + "k", {b}, {b})
                        : c.op(std::string("jn") + compareName(t.op) + tk + tk, {b, cr}, {b, cr});
        if (cConst) setK(l);
        c.go(static_cast<uint32_t>(i32(x, 3)), after, l);
        c.fall(after);
    }

    // Shifts by a constant, and & and | of whole numbers.
    void bitwise(Ctx& c) {
        const Ins& x = c.x;
        const int a = u8(x, 0);
        const bool k = x.op == Op::RShlSK || x.op == Op::RShrSK || x.op == Op::RBitAndSK || x.op == Op::RBitOrSK;
        if (c.guess(u8(x, 1)) != CI) island();
        const Reg b = c.read(u8(x, 1), CI, true), dst{a, CI};
        if (k) {
            const int64_t v = c.intConstant(u8(x, 2));
            const bool shift = x.op == Op::RShlSK || x.op == Op::RShrSK;
            if (shift && (v < 0 || v >= 64)) island();
            const char* stem = x.op == Op::RShlSK ? "shlik" : x.op == Op::RShrSK ? "shrik" : x.op == Op::RBitAndSK ? "andik" : "orik";
            c.op(stem, {dst, b}, {b}, {dst}).ki = v;
        } else {
            if (c.guess(u8(x, 2)) != CI) island();
            const Reg r = c.read(u8(x, 2), CI, true);
            const char* stem = x.op == Op::RBitAndSS ? "andi" : "ori";
            // Two operands, the first changed: into dst from whichever it is already.
            if (dst == b || dst == r) {
                const Reg other = dst == b ? r : b;
                c.op(stem, {dst, other}, {dst, other}, {dst});
            } else {
                c.op("movi", {dst, b}, {b}, {dst});
                c.op(stem, {dst, r}, {dst, r}, {dst});
            }
        }
        c.write(a, K{kInt});
        c.s.top = u8(x, 3);
        c.fall(c.s);
    }

    // Math's functions and the conversions (NumericOp) on the stack's top, where the operands'
    // kinds are known (or checked): as the VM's own quick ways do them.
    void numeric(Ctx& c) {
        State& s = c.s;
        const int top = s.top - 1;
        const K k = s.kind(top);
        const int cls = classOf(k);
        const Reg asI{top, CI}, asF{top, CF};
        const NumericOp which = static_cast<NumericOp>(u8(c.x, 0));
        // The operand, as `cl`, in the top's own register (which the steps change in place).
        auto own = [&](Cls cl, bool exact) {
            const Reg from = c.number(top, cl, exact), to{top, cl};
            if (!(from == to)) c.op(cl == CF ? "movf" : "movi", {to, from}, {from}, {to});
            return to;
        };
        switch (which) {
            case NumericOp::ToInt:
                if (cls == CF) c.leaves(c.op("cvtfi", {asI, asF}, {asF}, {asI}));
                else if (cls != CI) island();
                c.write(top, K{kInt});
                break;
            case NumericOp::ToFloat:
                if (cls == CI) {
                    if (k.bits != kInt) island();  // as the VM's quick way: an int
                    c.op("cvtif", {asF, asI}, {asI}, {asF});
                } else if (cls != CF) {
                    island();
                }
                c.write(top, K{kFloat});
                break;
            case NumericOp::ToByte: {
                const Reg v = own(CI, true);
                c.leaves(c.op("chkbyte", {v}, {v}));
                c.write(top, K{kByte});
                break;
            }
            case NumericOp::Sqrt:
            case NumericOp::AbsFloat: {
                const Reg from = c.number(top, CF, false);
                c.op(which == NumericOp::Sqrt ? "sqrtf" : "absf", {asF, from}, {from}, {asF});
                c.write(top, K{kFloat});
                break;
            }
            case NumericOp::AbsInt: {
                const Reg v = own(CI, true);
                c.leaves(c.op("absi", {v}, {v}, {v}));
                c.write(top, K{kInt});
                break;
            }
            case NumericOp::Floor:
            case NumericOp::Ceil:
            case NumericOp::Round: {
                const Reg from = c.number(top, CF, false);
                c.leaves(c.op(which == NumericOp::Floor ? "floori" : which == NumericOp::Ceil ? "ceili" : "roundi", {asI, from}, {from}, {asI}));
                c.write(top, K{kInt});
                break;
            }
            case NumericOp::MinInt:
            case NumericOp::MaxInt:
            case NumericOp::MinFloat:
            case NumericOp::MaxFloat: {
                // As the VM's quick way: two ints, or two floats; the first kept unless the
                // second is less (more).
                const bool min = which == NumericOp::MinInt || which == NumericOp::MinFloat;
                const Cls cl = which == NumericOp::MinFloat || which == NumericOp::MaxFloat ? CF : CI;
                const Reg second = c.number(top, cl, true);
                const Reg firstFrom = c.number(top - 1, cl, true), first{top - 1, cl};
                if (!(firstFrom == first)) c.op(cl == CF ? "movf" : "movi", {first, firstFrom}, {firstFrom}, {first});
                c.op(std::string(min ? "min" : "max") + (cl == CF ? "f" : "i"), {first, second}, {first, second}, {first});
                c.write(top - 1, K{cl == CF ? kFloat : kInt});
                s.top--;
                break;
            }
            default: island();
        }
        c.fall(s);
    }

    // ---- The analysis: each instruction's state, from each way in ----

    // Each instruction's state, from what each way in starts knowing and what the instructions
    // before it now give it (not all they ever gave: what one gave on its first time round, from
    // less known, is gone once it gives more). Each `start` is joined in where its way in is.
    void flow(const std::map<int, State>& start) {
        const size_t n = ins.size();
        in.assign(n, State{});
        results.assign(n, Result{});
        std::vector<std::map<int, State>> given(n);  // what each instruction is given, by which (-1: a way in)
        std::vector<std::set<int>> gaveTo(n);        // where each last gave something
        std::vector<bool> done(n, false);
        auto recompute = [&](int t) {
            State st;
            for (const auto& [from, g] : given[static_cast<size_t>(t)]) join(st, g);
            if (done[static_cast<size_t>(t)] && sameState(st, in[static_cast<size_t>(t)])) return false;
            in[static_cast<size_t>(t)] = st;
            return true;
        };
        std::vector<int> work;
        for (const auto& [e, st] : start) {
            given[static_cast<size_t>(e)][-1] = st;
            recompute(e);
            work.push_back(e);
        }
        for (long steps = 0; !work.empty(); steps++) {
            if (steps > 200000) refuse("the analysis does not settle");
            const int i = work.back();
            work.pop_back();
            done[static_cast<size_t>(i)] = true;
            Result r = transfer(i, in[static_cast<size_t>(i)]);
            std::map<int, State> gives;
            for (const Edge& e : r.edges) {
                if (e.to < 0 || e.to >= static_cast<int>(n)) refuse("a way past the loop's end");
                join(gives[e.to], e.state);
            }
            std::set<int> touched;
            for (const auto& [t, st] : gives) {
                auto old = given[static_cast<size_t>(t)].find(i);
                if (old == given[static_cast<size_t>(t)].end() || !sameState(old->second, st)) {
                    given[static_cast<size_t>(t)][i] = st;
                    touched.insert(t);
                }
            }
            for (int t : gaveTo[static_cast<size_t>(i)])
                if (!gives.count(t)) {
                    given[static_cast<size_t>(t)].erase(i);
                    touched.insert(t);
                }
            gaveTo[static_cast<size_t>(i)].clear();
            for (const auto& [t, st] : gives) gaveTo[static_cast<size_t>(i)].insert(t);
            results[static_cast<size_t>(i)] = std::move(r);
            for (int t : touched)
                if (recompute(t) || !done[static_cast<size_t>(t)]) work.push_back(t);
        }
        // An instruction no longer given anything is not reached.
        for (size_t i = 0; i < n; i++)
            if (given[i].empty()) {
                in[i] = State{};
                results[i] = Result{};
            }
    }

    void analyse(int headTop) {
        entries = {0};
        tops[ins[0].at] = headTop;
        for (int round = 0; round < 8; round++) {
            resumed = entries;
            resumed.erase(0);
            nextComeBack.clear();
            std::map<int, State> start;
            for (int e : entries) {
                if (e == 0) {
                    State st = fresh(headTop);
                    for (const auto& [slot, value] : headConstants) {
                        K k;
                        k.known = true;
                        k.value = value;
                        st.kinds[slot] = k;  // as entered, its value known
                    }
                    start[e] = st;
                } else if (auto known = comeBack.find(e); known != comeBack.end() && known->second.known) {
                    start[e] = known->second;
                } else {
                    start[e] = fresh(topAt(e));
                }
            }
            flow(start);
            // Where the VM comes back in: after an island, where the region knows the top and does
            // the instruction itself; knowing there what is known after each island coming there,
            // and on each way within the region.
            std::set<int> more{0};
            for (size_t i = 0; i < ins.size(); i++) {
                if (!in[i].known || !results[i].island) continue;
                for (int e : results[i].resumes) {
                    if (e <= 0 || e >= static_cast<int>(ins.size()) || topAt(e) < 0) continue;
                    if (in[static_cast<size_t>(e)].known && results[static_cast<size_t>(e)].island) continue;
                    more.insert(e);
                    join(nextComeBack[e], afterIsland(in[i], static_cast<int>(i), topAt(e)));
                }
            }
            for (auto c = nextComeBack.begin(); c != nextComeBack.end();) {
                if (!more.count(c->first)) c = nextComeBack.erase(c);
                else ++c;
            }
            bool same = more == entries && nextComeBack.size() == comeBack.size();
            for (const auto& [e, st] : nextComeBack) {
                auto old = comeBack.find(e);
                same = same && old != comeBack.end() && sameState(old->second, st);
            }
            if (same) break;
            if (round == 7) refuse("what is known where the VM comes back in does not settle");
            entries = std::move(more);
            comeBack = nextComeBack;
        }
        // No way in at an island: the VM runs it as it comes to it. (Its ways there within the
        // region, made as to a way in, still write back and load: so it may stay one.)
        for (auto e = entries.begin(); e != entries.end();) {
            if (*e != 0 && (!in[static_cast<size_t>(*e)].known || results[static_cast<size_t>(*e)].island)) e = entries.erase(e);
            else ++e;
        }
        if (results[0].island) refuse("its first instruction is the VM's");
        for (size_t i = 0; i < ins.size(); i++)
            if (in[i].known)
                for (const ExitPoint& e : results[i].exits)
                    if (!e.bad.empty()) refuse(e.bad);
    }

    // ---- Liveness: which registers each instruction reads before writing ----

    std::vector<std::set<Reg>> liveIn;

    // What a way out reads: the registers it writes back. (A way to an instruction the VM comes
    // back in at loads what that reads after.)
    static void addUses(std::set<Reg>& live, const ExitPoint& e) {
        for (const LOp& l : e.stores) live.insert(l.uses.begin(), l.uses.end());
    }

    void findLive() {
        liveIn.assign(ins.size(), {});
        for (bool changed = true; changed;) {
            changed = false;
            for (int i = static_cast<int>(ins.size()) - 1; i >= 0; i--) {
                if (!in[static_cast<size_t>(i)].known) continue;
                const Result& r = results[static_cast<size_t>(i)];
                std::set<Reg> live;
                if (r.fallExit >= 0) addUses(live, r.exits[static_cast<size_t>(r.fallExit)]);
                else if (r.fallsThrough && i + 1 < static_cast<int>(ins.size())) live = liveIn[static_cast<size_t>(i) + 1];
                for (auto l = r.ops.rbegin(); l != r.ops.rend(); ++l) {
                    if (l->jumpTo >= 0) live.insert(liveIn[static_cast<size_t>(l->jumpTo)].begin(), liveIn[static_cast<size_t>(l->jumpTo)].end());
                    if (l->jumpExit >= 0) addUses(live, r.exits[static_cast<size_t>(l->jumpExit)]);
                    for (const Reg& d : l->defs) live.erase(d);
                    if (l->exit >= 0) addUses(live, r.exits[static_cast<size_t>(l->exit)]);
                    live.insert(l->uses.begin(), l->uses.end());
                }
                if (live != liveIn[static_cast<size_t>(i)]) {
                    liveIn[static_cast<size_t>(i)] = std::move(live);
                    changed = true;
                }
            }
        }
        for (int e : entries)
            for (const Reg& r : liveIn[static_cast<size_t>(e)])
                if (r.slot < 0) refuse("a scratch register read before it is written");
    }

    // ---- Registers ----

    // Each value its register: two that are wanted at once in two, and those never wanted at
    // once sharing one, as the processor has nine for ints and eight for floats.
    std::map<Reg, int> regs;

    std::map<Reg, std::set<Reg>> clash;

    bool giveRegisters() {
        regs.clear();
        clash.clear();
        std::map<Reg, std::set<Reg>> moves;
        auto meet = [&](const Reg& a, const Reg& b) {
            if (a.cls != b.cls || a == b) return;
            clash[a].insert(b);
            clash[b].insert(a);
        };
        auto note = [&](const Reg& r) { clash[r]; };
        auto together = [&](const std::set<Reg>& live) {
            for (const Reg& a : live) {
                note(a);
                for (const Reg& b : live) meet(a, b);
            }
        };
        // What each way in loads, at once.
        for (int e : entries) together(liveIn[static_cast<size_t>(e)]);
        for (size_t i = 0; i < ins.size(); i++) {
            if (!in[i].known) continue;
            const Result& r = results[i];
            for (const ExitPoint& e : r.exits)
                if (e.toEntry >= 0) together(liveIn[static_cast<size_t>(e.toEntry)]);
            // Backwards through its steps, as findLive goes: what a step writes meets what is
            // wanted after it.
            std::set<Reg> live;
            if (r.fallExit >= 0) addUses(live, r.exits[static_cast<size_t>(r.fallExit)]);
            else if (r.fallsThrough && i + 1 < ins.size()) live = liveIn[i + 1];
            for (auto l = r.ops.rbegin(); l != r.ops.rend(); ++l) {
                if (l->jumpTo >= 0) live.insert(liveIn[static_cast<size_t>(l->jumpTo)].begin(), liveIn[static_cast<size_t>(l->jumpTo)].end());
                if (l->jumpExit >= 0) addUses(live, r.exits[static_cast<size_t>(l->jumpExit)]);
                // A move's two ends do not clash for the move itself: they may share a register,
                // and the move is then nothing.
                const bool isMove = (l->stem == "movi" || l->stem == "movf") && l->named.size() == 2;
                if (isMove) {
                    moves[l->named[0]].insert(l->named[1]);
                    moves[l->named[1]].insert(l->named[0]);
                }
                for (const Reg& d : l->defs) {
                    note(d);
                    for (const Reg& x : live)
                        if (!(isMove && x == l->named[1])) meet(d, x);
                }
                for (const Reg& d : l->defs) live.erase(d);
                if (l->exit >= 0) addUses(live, r.exits[static_cast<size_t>(l->exit)]);
                live.insert(l->uses.begin(), l->uses.end());
                for (const Reg& u : l->named) note(u);
            }
            for (const ExitPoint& e : r.exits)
                for (const LOp& l : e.stores)
                    for (const Reg& u : l.named) note(u);
        }
        // The most clashing first, each the lowest register none of those it meets has.
        std::vector<Reg> order;
        for (const auto& [r, others] : clash) order.push_back(r);
        std::stable_sort(order.begin(), order.end(), [&](const Reg& a, const Reg& b) { return clash[a].size() > clash[b].size(); });
        for (const Reg& r : order) {
            const int limit = r.cls == CI ? 9 : 8;
            std::vector<bool> taken(static_cast<size_t>(limit), false);
            for (const Reg& o : clash[r]) {
                auto g = regs.find(o);
                if (g != regs.end()) taken[static_cast<size_t>(g->second)] = true;
            }
            int got = -1;
            // A register of what it is moved from or to, where it is free.
            for (const Reg& m : moves[r]) {
                auto g = regs.find(m);
                if (got < 0 && g != regs.end() && !taken[static_cast<size_t>(g->second)]) got = g->second;
            }
            for (int k = 0; k < limit && got < 0; k++)
                if (!taken[static_cast<size_t>(k)]) got = k;
            if (got < 0) return false;
            regs[r] = got;
        }
        return true;
    }

    // ---- Values kept in their slots, where there are too many for the registers ----

    std::set<Reg> spilled;
    int temps = 0;

    // What is written back of a slot as the state `st` has it: its step, or "" where that is not
    // one kind.
    static LOp storeOf(const State& st, const Reg& r) {
        LOp l;
        l.slot = r.slot;
        const K k = st.kind(r.slot);
        const uint16_t w = static_cast<uint16_t>(k.bits & ~kEntry);
        if (r.cls == CF) {
            if (w == kFloat) l.stem = "storef";
        } else if (w == kInt) {
            l.stem = "storei";
        } else if (w == kByte) {
            l.stem = "storeby";
        } else if (w == kWhole && k.src >= 0) {
            l.stem = "storew";
            l.slot2 = k.src;
        }
        return l;
    }

    // One more value kept in its slot: of those that can be (a slot's, one kind wherever it is
    // written, not changed in place by Math's functions), the one clashing most for how often it
    // is used. False where there is none.
    bool spillOne() {
        std::map<Reg, int> used;
        std::set<Reg> barred;
        for (size_t i = 0; i < ins.size(); i++) {
            if (!in[i].known) continue;
            const Result& res = results[i];
            for (const LOp& l : res.ops) {
                for (const Reg& u : l.uses) used[u]++;
                for (const Reg& d : l.defs) {
                    used[d]++;
                    if (ins[i].op == Op::Numeric || storeOf(res.after, d).stem.empty()) barred.insert(d);
                }
            }
        }
        const Reg* best = nullptr;
        double bestScore = -1;
        for (const auto& [r, others] : clash) {
            if (r.slot < 0 || spilled.count(r) || barred.count(r)) continue;
            const double score = static_cast<double>(others.size()) / (1.0 + used[r]);
            if (score > bestScore) {
                bestScore = score;
                best = &r;
            }
        }
        if (best == nullptr) return false;
        const Reg r = *best;
        spilled.insert(r);
        // Each read of it loaded first, into a register of its own; each write written back after.
        for (size_t i = 0; i < ins.size(); i++) {
            if (!in[i].known) continue;
            Result& res = results[i];
            std::vector<LOp> out;
            for (LOp l : res.ops) {
                std::vector<LOp> after;
                const bool reads = std::find(l.uses.begin(), l.uses.end(), r) != l.uses.end();
                const bool writes = std::find(l.defs.begin(), l.defs.end(), r) != l.defs.end();
                if (reads || writes) {
                    const Reg t{-100 - temps++, r.cls};
                    if (reads) {
                        LOp load;
                        load.stem = r.cls == CI ? "loadi" : "loadf";
                        load.named = {t};
                        load.defs = {t};
                        load.slot = r.slot;
                        out.push_back(load);
                    }
                    if (writes) {
                        LOp store = storeOf(res.after, r);
                        store.named = {t};
                        store.uses = {t};
                        after.push_back(store);
                    }
                    for (auto* list : {&l.named, &l.uses, &l.defs})
                        for (Reg& x : *list)
                            if (x == r) x = t;
                }
                out.push_back(l);
                out.insert(out.end(), after.begin(), after.end());
            }
            res.ops = std::move(out);
            // Kept in its slot: nothing of it written back at a way out.
            for (ExitPoint& e : res.exits)
                std::erase_if(e.stores, [&](const LOp& l) { return !l.named.empty() && l.named[0] == r; });
        }
        return true;
    }
    std::string name(const LOp& l) const {
        std::string s = l.stem;
        for (const Reg& r : l.named) s += "_" + std::to_string(regs.at(r));
        return s;
    }

    // How much the region does itself, beside what it leaves to the VM each time round: where it
    // would leave too often for what it does between, the VM alone is as quick.
    bool worthIt() const {
        int work = 0, islands = 0;
        for (size_t i = 0; i < ins.size(); i++) {
            if (!in[i].known) continue;
            if (results[i].island) {
                islands++;
                continue;
            }
            for (const LOp& l : results[i].ops)
                if (l.stem != "jump" && l.stem != "movi" && l.stem != "movf") work++;
        }
        static const int ratio = [] {
            const char* given = std::getenv("JADE_NATIVE_RATIO");
            return given != nullptr ? std::atoi(given) : 6;
        }();
        return work >= 3 && work >= ratio * islands;
    }
};

// ---- Making the machine code ----

// What a way in loads: each register it reads before writing, from its slot. Where the region
// takes a slot as entered to hold what it made there on another way (an int, a float, a byte),
// the slot is checked to hold that first, and the region leaves at once where it does not (to
// `leave`): what it made may rest on a guess, which says nothing of what it is entered with.
void loads(Assembler& a, const Region& r, int entry, std::vector<std::string>* names, int leave) {
    const State& st = r.in[static_cast<size_t>(entry)];
    for (const Reg& reg : r.liveIn[static_cast<size_t>(entry)]) {
        const K k = st.kind(reg.slot);
        const uint16_t w = static_cast<uint16_t>(k.bits & ~kEntry);
        if (!(k.bits & kEntry) || w == 0) continue;
        const char* check = reg.cls == CF ? (w == kFloat ? "chkf" : nullptr)
                            : w == kInt   ? "chkint"
                            : w == kByte  ? "chkbyk"
                            : (w & ~kWholes) == 0 ? "chki"
                                                  : nullptr;
        if (check == nullptr) throw std::runtime_error("a slot of two kinds at a way in");
        if (names != nullptr) names->push_back(check);
        Step& c = a.add(check);
        c.slot = reg.slot;
        c.exit = leave;
    }
    for (const Reg& reg : r.liveIn[static_cast<size_t>(entry)]) {
        const std::string n = std::string(reg.cls == CI ? "loadi_" : "loadf_") + std::to_string(r.regs.at(reg));
        if (names != nullptr) names->push_back(n);
        a.add(n).slot = reg.slot;
    }
}

// The region's code, and each way in: (the instruction, its address in the code).
std::vector<std::pair<uint32_t, void*>> make(Region& r, std::vector<std::string>* names) {
    Assembler a;
    std::vector<int> labels(r.ins.size());
    for (int& l : labels) l = a.label();
    auto add = [&](const std::string& n) -> Step& {
        if (names != nullptr) names->push_back(n);
        return a.add(n);
    };
    struct Out {
        int label;
        const ExitPoint* exit;
    };
    std::vector<Out> outs;
    // Leaving at a way in, before anything is done: back to the VM at that instruction.
    std::map<int, int> leaving;
    auto leaveAt = [&](int e) {
        auto found = leaving.find(e);
        if (found != leaving.end()) return found->second;
        return leaving[e] = a.label();
    };
    const int start = a.label();
    a.at(start);
    loads(a, r, 0, names, leaveAt(0));
    for (size_t i = 0; i < r.ins.size(); i++) {
        a.at(labels[i]);
        if (!r.in[i].known) continue;
        const Result& res = r.results[i];
        std::vector<int> exitLabels;
        for (size_t e = 0; e < res.exits.size(); e++) {
            exitLabels.push_back(a.label());
            if (static_cast<int>(e) != res.fallExit) outs.push_back(Out{exitLabels.back(), &res.exits[e]});
        }
        for (const LOp& l : res.ops) {
            // A move to the register it is in: nothing.
            if ((l.stem == "movi" || l.stem == "movf") && l.named.size() == 2 && r.regs.at(l.named[0]) == r.regs.at(l.named[1])) continue;
            Step& st = add(r.name(l));
            st.ki = l.ki;
            st.kf = l.kf;
            st.slot = l.slot;
            st.slot2 = l.slot2;
            if (l.jumpTo >= 0) st.jump = labels[static_cast<size_t>(l.jumpTo)];
            if (l.jumpExit >= 0) st.jump = exitLabels[static_cast<size_t>(l.jumpExit)];
            if (l.exit >= 0) st.exit = exitLabels[static_cast<size_t>(l.exit)];
        }
        // Going on to an instruction the VM comes back in at: written back and loaded, here.
        if (res.fallExit >= 0) {
            const ExitPoint& e = res.exits[static_cast<size_t>(res.fallExit)];
            for (const LOp& l : e.stores) {
                Step& st = add(r.name(l));
                st.slot = l.slot;
                st.slot2 = l.slot2;
            }
            loads(a, r, e.toEntry, names, leaveAt(e.toEntry));
        }
    }
    // The ways out: what the region wrote that the VM may read written back, then where the VM
    // goes on and the stack's top there; or, to an instruction the VM comes back in at, what it
    // loads, and on to it.
    for (const Out& o : outs) {
        a.at(o.label);
        for (const LOp& l : o.exit->stores) {
            Step& st = add(r.name(l));
            st.slot = l.slot;
            st.slot2 = l.slot2;
        }
        if (o.exit->toEntry >= 0) {
            loads(a, r, o.exit->toEntry, names, leaveAt(o.exit->toEntry));
            add("jump").jump = labels[static_cast<size_t>(o.exit->toEntry)];
        } else {
            add("ret").ret = static_cast<int64_t>(o.exit->at) | (static_cast<int64_t>(o.exit->state.top) << 32) |
                             (o.exit->miss ? (int64_t{1} << 48) : 0);
        }
    }
    // The other ways in: what each reads loaded, then on to its instruction.
    std::vector<std::pair<int, int>> ways;
    for (int e : r.entries) {
        if (e == 0) continue;
        const int label = a.label();
        a.at(label);
        loads(a, r, e, names, leaveAt(e));
        add("jump").jump = labels[static_cast<size_t>(e)];
        ways.emplace_back(e, label);
    }
    for (const auto& [e, label] : leaving) {
        a.at(label);
        const State& st = r.in[static_cast<size_t>(e)];
        add("ret").ret = static_cast<int64_t>(r.ins[static_cast<size_t>(e)].at) | (static_cast<int64_t>(st.top) << 32) | (int64_t{1} << 48);
    }
    void* code = a.finish({});
    if (code == nullptr) return {};
    std::vector<std::pair<uint32_t, void*>> out{{r.ins[0].at, a.address(start)}};
    for (const auto& [e, label] : ways) out.emplace_back(r.ins[static_cast<size_t>(e)].at, a.address(label));
    return out;
}

// The loops of a function's run code: each Loop jumping back, with where it jumps.
struct Candidate {
    int first, last;  // instructions, the loop's start and its Loop
};

void makeRegions(FunctionProto& f) {
    if (!enabled()) return;
    std::vector<Ins> all;
    std::unordered_map<uint32_t, int> index;
    for (size_t at = 0; at < f.run.size();) {
        const char* name = nullptr;
        const size_t size = runSize(f.run, at + kThreaded, name);
        Ins x;
        x.at = static_cast<uint32_t>(at);
        x.op = opAt(f.run.data() + at + kThreaded);
        x.o = f.run.data() + at + kThreaded + kOpBytes;
        x.next = static_cast<uint32_t>(at + kThreaded + size);
        index[x.at] = static_cast<int>(all.size());
        all.push_back(x);
        at += kThreaded + size;
    }
    // The VM's top where each run instruction starts; where two places say two, not known.
    std::unordered_map<uint32_t, int> tops;
    for (const auto& [at, top] : f.runTops) {
        auto [it, added] = tops.emplace(at, top);
        if (!added && it->second != top) it->second = -1;
    }
    std::vector<Candidate> loops;
    for (size_t i = 0; i < all.size(); i++) {
        if (all[i].op != Op::Loop) continue;
        auto head = index.find(static_cast<uint32_t>(i32(all[i], 0)));
        if (head == index.end() || head->second > static_cast<int>(i)) continue;
        loops.push_back(Candidate{head->second, static_cast<int>(i)});
    }
    // The outermost first: a loop inside one made into a region is in it already.
    std::sort(loops.begin(), loops.end(), [](const Candidate& x, const Candidate& y) { return x.last - x.first > y.last - y.first; });
    std::vector<Candidate> made;
    static const char* const dump = std::getenv("JADE_DUMP_NATIVE");
    const bool dumping = dump != nullptr && (f.name == dump || std::strcmp(dump, "*") == 0);
    for (const Candidate& c : loops) {
        bool inside = false;
        for (const Candidate& m : made) inside = inside || (c.first <= m.last && c.last >= m.first);
        if (inside) continue;
        auto headTop = tops.find(all[static_cast<size_t>(c.first)].at);
        if (headTop == tops.end() || headTop->second < 0) continue;
        Region r(f);
        r.tops = tops;
        // A range counted through, made just before the loop: its step, where none is given, is 1
        // (or -1, going down), as the VM's RangeLoop sets it.
        if (all[static_cast<size_t>(c.first)].op == Op::RangeNext && c.first > 0 && all[static_cast<size_t>(c.first) - 1].op == Op::RangeLoop) {
            const int flags = u8(all[static_cast<size_t>(c.first) - 1], 0);
            if ((flags & 4) == 0) r.headConstants[u16(all[static_cast<size_t>(c.first)], 0) + 2] = (flags & 2) ? -1 : 1;
        }
        for (int i = c.first; i <= c.last; i++) {
            r.index[all[static_cast<size_t>(i)].at] = static_cast<int>(r.ins.size());
            r.ins.push_back(all[static_cast<size_t>(i)]);
        }
        try {
            r.learnHints();
            r.analyse(headTop->second);
            if (!r.worthIt()) refuse("too little done between what the VM does");
            r.findLive();
            while (!r.giveRegisters()) {
                if (!r.spillOne()) refuse("more values at once than registers, and none can be kept in its slot");
                r.findLive();
            }
            std::vector<std::string> names;
            const auto ways = make(r, dumping ? &names : nullptr);
            if (ways.empty()) continue;
            uint32_t* misses = new uint32_t(0);  // kept as the region's code is, for as long as the program runs
            for (const auto& [at, code] : ways) f.regions.push_back(FunctionProto::NativeRegion{at, code, misses});
            made.push_back(c);
            if (dumping) {
                int islands = 0;
                for (size_t i = 0; i < r.ins.size(); i++) islands += r.in[i].known && r.results[i].island ? 1 : 0;
                std::fprintf(stderr, "== %s: a region of %zu instructions at %u, %zu ways in, %d the VM's, %zu steps:\n  ", f.name.c_str(),
                             r.ins.size(), r.ins[0].at, ways.size(), islands, names.size());
                for (const std::string& s : names) std::fprintf(stderr, "%s ", s.c_str());
                std::fprintf(stderr, "\n");
                for (size_t i = 0; i < r.ins.size(); i++)
                    if (r.in[i].known && r.results[i].island) std::fprintf(stderr, "  the VM's: %s at %u\n", opName(r.ins[i].op), r.ins[i].at);
            }
        } catch (const std::exception& e) {
            if (dumping) {
                std::fprintf(stderr, "== %s: no region for the loop at %u: %s\n", f.name.c_str(), all[static_cast<size_t>(c.first)].at, e.what());
                for (size_t i = 0; i < r.results.size() && i < r.in.size(); i++)
                    if (r.in[i].known && r.results[i].island) std::fprintf(stderr, "  the VM's: %s at %u\n", opName(r.ins[i].op), r.ins[i].at);
            }
        }
    }
}

#if JADE_NATIVE
const bool hooked = (makeNativeRegions = &makeRegions, true);
#endif

}  // namespace

}  // namespace jade::native
