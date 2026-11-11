// Native code's regions (M9.5, docs/native-plan.html, step 3): loops of a function's run code
// whose every instruction has register stencils, made into machine code as the run code is
// finished. The VM goes into one where the loop starts (that instruction's address in the code
// is VM::nativeEnter's), and the region gives back where the VM goes on.
//
// What a region does, it does as the VM would: the values it keeps in registers are written
// back to their slots at every way out, and a way out for anything the stencils do not do (an
// int's overflow, the VM's flag at a loop's end) goes back to the VM at the instruction that
// needs it, which then does it as it always does, error and message alike. So a region is only
// ever made where every instruction is understood: anything else, and the loop stays the VM's.
//
// This step's instructions: the typed int and float arithmetic, comparisons and jumps of M6.7
// step 5 (on operands the checker knows), moves, constants, a local pushed, `and`, `or`, `!` of
// a comparison, a pop, JumpIfFalse, a jump, and a loop's end.

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

// What a slot may hold where the region is: bits. Entry: what it held as the region began
// (untouched on some way here).
constexpr uint8_t kEntry = 1, kInt = 2, kFloat = 4, kBool = 8;

[[noreturn]] void refuse(const std::string& why) { throw std::runtime_error(why); }

// The typed instructions, by what they are.
struct Typed {
    enum Kind { None, Arith, Compare, Jump } kind = None;
    bool isFloat = false;
    char op = 0;     // + - * / for Arith; l (<) e (<=) g (>) G (>=) = (==) for the others
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
const char* compareName(char op) {
    switch (op) {
        case 'l': return "lt";
        case 'e': return "le";
        case 'g': return "gt";
        case 'G': return "ge";
        default: return "eq";
    }
}

// ---- What the code does to the stack and the slots ----

struct State {
    bool known = false;
    int top = -1;
    std::map<int, uint8_t> kinds;  // slots the region has written on some way here; the rest kEntry
    int flag = -1;                 // the slot whose bool the flag register holds, or -1
    uint8_t kind(int slot) const {
        auto f = kinds.find(slot);
        return f == kinds.end() ? kEntry : f->second;
    }
};

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
        const uint8_t joined = into.kind(s) | from.kind(s);
        if (joined != into.kind(s)) {
            into.kinds[s] = joined;
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

struct Region {
    const FunctionProto& f;
    std::vector<Ins> ins;                          // the loop's instructions, in order
    std::unordered_map<uint32_t, int> index;       // where an instruction starts -> its place in `ins`
    std::vector<State> in;                         // the state each instruction starts in
    std::set<std::pair<int, uint8_t>> loads;       // (slot, kInt or kFloat) read from its slot as the region begins
    std::set<int> written;                         // slots the region writes
    std::map<std::pair<int, uint8_t>, int> regs;   // (slot, kind) -> its register
    std::map<int, uint8_t> hints;                  // what each slot is read and written as anywhere in the loop

    // What each slot is read and written as, by the typed instructions, and so what a slot
    // copied to or from one is (a local pushed, then read by an int's addition): what a move of
    // a slot the region has not yet written takes it to be.
    void learnHints() {
        auto note = [&](int slot, uint8_t kind) { hints[slot] |= kind; };
        for (const Ins& x : ins) {
            const Typed t = typed(x.op);
            const uint8_t kind = t.isFloat ? kFloat : kInt;
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
        // Across copies, until nothing more is learnt. (A push's slot is the stack's top, which
        // this does not follow: that is the state's; so only moves between named slots.)
        for (bool changed = true; changed;) {
            changed = false;
            for (const Ins& x : ins) {
                if (x.op != Op::MoveS) continue;
                const int to = u8(x, 0), from = u8(x, 1);
                const uint8_t both = static_cast<uint8_t>(hints[to] | hints[from]);
                if (both != hints[to] || both != hints[from]) {
                    hints[to] = hints[from] = both;
                    changed = true;
                }
            }
        }
    }

    explicit Region(const FunctionProto& fn) : f(fn) {}

    const Value& constant(int k) const {
        if (k < 0 || static_cast<size_t>(k) >= f.constants.size()) refuse("a constant out of range");
        return f.constants[static_cast<size_t>(k)];
    }
    int64_t intConstant(int k) const {
        const Value& v = constant(k);
        if (!(v.isInt() || v.isByte())) refuse("an int's constant that is no int");
        return v.as.i;
    }
    double floatConstant(int k) const {
        const Value& v = constant(k);
        if (!v.isFloat()) refuse("a float's constant that is no float");
        return v.as.d;
    }

    // A read of `slot` as `need`, as a typed instruction reads it.
    void read(State& s, int slot, uint8_t need) {
        const uint8_t k = s.kind(slot);
        if ((k & ~(kEntry | need)) != 0) refuse("a slot read as another kind than it holds");
        if (need == kBool) {
            if (k != kBool || s.flag != slot) refuse("a bool read that is not the last comparison's");
            return;
        }
        if (k & kEntry) loads.insert({slot, need});
    }
    void write(State& s, int slot, uint8_t kind) {
        s.kinds[slot] = kind;
        written.insert(slot);
        if (kind == kBool) s.flag = slot;
        else if (s.flag == slot) s.flag = -1;
    }
    // What a slot holds, for a move or a push of it: one kind, known.
    uint8_t knownKind(State& s, int slot, int to = -1) {
        const uint8_t k = s.kind(slot);
        if (k == kInt || k == kFloat) return k;
        if (k == kEntry) {
            // Not written yet: what it is read as elsewhere, or what what it is copied to is.
            uint8_t hint = hints.count(slot) ? hints[slot] : 0;
            if (hint == 0 && to >= 0 && hints.count(to)) hint = hints[to];
            if (hint == kInt || hint == kFloat) {
                read(s, slot, hint);
                return hint;
            }
        }
        if (k == (kEntry | kInt) || k == (kEntry | kFloat)) {
            const uint8_t kind = static_cast<uint8_t>(k & ~kEntry);
            if (!loads.count({slot, kind})) refuse("a slot moved before the region knows what it holds");
            return kind;
        }
        refuse("a slot moved whose kind is not known here");
    }

    // ---- The analysis: each instruction's state, from the loop's start ----

    // Where control goes from instruction `i` in state `s`: each successor (an instruction of the
    // region, or a place outside it) with its state.
    struct Edge {
        int to = -1;          // an instruction of the region, or -1 for a place outside
        uint32_t at = 0;      // the place outside
        State state;
    };
    std::vector<Edge> step(int i, State s) {
        const Ins& x = ins[static_cast<size_t>(i)];
        std::vector<Edge> out;
        auto go = [&](uint32_t target, const State& st) {
            auto found = index.find(target);
            Edge e;
            e.state = st;
            if (found != index.end()) e.to = found->second;
            else e.at = target;
            out.push_back(std::move(e));
        };
        auto fall = [&](const State& st) { go(x.next, st); };
        const Typed t = typed(x.op);
        switch (t.kind) {
            case Typed::Arith: {
                const uint8_t kind = t.isFloat ? kFloat : kInt;
                if (t.form != 2) read(s, u8(x, 1), kind);
                if (t.form != 1) read(s, u8(x, 2), kind);
                write(s, u8(x, 0), kind);
                s.top = u8(x, 3);
                fall(s);
                return out;
            }
            case Typed::Compare: {
                const uint8_t kind = t.isFloat ? kFloat : kInt;
                read(s, u8(x, 1), kind);
                if (t.form == 0) read(s, u8(x, 2), kind);
                write(s, u8(x, 0), kBool);
                s.top = u8(x, 3);
                fall(s);
                return out;
            }
            case Typed::Jump: {
                const uint8_t kind = t.isFloat ? kFloat : kInt;
                read(s, u8(x, 0), kind);
                if (t.form == 0) read(s, u8(x, 1), kind);
                s.top = u8(x, 2);
                go(static_cast<uint32_t>(i32(x, 3)), s);
                fall(s);
                return out;
            }
            default: break;
        }
        switch (x.op) {
            case Op::MoveS: {
                const uint8_t kind = knownKind(s, u8(x, 1), u8(x, 0));
                write(s, u8(x, 0), kind);
                fall(s);
                return out;
            }
            case Op::MoveK: {
                const Value& v = constant(u8(x, 1));
                if (!(v.isInt() || v.isFloat())) refuse("a constant moved that is no number");
                write(s, u8(x, 0), v.isFloat() ? kFloat : kInt);
                fall(s);
                return out;
            }
            case Op::Const: {
                const Value& v = constant(u16(x, 0));
                if (!(v.isInt() || v.isFloat())) refuse("a constant pushed that is no number");
                write(s, s.top, v.isFloat() ? kFloat : kInt);
                s.top++;
                fall(s);
                return out;
            }
            case Op::GetLocal: {
                const uint8_t kind = knownKind(s, u16(x, 0), s.top);
                write(s, s.top, kind);
                s.top++;
                fall(s);
                return out;
            }
            case Op::SetTop:
                s.top = u8(x, 0);
                fall(s);
                return out;
            case Op::RNot:  // u8 A, u8 B, u8 T, u8 S: the comparison's answer turned round
                read(s, u8(x, 1), kBool);
                write(s, u8(x, 0), kBool);
                s.top = u8(x, 2);
                fall(s);
                return out;
            case Op::Pop:
                s.top--;
                fall(s);
                return out;
            case Op::RJumpIfFalse:
                read(s, u8(x, 0), kBool);
                s.top = u8(x, 1);
                go(static_cast<uint32_t>(i32(x, 2)), s);
                fall(s);
                return out;
            case Op::And:
            case Op::Or: {
                read(s, s.top - 1, kBool);
                go(static_cast<uint32_t>(i32(x, 0)), s);  // keeping the value
                State popped = s;
                popped.top--;
                fall(popped);
                return out;
            }
            case Op::Jump:
                go(static_cast<uint32_t>(i32(x, 0)), s);
                return out;
            case Op::Loop:
                go(static_cast<uint32_t>(i32(x, 0)), s);
                return out;
            default: refuse(std::string("an instruction with no stencils: ") + opName(x.op));
        }
    }

    void analyse(int headTop) {
        in.assign(ins.size(), State{});
        State start;
        start.top = headTop;
        join(in[0], start);
        std::vector<int> work{0};
        while (!work.empty()) {
            const int i = work.back();
            work.pop_back();
            for (const Edge& e : step(i, in[static_cast<size_t>(i)]))
                if (e.to >= 0 && join(in[static_cast<size_t>(e.to)], e.state)) work.push_back(e.to);
        }
        for (const State& s : in)
            if (!s.known) refuse("an instruction of the loop never reached");
    }

    // ---- Registers ----

    void giveRegisters() {
        int ints = 0, floats = 0;
        auto give = [&](int slot, uint8_t kind) {
            if (regs.count({slot, kind})) return;
            if (kind == kInt) {
                if (ints == 5) refuse("more than five ints");
                regs[{slot, kind}] = ints++;
            } else {
                if (floats == 8) refuse("more than eight floats");
                regs[{slot, kind}] = floats++;
            }
        };
        for (const auto& [slot, kind] : loads) give(slot, kind);
        for (const State& s : in)
            for (const auto& [slot, k] : s.kinds)
                for (uint8_t kind : {kInt, kFloat})
                    if (k & kind) give(slot, kind);
        // And what the last instruction writes, which no state after it holds.
        for (size_t i = 0; i < ins.size(); i++)
            for (const Edge& e : step(static_cast<int>(i), in[i]))
                for (const auto& [slot, k] : e.state.kinds)
                    for (uint8_t kind : {kInt, kFloat})
                        if (k & kind) give(slot, kind);
    }
    int reg(int slot, uint8_t kind) const {
        auto f = regs.find({slot, kind});
        if (f == regs.end()) refuse("a value with no register");
        return f->second;
    }
};

// ---- Making the machine code ----

struct Exit {
    uint32_t at;
    State state;
    int label;
};

void* make(Region& r, std::vector<std::string>* names) {
    Assembler a;
    std::vector<int> labels(r.ins.size());
    for (int& l : labels) l = a.label();
    std::vector<Exit> exits;
    auto exitTo = [&](uint32_t at, const State& s) {
        exits.push_back(Exit{at, s, a.label()});
        return exits.back().label;
    };
    auto add = [&](const std::string& name) -> Step& {
        if (names != nullptr) names->push_back(name);
        return a.add(name);
    };
    auto n = [](const char* stem, std::initializer_list<int> regs) {
        std::string s = stem;
        for (int k : regs) s += "_" + std::to_string(k);
        return s;
    };
    // The region begins: what it reads from slots before writing them.
    for (const auto& [slot, kind] : r.loads) add(n(kind == kInt ? "loadi" : "loadf", {r.reg(slot, kind)})).slot = slot;
    for (size_t i = 0; i < r.ins.size(); i++) {
        const Ins& x = r.ins[i];
        const State& s = r.in[i];
        a.at(labels[i]);
        // Where a jump to `target` goes: its instruction's label, or a way out there.
        auto target = [&](uint32_t to, const State& st) {
            auto found = r.index.find(to);
            return found != r.index.end() ? labels[static_cast<size_t>(found->second)] : exitTo(to, st);
        };
        const Typed t = typed(x.op);
        const uint8_t kind = t.isFloat ? kFloat : kInt;
        const char* tk = t.isFloat ? "f" : "i";
        if (t.kind == Typed::Arith) {
            const int dst = r.reg(u8(x, 0), kind);
            const char* stem = t.op == '+' ? "add" : t.op == '-' ? "sub" : t.op == '*' ? "mul" : "div";
            std::string name;
            Step* st = nullptr;
            if (t.form == 0) {
                st = &add(n((std::string(stem) + tk).c_str(), {dst, r.reg(u8(x, 1), kind), r.reg(u8(x, 2), kind)}));
            } else {
                const int k = t.form == 1 ? u8(x, 2) : u8(x, 1);
                const int src = r.reg(u8(x, t.form == 1 ? 1 : 2), kind);
                // b + 1 into b: an increment.
                if (!t.isFloat && t.op == '+' && dst == src && r.intConstant(k) == 1) {
                    st = &add(n("inci", {dst}));
                } else {
                    const bool reversed = t.form == 2 && (t.op == '-' || t.op == '/');
                    st = &add(n((std::string(stem) + tk + (reversed ? "kr" : "k")).c_str(), {dst, src}));
                    if (t.isFloat) st->kf = r.floatConstant(k);
                    else st->ki = r.intConstant(k);
                }
            }
            if (!t.isFloat) st->exit = exitTo(x.at, s);  // an overflow: the VM does the instruction
            continue;
        }
        if (t.kind == Typed::Compare) {
            Step& st = add(t.form == 0 ? n((std::string(compareName(t.op)) + tk + tk).c_str(), {r.reg(u8(x, 1), kind), r.reg(u8(x, 2), kind)})
                                       : n((std::string(compareName(t.op)) + tk + "k").c_str(), {r.reg(u8(x, 1), kind)}));
            if (t.form == 1) {
                if (t.isFloat) st.kf = r.floatConstant(u8(x, 2));
                else st.ki = r.intConstant(u8(x, 2));
            }
            continue;
        }
        if (t.kind == Typed::Jump) {
            State after = s;
            after.top = u8(x, 2);
            Step& st = add(t.form == 0 ? n((std::string("jn") + compareName(t.op) + tk + tk).c_str(), {r.reg(u8(x, 0), kind), r.reg(u8(x, 1), kind)})
                                       : n((std::string("jn") + compareName(t.op) + tk + "k").c_str(), {r.reg(u8(x, 0), kind)}));
            if (t.form == 1) {
                if (t.isFloat) st.kf = r.floatConstant(u8(x, 1));
                else st.ki = r.intConstant(u8(x, 1));
            }
            st.jump = target(static_cast<uint32_t>(i32(x, 3)), after);
            continue;
        }
        switch (x.op) {
            case Op::MoveS:
            case Op::GetLocal: {
                const int from = x.op == Op::MoveS ? u8(x, 1) : u16(x, 0);
                const int to = x.op == Op::MoveS ? u8(x, 0) : s.top;
                State copy = s;
                const uint8_t k = r.knownKind(copy, from, to);
                const int dst = r.reg(to, k), src = r.reg(from, k);
                if (dst != src) add(n(k == kInt ? "movi" : "movf", {dst, src}));
                break;
            }
            case Op::MoveK:
            case Op::Const: {
                const int to = x.op == Op::MoveK ? u8(x, 0) : s.top;
                const Value& v = r.constant(x.op == Op::MoveK ? u8(x, 1) : u16(x, 0));
                if (v.isFloat()) add(n("setfk", {r.reg(to, kFloat)})).kf = v.as.d;
                else add(n("setik", {r.reg(to, kInt)})).ki = v.as.i;
                break;
            }
            case Op::SetTop:
            case Op::Pop: break;
            case Op::RNot: add("notflag"); break;
            case Op::RJumpIfFalse: {
                State after = s;
                after.top = u8(x, 1);
                add("jfalse").jump = target(static_cast<uint32_t>(i32(x, 2)), after);
                break;
            }
            case Op::And:
            case Op::Or: add(x.op == Op::And ? "jfalse" : "jtrue").jump = target(static_cast<uint32_t>(i32(x, 0)), s); break;
            case Op::Jump: add("jump").jump = target(static_cast<uint32_t>(i32(x, 0)), s); break;
            case Op::Loop: {
                Step& st = add("loopback");
                st.jump = target(static_cast<uint32_t>(i32(x, 0)), s);
                st.exit = exitTo(x.at, s);  // the VM's flag: the VM does the loop's end, and comes back
                break;
            }
            default: refuse(std::string("an instruction with no stencils: ") + opName(x.op));
        }
    }
    // The ways out: each slot the region wrote and the VM may read written back, then where the
    // VM goes on and the stack's top there.
    for (size_t e = 0; e < exits.size(); e++) {
        const Exit x = exits[e];  // a copy: making a step may grow `exits` no further, but be safe
        a.at(x.label);
        for (int slot : r.written) {
            if (slot >= x.state.top) continue;  // above the stack: nothing reads it
            const uint8_t k = x.state.kind(slot);
            if (k == kEntry) continue;            // not written on the way here
            const uint8_t written = static_cast<uint8_t>(k & ~kEntry);
            if (written != kInt && written != kFloat && written != kBool) refuse("a slot of two kinds at a way out");
            if ((k & kEntry) && written != kBool && !r.loads.count({slot, written})) refuse("a slot written on some ways out only");
            if (written == kBool) {
                if ((k & kEntry) || x.state.flag != slot) refuse("a bool at a way out that the flag register does not hold");
                add("storeb").slot = slot;
            } else {
                add(n(written == kInt ? "storei" : "storef", {r.reg(slot, written)})).slot = slot;
            }
        }
        add("ret").ret = static_cast<int64_t>(x.at) | (static_cast<int64_t>(x.state.top) << 32);
    }
    return a.finish({});
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
    const bool dumping = dump != nullptr && f.name == dump;
    for (const Candidate& c : loops) {
        bool inside = false;
        for (const Candidate& m : made) inside = inside || (c.first <= m.last && c.last >= m.first);
        if (inside) continue;
        int headTop = -1;
        for (const auto& [at, top] : f.runTops)
            if (at == all[static_cast<size_t>(c.first)].at) headTop = top;
        if (headTop < 0) continue;
        Region r(f);
        for (int i = c.first; i <= c.last; i++) {
            r.index[all[static_cast<size_t>(i)].at] = static_cast<int>(r.ins.size());
            r.ins.push_back(all[static_cast<size_t>(i)]);
        }
        try {
            r.learnHints();
            r.analyse(headTop);
            r.giveRegisters();
            std::vector<std::string> names;
            void* code = make(r, dumping ? &names : nullptr);
            if (code == nullptr) continue;
            f.regions.push_back(FunctionProto::NativeRegion{r.ins[0].at, code});
            made.push_back(c);
            if (dumping) {
                std::fprintf(stderr, "== %s: a region of %zu instructions at %u, %zu steps:\n  ", f.name.c_str(), r.ins.size(), r.ins[0].at, names.size());
                for (const std::string& s : names) std::fprintf(stderr, "%s ", s.c_str());
                std::fprintf(stderr, "\n");
            }
        } catch (const std::exception& e) {
            if (dumping) std::fprintf(stderr, "== %s: no region for the loop at %u: %s\n", f.name.c_str(), all[static_cast<size_t>(c.first)].at, e.what());
        }
    }
}

#if JADE_NATIVE
const bool hooked = (makeNativeRegions = &makeRegions, true);
#endif

}  // namespace

}  // namespace jade::native
