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
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <map>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
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

// An instruction that ends the function (a return, or a throw), where a region leaves for the VM
// for good: what is left of the function is the VM's.
bool endsFunction(Op op) { return op == Op::Return || op == Op::ReturnInit || op == Op::CheckReturn || op == Op::Throw; }
// Whether an instruction may go anywhere but on to the next.
bool mayJump(Op op) {
    if (targetOperand(op) > 0 || op == Op::Loop) return true;
    const std::string_view name = opName(op);
    return name.find("JUMP") != std::string_view::npos;
}
// For each of `count` instructions (their ops by `opAt(i)`), whether it goes straight on, with no
// jump, to one that ends the function: a loop's way out by `return` or `throw`, its work (the
// message made, the exception) done once, not each time round.
template <typename OpAt>
std::vector<bool> endingRuns(int count, OpAt opAt) {
    std::vector<bool> ending(static_cast<size_t>(count), false);
    for (int i = count - 1; i >= 0; i--) {
        const Op op = opAt(i);
        ending[static_cast<size_t>(i)] = endsFunction(op) || (!mayJump(op) && i + 1 < count && ending[static_cast<size_t>(i) + 1]);
    }
    return ending;
}

// ---- Kinds ----

// What a slot may hold where the region is: bits. Entry: what it held as the region was entered
// (untouched on some way here), in memory, and in its register where the region reads it.
constexpr uint16_t kEntry = 1, kInt = 2, kFloat = 4, kBool = 8, kByte = 16;
constexpr uint16_t kWhole = 32;   // an int or a byte read from the list or bytes in slot `src`
constexpr uint16_t kCopy = 64;    // a copy of slot `src` as it was entered, read there
constexpr uint16_t kCopyI = 128;  // the same, its payload in an int register as well (a whole number)
constexpr uint16_t kCopyF = 256;  // the same, in a float register
constexpr uint16_t kMem = 512;    // in its slot, which the region wrote (an item, a field): read there
constexpr uint16_t kWholes = kInt | kByte | kWhole | kCopyI;
constexpr uint16_t kCopies = kCopy | kCopyI | kCopyF;

struct K {
    uint16_t bits = kEntry;
    int16_t src = -1;      // for kWhole and the copies; -2 where two meet
    bool known = false;    // an int whose value is known on every way here: `value`
    int64_t value = 0;
    int16_t fact = -1;     // what the region knows the slot holds on every way here (Region::facts)
    bool operator==(const K& o) const {
        return bits == o.bits && src == o.src && known == o.known && (!known || value == o.value) && fact == o.fact;
    }
};
K constantInt(int64_t v) {
    K k{kInt};
    k.known = true;
    k.value = v;
    return k;
}

// Slots and their kinds, in slot order: a sorted array, which a state is copied as often as the
// analysis goes on (a tree's nodes each made anew were most of its time).
class Kinds {
public:
    using Item = std::pair<int, K>;
    using iterator = std::vector<Item>::iterator;
    using const_iterator = std::vector<Item>::const_iterator;
    iterator begin() { return items_.begin(); }
    iterator end() { return items_.end(); }
    const_iterator begin() const { return items_.begin(); }
    const_iterator end() const { return items_.end(); }
    size_t size() const { return items_.size(); }
    const_iterator find(int slot) const {
        auto at = std::lower_bound(items_.begin(), items_.end(), slot, [](const Item& i, int s) { return i.first < s; });
        return at != items_.end() && at->first == slot ? at : items_.end();
    }
    K& operator[](int slot) {
        auto at = std::lower_bound(items_.begin(), items_.end(), slot, [](const Item& i, int s) { return i.first < s; });
        if (at == items_.end() || at->first != slot) at = items_.insert(at, Item{slot, K{}});
        return at->second;
    }
    iterator erase(iterator at) { return items_.erase(at); }
    void erase(int slot) {
        auto at = std::lower_bound(items_.begin(), items_.end(), slot, [](const Item& i, int s) { return i.first < s; });
        if (at != items_.end() && at->first == slot) items_.erase(at);
    }
    std::vector<Item>& items() { return items_; }
    const std::vector<Item>& items() const { return items_; }

private:
    std::vector<Item> items_;
};

struct State {
    bool known = false;
    int top = -1;
    Kinds kinds;  // slots the region has written on some way here; the rest kEntry
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

// What the region analysed now knows of a value that may also be null (Region::facts): for
// each fact, the one that says the same of a value that may be null (what it holds a guess the
// steps check, null among what they leave to the VM); fact 0 is null itself.
thread_local const std::vector<int16_t>* maybeNullFacts = nullptr;
int16_t joinFacts(int16_t a, int16_t b) {
    if (a == b) return a;
    if (a < 0 || b < 0 || maybeNullFacts == nullptr) return -1;
    const auto& n = *maybeNullFacts;
    if (static_cast<size_t>(a) >= n.size() || static_cast<size_t>(b) >= n.size()) return -1;
    // Null on one way, something known on the other (`x < 0 ? null : cells[i]`): that, or null.
    if (a == 0) return n[static_cast<size_t>(b)];
    if (b == 0) return n[static_cast<size_t>(a)];
    return n[static_cast<size_t>(a)] == n[static_cast<size_t>(b)] ? n[static_cast<size_t>(a)] : static_cast<int16_t>(-1);
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
    // The two in slot order, merged.
    const auto& as = into.kinds.items();
    const auto& bs = from.kinds.items();
    std::vector<Kinds::Item> merged;
    merged.reserve(as.size() + bs.size());
    size_t i = 0, k = 0;
    while (i < as.size() || k < bs.size()) {
        int s;
        K a, b;
        if (k == bs.size() || (i < as.size() && as[i].first < bs[k].first)) {
            s = as[i].first, a = as[i++].second;
        } else if (i == as.size() || bs[k].first < as[i].first) {
            s = bs[k].first, b = bs[k++].second;
        } else {
            s = as[i].first, a = as[i++].second, b = bs[k++].second;
        }
        K j{static_cast<uint16_t>(a.bits | b.bits), a.src};
        const bool aSrc = (a.bits & (kWhole | kCopies | kMem)) != 0, bSrc = (b.bits & (kWhole | kCopies | kMem)) != 0;
        if (!aSrc) j.src = b.src;
        else if (bSrc && a.src != b.src) j.src = -2;
        j.known = a.known && b.known && a.value == b.value;
        j.value = j.known ? a.value : 0;
        j.fact = joinFacts(a.fact, b.fact);
        if (!(j == a)) changed = true;
        merged.push_back(Kinds::Item{s, j});
    }
    if (changed) into.kinds.items() = std::move(merged);
    const int flag = into.flag == from.flag ? into.flag : -1;
    if (flag != into.flag) {
        into.flag = flag;
        changed = true;
    }
    return changed;
}

bool sameState(const State& a, const State& b) {
    if (a.known != b.known || a.top != b.top || a.flag != b.flag) return false;
    const auto& as = a.kinds.items();
    const auto& bs = b.kinds.items();
    size_t i = 0, k = 0;
    const K none{};
    while (i < as.size() || k < bs.size()) {
        if (k == bs.size() || (i < as.size() && as[i].first < bs[k].first)) {
            if (!(as[i++].second == none)) return false;
        } else if (i == as.size() || bs[k].first < as[i].first) {
            if (!(bs[k++].second == none)) return false;
        } else if (!(as[i++].second == bs[k++].second)) {
            return false;
        }
    }
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

// Held in memory, as entered or as the region wrote it there: read from the slot.
bool inMemory(const K& k) { return (k.bits & kMem) != 0 && (k.bits & ~(kEntry | kMem)) == 0; }

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
    const ObjString* field = nullptr;  // the field a getp or setp step reads or writes, where its name is known
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
    bool ends = false;         // one that ends the function (a return in the loop): the VM's, never back
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
    // Slots a field holding a list is read into in the loop, and what that list holds where the
    // region is made (-2: fields of two kinds into one slot): a guess, checked as the frame's.
    mutable std::map<int, int> fieldLists;
    std::vector<State> in;                     // the state each instruction starts in
    std::set<int> entries;                     // where the VM comes in
    std::set<int> resumed;                     // of them, those after an island: known only as memory holds them
    std::map<int, int64_t> headConstants;      // slots known as the loop starts (a range's step)
    std::map<int, State> comeBack;             // what a way in after an island starts knowing, this round
    std::map<int, State> nextComeBack;         // and for the next round, from this round's islands and ways
    // Lists whose items are read as ints, each read checking the object is no bytes (chklist):
    // where such an item meets an int the region made in one variable, it is written back as an
    // int on every way (a byte is written back as a byte). Found when the stores at a way out
    // meet one (wantInts), and the region made again with them.
    std::set<int> intLists;
    mutable std::set<int> wantInts;
    std::vector<Result> results;               // each instruction's, in its state

    explicit Region(const FunctionProto& fn) : f(fn) {}

    // The slots of the call the regions are made in (VM::nativeFirst), or null: the list in a slot
    // there says what a list read from that slot is likely to hold. A guess the steps check.
    // Only the slots below the VM's top are read: those above may hold values the collector
    // has freed since.
    const Value* frame = nullptr;
    int frameSlots = 0;
    // What a number in the frame's slot is now, where the region is made: CI, CF, or -1.
    int numberClass(int slot) const {
        if (frame == nullptr || slot < 0 || slot >= frameSlots || slot >= f.maxStack) return -1;
        const Value& v = frame[slot];
        return v.isInt() ? CI : v.isFloat() ? CF : -1;
    }
    // What a list (or bytes) holds: CI, CF, or -1 (not one, or of values).
    static int listClass(const Value& v) {
        if (v.isObjType(ObjType::Bytes)) return CI;
        if (!v.isObjType(ObjType::List) || !asList(v)->packed()) return -1;
        const ListStore store = asList(v)->store();
        if (store == ListStore::Narrow && asList(v)->pack->narrow == Narrow::Byte) return CI;  // a List<byte>, read as bytes are
        return store == ListStore::Floats ? CF : store == ListStore::Ints ? CI : -1;
    }
    // What the list in a slot holds, in the call the region is made in: read by the region from a
    // field (fieldLists), what that field holds; otherwise the frame's slot.
    int frameClass(int slot) const {
        if (auto it = fieldLists.find(slot); it != fieldLists.end() && it->second >= 0) return it->second;
        if (frame == nullptr || slot < 0 || slot >= frameSlots || slot >= f.maxStack) return -1;
        return listClass(frame[slot]);
    }
    // What the list in a field holds, where the region is made: the field of the object in slot
    // `objSlot` (as the frame holds it) where the place's cache says, if the cache saw that
    // object's layout and a field kept as a value.
    // Where the cache has seen nothing (the read not made yet: a loop inside that has not run),
    // the field named by the instruction's own way, at `slow` in the run code.
    // The name a GetProp or GetOwn uses, where an R instruction's own way (at `slow`) holds it;
    // with `sets`, a SetProp's or SetOwn's.
    const ObjString* slowName(int32_t slow, bool sets = false) const {
        if (slow < 0) return nullptr;
        for (size_t at = static_cast<size_t>(slow), n = 0; n < 4 && at + kThreaded + kOpBytes + 2 <= f.run.size(); n++) {
            const uint8_t* const p = f.run.data() + at + kThreaded;
            const Op o = opAt(p);
            if (sets ? (o == Op::SetProp || o == Op::SetOwn) : (o == Op::GetProp || o == Op::GetOwn)) {
                const size_t k = static_cast<size_t>(p[kOpBytes] | (p[kOpBytes + 1] << 8));
                if (k >= f.constants.size() || !f.constants[k].isObjType(ObjType::String)) return nullptr;
                return asString(f.constants[k]);
            }
            const char* name = nullptr;
            at += kThreaded + runSize(f.run, at + kThreaded, name);
        }
        return nullptr;
    }
    // The value type of the items of the list the frame's iterator in slot `itSlot` goes through,
    // where they are kept in the value itself (a Color) with no type of their own; null for any other.
    // With `where`, of a list of any value type's items, none null, read where they are.
    const ObjClass* iteratedClass(int itSlot, bool where = false) const {
        if (frame == nullptr || itSlot < 0 || itSlot >= frameSlots || itSlot >= f.maxStack) return nullptr;
        const Value& v = frame[itSlot];
        if (!v.isObjType(ObjType::Iterator)) return nullptr;
        const Value& source = asIterator(v)->source;
        if (!source.isObjType(ObjType::List) || asList(source)->store() != ListStore::Items) return nullptr;
        const PackedItems& pack = *asList(source)->pack;
        const ObjClass* const cls = pack.itemClass;
        if (where) return pack.nullable ? nullptr : cls;
        if (cls == nullptr || cls->smallId == 0 || pack.itemType != nullptr || pack.typeSlot != 0) return nullptr;
        const ValueLayout* const layout = cls->layout.load(std::memory_order_acquire);
        return layout != nullptr && layout->size <= 8 ? cls : nullptr;
    }
    // Whether instruction `i` is on the straight way to a return or a throw (endingRuns).
    std::vector<bool> ending_;
    bool endingAt(int i) {
        if (ending_.size() != ins.size()) ending_ = endingRuns(static_cast<int>(ins.size()), [&](int k) { return ins[static_cast<size_t>(k)].op; });
        return i >= 0 && static_cast<size_t>(i) < ending_.size() && ending_[static_cast<size_t>(i)];
    }
    // What global `k` holds as the region is made (nativeGlobalsSeen); null where not known.
    static const Value* globalSeen(int k) {
        if (nativeGlobalsSeen.values == nullptr || k < 0 || static_cast<size_t>(k) >= nativeGlobalsSeen.count) return nullptr;
        return nativeGlobalsSeen.values + k;
    }
    // The class an RCall at `i` makes a value of from its header alone, as the VM's RCall does at
    // once, where slot `callee` holds it as the region is made: kept in the value itself, every
    // field of its header a byte; nothing of a type given right before. Null for anything else.
    const ObjClass* madeValue(const State& s, int callee, int argc, int i) const {
        const Value* v = knownValue(s, callee);
        if (v == nullptr || !v->isObjType(ObjType::Class)) return nullptr;
        const ObjClass* const cls = asClass(*v);
        if (!cls->isValue || !cls->madeFromHeader || cls->staticInit != nullptr || cls->fieldTypesMaker != nullptr || cls->smallId == 0 ||
            cls->components.size() != static_cast<size_t>(argc))
            return nullptr;
        const ValueLayout* const layout = cls->layout.load(std::memory_order_acquire);
        if (layout == nullptr || layout->size > 8) return nullptr;
        for (int k = 0; k < argc; k++) {
            const ValueField* field = headerFieldOf(cls, layout, k);
            if (field == nullptr || field->kind != ValueField::Narrow || field->narrow != Narrow::Byte) return nullptr;
        }
        for (int back = 1; back <= 2 && i - back >= 0; back++)
            if (ins[static_cast<size_t>(i - back)].op == Op::WithTypes) return nullptr;
        return cls;
    }
    // The field of a value type's header's component `k`: the field of its name.
    static const ValueField* headerFieldOf(const ObjClass* cls, const ValueLayout* layout, int k) {
        if (layout == nullptr || k < 0 || static_cast<size_t>(k) >= cls->components.size()) return nullptr;
        const ObjString* const name = cls->components[static_cast<size_t>(k)];
        for (const ValueField& field : layout->fields)
            if (field.name == name || (field.name != nullptr && name != nullptr && field.name->utf8() == name->utf8())) return &field;
        return nullptr;
    }
    // Whether the region makes a closure, which may hold slots of the frame open.
    bool makesClosures() const {
        for (const Ins& x : ins)
            if (x.op == Op::Closure) return true;
        return false;
    }
    // Whether the frame's iterator in slot `itSlot` goes through a list of a value type's items.
    bool iteratesItems(int itSlot) const {
        if (frame == nullptr || itSlot < 0 || itSlot >= frameSlots || itSlot >= f.maxStack) return false;
        const Value& v = frame[itSlot];
        if (!v.isObjType(ObjType::Iterator)) return false;
        const Value& source = asIterator(v)->source;
        return source.isObjType(ObjType::List) && asList(source)->store() == ListStore::Items;
    }
    // The value type of the items of the list the frame holds in slot `listSlot`, where it holds a
    // list of a value type's items; null for anything else.
    const ObjClass* itemsClass(const State& s, int listSlot) const {
        if (const Fact* known = factOf(s, listSlot); known != nullptr && known->items != nullptr) return known->items;
        if (frame == nullptr || listSlot < 0 || listSlot >= frameSlots || listSlot >= f.maxStack) return nullptr;
        const Value& v = frame[listSlot];
        if (!v.isObjType(ObjType::List) || asList(v)->store() != ListStore::Items) return nullptr;
        return asList(v)->pack->itemClass;
    }
    // What the region knows a slot holds on the way to a step (K::fact): a value of a value type,
    // made or read there, of that type (an item read: a Ref or a Small value, B1); an object read
    // from a field or a global, as it was where the region is made (`image.cells.cells`, B2; the
    // class called to make a value, B3); a list of a value type's items, their type. Each kept
    // once, by what it says, so that two ways that know the same meet knowing it; two that know
    // otherwise meet knowing nothing, and the frame's slot is the guess again. Guesses all, which
    // the steps check. Before (to 2026-11-12) they were kept by the slot alone, for the whole
    // region, and a slot that held two things on two stretches (the image's pixels, then the image,
    // in `im[x, y] = Color(...)`) was known as neither.
    struct Fact {
        bool hasObject = false;
        Value object{};
        const ObjClass* items = nullptr;
        const ObjClass* value = nullptr;
        int from = -1;  // the instruction whose field read (getpv) put it there: one looked at once is held
        bool isNull = false;  // null itself (fact 0)
        bool operator==(const Fact& o) const {
            return hasObject == o.hasObject && (!hasObject || (object.kind == o.object.kind && object.as.i == o.object.as.i)) && items == o.items &&
                   value == o.value && from == o.from && isNull == o.isNull;
        }
    };
    static Fact nullFact() {
        Fact f;
        f.isNull = true;
        return f;
    }
    mutable std::vector<Fact> facts = std::vector<Fact>(1, nullFact());
    // For each fact, the one for a value that may be null as well (joinFacts): the same but for
    // `from`, as a slot that may be null is no field looked at once.
    mutable std::vector<int16_t> maybeNull = std::vector<int16_t>(1, 0);
    int16_t intern(const Fact& f) const {
        auto found = std::find(facts.begin(), facts.end(), f);
        if (found != facts.end()) return static_cast<int16_t>(found - facts.begin());
        if (facts.size() >= 32000) return -1;
        facts.push_back(f);
        const int16_t id = static_cast<int16_t>(facts.size() - 1);
        maybeNull.push_back(id);
        if (f.from >= 0) {
            Fact g = f;
            g.from = -1;
            const int16_t other = intern(g);
            maybeNull[static_cast<size_t>(id)] = other;
        }
        return id;
    }
    const Fact* factOf(const State& s, int slot) const {
        K k = s.kind(slot);
        // A copy of a slot as entered: what is known of that slot.
        if (k.fact < 0 && (k.bits & kCopies) != 0 && (k.bits & ~kCopies) == 0 && k.src >= 0) k = s.kind(k.src);
        return k.fact >= 0 && static_cast<size_t>(k.fact) < facts.size() ? &facts[static_cast<size_t>(k.fact)] : nullptr;
    }
    template <class Change>
    void noteFact(State& s, int slot, Change change) const {
        const K k = s.kind(slot);
        Fact f = k.fact >= 0 ? facts[static_cast<size_t>(k.fact)] : Fact{};
        f.isNull = false;
        change(f);
        const int16_t id = intern(f);
        if (id >= 0) s.kinds[slot].fact = id;
    }
    void noteValue(State& s, int slot, const ObjClass* cls) const {
        noteFact(s, slot, [&](Fact& f) { f.value = cls; });
    }
    void noteItems(State& s, int slot, const ObjClass* items) const {
        noteFact(s, slot, [&](Fact& f) { f.items = items; });
    }
    const ObjClass* valueClass(const State& s, int slot) const {
        if (const Fact* known = factOf(s, slot); known != nullptr && known->value != nullptr) return known->value;
        if (frame == nullptr || slot < 0 || slot >= frameSlots || slot >= f.maxStack) return nullptr;
        const Value& v = frame[slot];
        if (v.isSmall()) return smallClassOf(v);
        if (v.isRef() && v.refDepth == 0 && v.refOffset == 0 && v.as.obj->type == ObjType::List && asList(v)->store() == ListStore::Items)
            return asList(v)->pack->itemClass;
        return nullptr;
    }
    // The field a GetProp (whose cache is `cache`) reads of a value of the type `cls`, where the
    // cache saw that type: a byte, an int or a float; null for any other.
    const ValueField* plainField(const ObjClass* cls, int cache, int32_t slow, const ObjString* named = nullptr) const {
        const ValueField* const found = fieldOf(cls, cache, slow, named);
        if (found == nullptr) return nullptr;
        const ValueField& field = *found;
        const bool isByte = field.kind == ValueField::Narrow && field.narrow == Narrow::Byte;
        return isByte || field.kind == ValueField::Int || field.kind == ValueField::Float ? &field : nullptr;
    }
    // The field `items[i].x` reads (RGetIndexProp), where the frame holds a list of a value type's
    // items in slot `listSlot` and the place's cache knows the field: its class in `cls`. Null for
    // anything else.
    const ValueField* itemField(int listSlot, int cache, int32_t slow, const ObjClass*& cls) const {
        if (frame == nullptr || listSlot < 0 || listSlot >= frameSlots || listSlot >= f.maxStack) return nullptr;
        const Value& v = frame[listSlot];
        if (!v.isObjType(ObjType::List)) return nullptr;
        const ObjList* list = asList(v);
        if (list->store() != ListStore::Items || list->pack->itemClass == nullptr) return nullptr;
        const ObjClass* const c = list->pack->itemClass;
        const ValueField* const field = fieldOf(c, cache, slow);
        if (field != nullptr) cls = c;
        return field;
    }
    // The field of a value type `cls` a place reads: where its cache says, where the cache saw
    // that type; or, where the cache has seen nothing (a read the VM has not come to yet: the
    // second part of an `or`), the field named by the instruction's own way, at `slow`.
    const ValueField* fieldOf(const ObjClass* cls, int cache, int32_t slow, const ObjString* named = nullptr) const {
        if (cls == nullptr || f.caches == nullptr || cache < 0 || cache >= static_cast<int>(f.cacheCount)) return nullptr;
        const ValueLayout* const layout = cls->layout.load(std::memory_order_acquire);
        if (layout == nullptr) return nullptr;
        const uint64_t seen = f.caches[cache].load(std::memory_order_relaxed);
        if (seen == 0) {
            const ObjString* const name = named != nullptr ? named : slowName(slow);
            if (name == nullptr) return nullptr;
            for (const ValueField& field : layout->fields)
                if (field.name != nullptr && field.name->utf8() == name->utf8()) return &field;
            return nullptr;
        }
        if (static_cast<uint32_t>(seen >> 32) != cls->shape) return nullptr;
        const size_t k = (seen >> 16) & 0xFFFF;
        return k < layout->fields.size() ? &layout->fields[k] : nullptr;
    }
    // Whether a GetProp reads `size` of a list or bytes the frame holds in slot `objSlot`.
    bool sizeRead(int objSlot, int32_t slow) const {
        if (frame == nullptr || objSlot < 0 || objSlot >= frameSlots || objSlot >= f.maxStack) return false;
        const Value& v = frame[objSlot];
        if (!v.isObjType(ObjType::List) && !v.isObjType(ObjType::Bytes)) return false;
        const ObjString* const name = slowName(slow);
        return name != nullptr && name->utf8() == "size";
    }
    int fieldClass(const State& s, int objSlot, int cache, int32_t slow) const {
        Value field;
        return fieldValue(s, objSlot, cache, slow, field) ? listClass(field) : -1;
    }
    // The field itself, as the frame's object holds it where the region is made.
    bool fieldValue(const State& s, int objSlot, int cache, int32_t slow, Value& out) const {
        const Value* const known = knownValue(s, objSlot);
        if (known == nullptr) return false;
        const Value& v = *known;
        if (!v.isObjType(ObjType::Instance) || f.caches == nullptr || cache < 0 || cache >= static_cast<int>(f.cacheCount)) return false;
        const uint64_t seen = f.caches[cache].load(std::memory_order_relaxed);
        const ObjInstance* const inst = asInstance(v);
        if (seen == 0) {
            const ObjString* const name = slowName(slow);
            const Value* const field = name == nullptr ? nullptr : inst->field(name);
            if (field == nullptr) return false;
            out = *field;
            return true;
        }
        if ((seen & 0x80) != 0 || static_cast<uint32_t>(seen >> 32) != inst->shape) return false;
        const size_t at = static_cast<size_t>((seen >> 16) & 0xFFFF);
        if (at < sizeof(ObjInstance)) return false;
        std::memcpy(static_cast<void*>(&out), reinterpret_cast<const uint8_t*>(inst) + at, sizeof(Value));
        return true;
    }
    // What a slot holds where the region is made: what a field or global read in the region held
    // then (a Fact), or the frame's slot. A guess the steps check.
    void noteFieldValue(State& s, int slot, const Value& v) const {
        noteFact(s, slot, [&](Fact& f) {
            f.hasObject = true;
            f.object = v;
        });
    }
    const Value* knownValue(const State& s, int slot) const {
        if (const Fact* known = factOf(s, slot); known != nullptr && known->hasObject) return &known->object;
        if (frame == nullptr || slot < 0 || slot >= frameSlots || slot >= f.maxStack) return nullptr;
        return &frame[slot];
    }

    // The address of a place's cache (GetProp's and the like), which the stencils read as the VM
    // fills it: there as long as the run code is.
    int64_t cacheAddress(int cache) const {
        if (f.caches == nullptr || cache < 0 || cache >= static_cast<int>(f.cacheCount)) island();
        return static_cast<int64_t>(reinterpret_cast<uintptr_t>(f.caches.get() + cache));
    }

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
                case Op::Dup2:
                    // The two on top copied (`items[i] += v`): how the copy is read; or, where that
                    // says nothing, the slot itself later.
                    if (slot == top - 2 || slot == top - 1) {
                        const int copy = useClass(j, slot + 2, top + 2, depth + 1, guessing);
                        if (copy >= 0) return copy;
                    }
                    top += 2;
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
                case Op::GetIndexRef:
                    // list index -> item: reads the index as a whole number (a guess), the list as none.
                    if (slot == top - 1) return guessing ? CI : -1;
                    if (slot == top - 2) return -1;
                    top--;
                    j++;
                    continue;
                case Op::KeepLocal:
                case Op::OwnLocal:
                case Op::WithDestinationAt:
                    j++;
                    continue;
                case Op::PlaceIndex:
                    if (slot == top - 1) return guessing ? CI : -1;
                    if (slot == top - 2) return -1;
                    top--;
                    j++;
                    continue;
                case Op::RGetGlobal:
                    if (u8(x, 0) == slot) return -1;
                    top = u8(x, 1);
                    j++;
                    continue;
                case Op::PopUnder: {
                    // The top moved down: how it is read there; anything else under it gone.
                    const int first = top - 1 - u16(x, 0);
                    if (slot == top - 1 && first != slot) return useClass(j, first, first + 1, depth + 1, guessing);
                    if (slot >= first) return -1;
                    top = first + 1;
                    j++;
                    continue;
                }
                case Op::RGetIndexSS:
                case Op::RGetIndexSK:
                case Op::RGetIndexPropSS:
                case Op::RGetIndexPropSK: {
                    // u8 A, u8 B, u8 C, u8 T: reads C as an index (a whole number, a guess the
                    // step reading it checks), B as no number.
                    const bool indexSlot = x.op == Op::RGetIndexSS || x.op == Op::RGetIndexPropSS;
                    if (guessing && indexSlot && u8(x, 2) == slot && u8(x, 1) != slot) return CI;
                    if (u8(x, 0) == slot || u8(x, 1) == slot || (indexSlot && u8(x, 2) == slot)) return -1;
                    top = u8(x, 3);
                    j++;
                    continue;
                }
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
                            (void)other;  // a slot's hints may be another variable's, where slots are shared
                            return -1;
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
            if (w == kMem) continue;  // in its slot already
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
                // An int the region made meeting an item read from the list in slot `src`, an int or
                // a byte: made again with that list's items read as ints (intLists).
                if (w == (kInt | kWhole) && k.src >= 0 && !intLists.count(k.src)) wantInts.insert(k.src);
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
        // Whether the steps are made: only once the analysis is done, which follows the states
        // alone (the steps of its every time round were most of its time).
        bool lowering = true;
        LOp scratch;

        // The region leaves for the VM at this instruction, where a step cannot do its part.
        int exitHere() { return exitTo(x.at, s); }
        int exitTo(uint32_t at, const State& st) {
            if (!lowering) return 0;
            out.exits.push_back(ExitPoint{at, st, {}, {}, false, -1});  // what it writes back, worked out once the analysis is done
            return static_cast<int>(out.exits.size()) - 1;
        }
        // A step that may leave for the VM, which then does the instruction (and the rest of the
        // time round).
        void leaves(LOp& l) {
            l.exit = exitHere();
            if (lowering) out.exits.back().miss = true;
        }
        LOp& op(std::string_view stem, std::initializer_list<Reg> named = {}, std::initializer_list<Reg> uses = {},
                std::initializer_list<Reg> defs = {}) {
            if (!lowering) return scratch;
            out.ops.push_back(LOp{});
            LOp& l = out.ops.back();
            l.stem = stem;
            l.named = named;
            l.uses = uses;
            l.defs = defs;
            return l;
        }
        void go(uint32_t target, const State& st, LOp& jumping) {
            auto found = r.index.find(target);
            if (found == r.index.end()) {
                jumping.jumpExit = exitTo(target, st);
            } else if (r.resumed.count(found->second)) {
                jumping.jumpExit = exitTo(target, st);
                if (lowering) out.exits.back().toEntry = found->second;
                r.cameBy(found->second, asInMemory(st));
            } else {
                jumping.jumpTo = found->second;
                out.edges.push_back(Edge{found->second, st});
            }
        }
        void fall(const State& st) {
            if (r.resumed.count(i + 1)) {
                out.fallExit = exitTo(r.ins[static_cast<size_t>(i) + 1].at, st);
                if (lowering) out.exits.back().toEntry = i + 1;
                else out.fallExit = -1;
                r.cameBy(i + 1, asInMemory(st));
            } else {
                out.edges.push_back(Edge{i + 1, st});
            }
        }

        // A slot read as an int or a float: its register. Where the region has not written it
        // and only guesses what it holds (`checked`), the slot's kind is checked first, and the
        // VM does the instruction where the guess is wrong.
        Reg read(int slot, Cls cls, bool checked) {
            // A copy of a slot as entered: that slot's register; or as memory holds it.
            if (const K k = s.kind(slot); k.bits == kCopy && k.src >= 0 && (s.kind(k.src).bits == kEntry || inMemory(s.kind(k.src)))) slot = k.src;
            if (inMemory(s.kind(slot))) {
                if (checked) {
                    LOp& l = op(cls == CI ? "chki" : "chkf");
                    l.slot = slot;
                    leaves(l);
                }
                op(cls == CI ? "loadi" : "loadf", {Reg{slot, cls}}, {}, {Reg{slot, cls}}).slot = slot;
                return Reg{slot, cls};
            }
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
            if (const K k = s.kind(slot); k.bits == kCopy && k.src >= 0 && (s.kind(k.src).bits == kEntry || inMemory(s.kind(k.src)))) slot = k.src;
            const int known = inMemory(s.kind(slot)) ? -1 : classOf(s.kind(slot));
            if (known >= 0) return static_cast<Cls>(known);
            if (known == -2) island();
            auto h = r.hints.find(slot);
            if (h == r.hints.end()) {
                // No typed instruction says (a value only shifted, say): as entered, what the
                // frame holds now, where the region is made; checked where it is read, as a hint.
                if (s.kind(slot).bits == kEntry) {
                    const int now = r.numberClass(slot);
                    if (now >= 0) return static_cast<Cls>(now);
                }
                island();
            }
            if (h->second == kInt) return CI;
            if (h->second == kFloat) return CF;
            island();
        }
        void write(int slot, K k) {
            s.kinds[slot] = k;
            if (k.bits == kBool) s.flag = slot;
            else if (s.flag == slot) s.flag = -1;
        }
        // A slot about to be written in memory (an item, a field): a copy of it still wanted is
        // made a copy in fact first, in its own slot; a whole number read from the list there is
        // no longer told int or byte by it.
        void writeMemory(int slot) {
            for (auto& [y, k] : s.kinds) {
                if (y == slot || y >= s.top) continue;
                if ((k.bits & (kCopies | kWhole)) == 0 || k.src != slot) continue;
                if ((k.bits & ~kCopies) == 0) {
                    LOp& l = op("copyslot");
                    l.slot = y;
                    l.slot2 = slot;
                    k = K{kMem};
                } else {
                    k.bits = static_cast<uint16_t>(k.bits | kInt | kFloat);  // two kinds: read by the VM
                }
            }
            write(slot, K{kMem});
        }
        // Whether another slot below the top is a copy of `slot`, or a whole number read from the
        // list there: what a step writing the slot in place of writeMemory leaves out.
        bool copiedElsewhere(int slot) const {
            for (const auto& [y, k] : s.kinds)
                if (y != slot && y < s.top && (k.bits & (kCopies | kWhole)) != 0 && k.src == slot) return true;
            return false;
        }
        // The slot an object is read from: the slot itself, as entered, or the one it copies.
        int objectSlot(int slot) {
            const K k = s.kind(slot);
            if (k.bits == kEntry || inMemory(k)) return slot;
            if ((k.bits & ~kCopies) == 0 && k.src >= 0) return k.src;
            island();
        }
        // The flag register holding slot `slot`'s bool: as it is, or read from the slot.
        void flagOf(int slot) {
            const K k = s.kind(slot);
            if (k.bits == kBool && s.flag == slot) return;
            if (k.bits != kEntry && !inMemory(k)) island();
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
            if ((k.bits == kCopy || k.bits == kCopyI || k.bits == kCopyF) && k.src >= 0 && (s.kind(k.src).bits == kEntry || inMemory(s.kind(k.src))))
                from = k.src;
            else if (k.bits == kEntry || inMemory(k)) from = slot;
            if (from >= 0) {
                LOp& l = op(cls == CF ? "chkf" : exact ? "chkint" : "chki");
                l.slot = from;
                leaves(l);
                // Written in memory by the region: loaded here (as entered, its register is loaded
                // where the region is entered).
                if (inMemory(s.kind(from))) op(cls == CI ? "loadi" : "loadf", {Reg{from, cls}}, {}, {Reg{from, cls}}).slot = from;
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

    Result transfer(int i, const State& start, bool lowering = true) {
        const Ins& x = ins[static_cast<size_t>(i)];
        Ctx c{*this, i, x, start, {}, true, {}};
        c.lowering = lowering;
        try {
            step(c);
        } catch (const Island&) {
            static const bool trace = std::getenv("JADE_TRACE_ISLANDS") != nullptr;
            if (trace)
                std::fprintf(stderr, "  island: %s at %u (%s), first slots %d:%d %d:%d\n", opName(x.op), x.at, f.name.c_str(), x.o[0], start.kind(x.o[0]).bits,
                             x.o[1], start.kind(x.o[1]).bits);
            Result out;
            out.island = true;
            out.fallsThrough = false;
            out.exits.push_back(ExitPoint{x.at, start, {}, {}, false, -1});
            out.ops.push_back(LOp{});
            out.ops.back().stem = "jump";
            out.ops.back().jumpExit = 0;
            // A return or a throw, or on the straight way to one (the exception made, B2): the VM
            // goes on from here and never comes back into the loop.
            if (endsFunction(x.op) || endingAt(i)) {
                out.ends = true;
                return out;
            }
            // The VM comes back in after it, or where it jumps.
            out.resumes.push_back(i + 1);
            if (const int t = targetOperand(x.op); t > 0 && static_cast<int>(kOpBytes) + (t - 1) + 4 <= static_cast<int>(x.next - x.at - kThreaded)) {
                auto to = index.find(static_cast<uint32_t>(i32(x, t - 1)));
                if (to != index.end()) out.resumes.push_back(to->second);
            }
            return out;
        }
        if (lowering) c.out.after = c.s;
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
            case Op::Dup2: {  // the list and the index of `items[i] += v`, for the read and the write
                const int t = s.top;
                move(c, t - 2, t);
                s.top = t + 1;
                move(c, t - 1, t + 1);
                s.top = t + 2;
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
                } else if (v.isByte()) {
                    // A byte (a Color's part written as a number, B3): in a register, a byte.
                    c.op("setik", {Reg{to, CI}}, {}, {Reg{to, CI}}).ki = v.as.i;
                    c.write(to, K{kByte});
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
                if (const ObjClass* const cls = itemsClass(s, list)) {
                    // An item of a list of a value type's items (M9.5 B1): a copy of it, where it
                    // is kept in the value itself (a Color), in memory; anything else the VM's.
                    const ValueLayout* const layout = cls->layout.load(std::memory_order_acquire);
                    if (cls->smallId == 0 || layout == nullptr || layout->size > 8 || c.copiedElsewhere(a)) island();
                    Reg index{};
                    if (x.op == Op::RGetIndexSK) {
                        index = Reg{kScratchI, CI};
                        c.op("setik", {index}, {}, {index}).ki = c.intConstant(u8(x, 2));
                    } else {
                        index = c.read(u8(x, 2), CI, true);
                    }
                    LOp& l = c.op("idxsm", {index}, {index});
                    l.slot = list;
                    l.slot2 = static_cast<int64_t>((static_cast<uint64_t>(cls->smallId) << 32) | (static_cast<uint64_t>(a) << 8) | layout->size);
                    l.ki = static_cast<int64_t>(reinterpret_cast<intptr_t>(cls));
                    c.leaves(l);
                    c.write(a, K{kMem});
                    noteValue(s, a, cls);
                    s.top = u8(x, 3);
                    c.fall(s);
                    return;
                }
                // As read next; or a guess, which the step checks (an item of another kind leaves):
                // how it is used, or what the list holds in the call the region is made in.
                int cls = useClass(c.i, a, u8(x, 3));
                if (cls < 0) cls = useClass(c.i, a, u8(x, 3), 0, true);
                if (cls < 0) cls = frameClass(list);
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
                if (cls == CI && c.r.intLists.count(list)) {
                    // No bytes, so an int (a byte is left to the VM): kInt, written back as one.
                    LOp& chk = c.op("chklist");
                    chk.slot = list;
                    c.leaves(chk);
                    c.write(a, K{kInt});
                } else {
                    c.write(a, cls == CF ? K{kFloat} : K{kWhole, static_cast<int16_t>(list)});
                }
                s.top = u8(x, 3);
                c.fall(s);
                return;
            }
            case Op::GetIndexRef: {  // list index -> item, where it is (M9.5 B1)
                // An item of a list of a value type's items, as the frame's list is where the
                // region is made: a Ref to it, in memory. Anything else the VM's.
                const int top = s.top;
                const int list = c.objectSlot(top - 2);
                const ObjClass* const cls = itemsClass(s, list);
                if (cls == nullptr || c.copiedElsewhere(top - 2)) island();
                const Reg index = c.read(top - 1, CI, true);
                LOp& l = c.op("iref", {index}, {index});
                l.slot = list;
                l.slot2 = top - 2;
                l.ki = static_cast<int64_t>(reinterpret_cast<intptr_t>(cls));
                c.leaves(l);
                c.write(top - 2, K{kMem});
                noteValue(s, top - 2, cls);
                s.top = top - 1;
                c.fall(s);
                return;
            }
            case Op::KeepLocal: {  // u16 slot
                // Nothing to a number in a register; for one in memory, nothing unless it is a Ref
                // to a place of the frame's (the VM copies it).
                const int slot = u16(x, 0);
                const K k = s.kind(slot);
                if (k.bits == kEntry || inMemory(k)) {
                    LOp& l = c.op("keepl");
                    l.slot = slot;
                    c.leaves(l);
                } else if ((k.bits & (kInt | kFloat | kByte | kBool)) == 0 || (k.bits & ~(kInt | kFloat | kByte | kBool)) != 0) {
                    island();
                }
                c.fall(s);
                return;
            }
            case Op::RGetGlobal: {  // u8 A, u8 T, u8 S, u16 global, i32 slow: slot A = the global (B3)
                const int a = u8(x, 0);
                c.writeMemory(a);
                LOp& l = c.op("getg");
                l.ki = u16(x, 3);
                l.slot2 = a;
                c.leaves(l);
                if (const Value* g = globalSeen(u16(x, 3)); g != nullptr && g->kind == ValueKind::Obj) noteFieldValue(s, a, *g);
                s.top = u8(x, 1);
                c.fall(s);
                return;
            }
            case Op::RCall: {  // u8 A, u8 F, u8 T, u8 S, u8 argc, u8 flags, u8 X1..X4, i32 slow, i32 resume
                // A value of a value type kept in the value itself, made from its header's byte
                // fields (`Color(r, g, b, a)`, B3), where slot F holds its class as the region is
                // made (the step checks it), nothing of a type given right before, and each part a
                // byte the region knows: its bytes put together. Any other call the VM's.
                const int a = u8(x, 0), argc = u8(x, 4), flags = u8(x, 5);
                const int callee = c.objectSlot(u8(x, 1));
                const ObjClass* const cls = madeValue(s, callee, argc, c.i);
                if (cls == nullptr) island();
                const ValueLayout* const layout = cls->layout.load(std::memory_order_acquire);
                uint64_t constant = 0;
                std::vector<std::pair<Reg, int>> parts;  // a byte's register, and its offset
                for (int k = 0; k < argc; k++) {
                    const ValueField* field = headerFieldOf(cls, layout, k);
                    if (field == nullptr) island();
                    const int xk = u8(x, 6 + k);
                    if (flags & (1 << k)) {
                        const int64_t v = c.intConstant(xk);
                        if (v < 0 || v > 255) island();
                        constant |= static_cast<uint64_t>(v) << (8 * field->offset);
                    } else {
                        if (s.kind(xk).bits != kByte) island();
                        parts.emplace_back(c.read(xk, CI, false), static_cast<int>(field->offset));
                    }
                }
                const Reg acc{kScratchI, CI};
                c.op("setik", {acc}, {}, {acc}).ki = static_cast<int64_t>(constant);
                for (const auto& [reg, offset] : parts) c.op("packb", {acc, reg}, {acc, reg}, {acc}).ki = 8 * offset;
                c.writeMemory(a);
                LOp& l = c.op("mksm", {acc}, {acc});
                l.slot = callee;
                l.slot2 = static_cast<int64_t>((static_cast<uint64_t>(cls->smallId) << 32) | static_cast<uint64_t>(a));
                l.ki = static_cast<int64_t>(reinterpret_cast<intptr_t>(cls));
                c.leaves(l);
                noteValue(s, a, cls);
                s.top = u8(x, 2);
                c.fall(s);
                return;
            }
            case Op::WithDestinationAt:
                // The place the call after it makes its value in: the VM's way of making it where it
                // goes. A value the region makes (RCall) is put there by the SetIndex after; the VM,
                // given none, makes the same value as a value of its own.
                if (c.i + 1 >= static_cast<int>(ins.size()) || ins[static_cast<size_t>(c.i + 1)].op != Op::RCall) island();
                c.fall(s);
                return;
            case Op::PlaceIndex: {  // list index -> place: an item of a list of a value type's items, to be changed (B3)
                const int top = s.top;
                const int list = c.objectSlot(top - 2);
                if (itemsClass(s, list) == nullptr || c.copiedElsewhere(top - 2)) island();
                const Reg index = c.read(top - 1, CI, true);
                LOp& l = c.op("placei", {index}, {index});
                l.slot = list;
                l.slot2 = top - 2;
                c.leaves(l);
                c.write(top - 2, K{kMem});
                s.top = top - 1;
                c.fall(s);
                return;
            }
            case Op::Swap: {
                // The two on top changed round, where the next instruction pops the one on top
                // (`a[i] = V(...)`: the place under the value, dropped): the value moved down.
                const int top = s.top;
                if (c.i + 1 >= static_cast<int>(ins.size()) || ins[static_cast<size_t>(c.i + 1)].op != Op::Pop || top < 2) island();
                move(c, top - 1, top - 2);
                c.fall(s);
                return;
            }
            case Op::GetOwnRef:
            case Op::GetPropRef:  // (where it is: for a number or an object, the same as a read)
            case Op::GetOwn:
            case Op::GetProp: {  // u16 name, u16 cache: obj -> value
                // A byte, int or float field of a value of a value type (B2: read off what an
                // inline op [] gave); anything else the VM's.
                const int top = s.top;
                const int obj = c.objectSlot(top - 1);
                const Value& name = constant(u16(x, 0));
                const ObjClass* const vcls = valueClass(s, obj);
                if (vcls == nullptr) {
                    // An object a field holds, read on through (`cells.cells`: the field of what a
                    // field holds, which the VM reads through for a value kept in the object):
                    // copied as RGetProp's getpv copies it, where the field held an object as the
                    // region was made (a copy of an object is the object), and known as RGetProp's
                    // is, to be looked at once. Anything else the VM's.
                    Value held;
                    if (!fieldValue(s, obj, u16(x, 2), -1, held) || held.kind != ValueKind::Obj) island();
                    c.writeMemory(top - 1);
                    LOp& l = c.op("getpv");
                    l.slot = obj;
                    l.slot2 = top - 1;
                    l.ki = cacheAddress(u16(x, 2));
                    l.field = name.isObjType(ObjType::String) ? asString(name) : nullptr;
                    c.leaves(l);
                    noteFact(s, top - 1, [&](Fact& f) { f.from = c.i; });
                    noteFieldValue(s, top - 1, held);
                    if (held.isObjType(ObjType::List) && asList(held)->store() == ListStore::Items && asList(held)->pack->itemClass != nullptr)
                        noteItems(s, top - 1, asList(held)->pack->itemClass);
                    c.fall(s);
                    return;
                }
                const ValueField* const field = name.isObjType(ObjType::String) ? plainField(vcls, u16(x, 2), -1, asString(name)) : nullptr;
                if (field == nullptr) island();
                const bool isFloat = field->kind == ValueField::Float, isByte = field->kind == ValueField::Narrow;
                const Reg dst{top - 1, isFloat ? CF : CI};
                LOp& l = c.op(isFloat ? "valpf" : isByte ? "valpb" : "valpi", {dst}, {}, {dst});
                l.slot = obj;
                l.slot2 = static_cast<int64_t>((static_cast<uint64_t>(vcls->smallId) << 32) | field->offset);
                l.ki = static_cast<int64_t>(reinterpret_cast<intptr_t>(vcls));
                c.leaves(l);
                c.write(top - 1, K{static_cast<uint16_t>(isFloat ? kFloat : isByte ? kByte : kInt)});
                c.fall(s);
                return;
            }
            case Op::PopUnder: {  // u16 count: the top moved down over the `count` below it, which go
                // (The end of a block that gives a value, an inline op [] put in place, B2.) The
                // VM closes what a closure has of those slots: none where the region makes none.
                const int count = u16(x, 0);
                const int top = s.top;
                const int first = top - 1 - count;
                if (makesClosures() || first < 0) island();
                for (const auto& [y, k] : s.kinds)
                    if (y < first && (k.bits & (kCopies | kWhole)) != 0 && k.src >= first && k.src < top) island();
                if (count > 0) move(c, top - 1, first);
                s.top = first + 1;
                c.fall(s);
                return;
            }
            case Op::OwnLocal: {  // u16 slot
                // A Ref to an item the region made (a value kept in the value itself, a Color):
                // a copy of it; a number in a register as it is. Anything else the VM's.
                const int slot = u16(x, 0);
                const K k = s.kind(slot);
                if (k.bits == kCopy && k.src >= 0) {
                    // A copy of a slot as entered, not made yet (an argument of an inline body
                    // that is a parameter passed on: `image[x, y] = c`, 2026-11-13): made now,
                    // then taken as its own as one in memory is.
                    const ObjClass* const cls = valueClass(s, k.src) != nullptr ? valueClass(s, k.src) : valueClass(s, slot);
                    const ValueLayout* const layout = cls != nullptr ? cls->layout.load(std::memory_order_acquire) : nullptr;
                    if (cls == nullptr || cls->smallId == 0 || layout == nullptr || layout->size > 8) island();
                    c.writeMemory(slot);
                    LOp& copy = c.op("copyslot");
                    copy.slot = slot;
                    copy.slot2 = k.src;
                    LOp& l = c.op("ownsm");
                    l.slot = slot;
                    l.slot2 = static_cast<int64_t>((static_cast<uint64_t>(cls->smallId) << 32) | layout->size);
                    l.ki = static_cast<int64_t>(reinterpret_cast<intptr_t>(cls));
                    c.leaves(l);
                    c.write(slot, K{kMem});
                    noteValue(s, slot, cls);
                } else if (k.bits == kEntry || inMemory(k)) {
                    const ObjClass* const cls = valueClass(s, slot);
                    const ValueLayout* const layout = cls != nullptr ? cls->layout.load(std::memory_order_acquire) : nullptr;
                    if (cls == nullptr) {
                        // What the slot holds not known (null on one way, a value on another:
                        // `getOrNull`'s, 2026-11-13): anything but a Ref as it is, a Ref the VM's.
                        LOp& l = c.op("ownsm");
                        l.slot = slot;
                        c.leaves(l);
                        c.write(slot, K{kMem});
                        c.fall(s);
                        return;
                    }
                    if (cls->smallId == 0 || layout == nullptr || layout->size > 8) island();
                    LOp& l = c.op("ownsm");
                    l.slot = slot;
                    l.slot2 = static_cast<int64_t>((static_cast<uint64_t>(cls->smallId) << 32) | layout->size);
                    l.ki = static_cast<int64_t>(reinterpret_cast<intptr_t>(cls));
                    c.leaves(l);
                    c.write(slot, K{kMem});
                    noteValue(s, slot, cls);
                } else if ((k.bits & (kInt | kFloat | kByte | kBool)) == 0 || (k.bits & ~(kInt | kFloat | kByte | kBool)) != 0) {
                    island();
                }
                c.fall(s);
                return;
            }
            case Op::JumpIfNotClass: {  // u16 global, i32 target: value ->, on to the target unless of exactly that class
                const int top = s.top;
                LOp& l = c.op("clsflag");
                l.slot = c.objectSlot(top - 1);
                l.ki = u16(x, 0);
                State after = s;
                after.top = top - 1;
                after.flag = -1;  // the flag holds the answer, no slot's bool
                s = after;
                c.go(static_cast<uint32_t>(i32(x, 2)), after, c.op("jfalse"));
                c.fall(after);
                return;
            }
            case Op::RGetUpvalue: {  // u8 A, u8 T, u16 upvalue: slot A = what the captured variable holds
                const int a = u8(x, 0);
                c.writeMemory(a);
                LOp& l = c.op("getuv");
                l.slot2 = a;
                l.ki = u16(x, 2);
                s.top = u8(x, 1);
                c.fall(s);
                return;
            }
            case Op::AssertNotNull: {  // value -> value, null thrown for by the VM
                const int top = s.top;
                const K k = s.kind(top - 1);
                if ((k.bits & (kInt | kFloat | kByte | kBool)) != 0 && (k.bits & ~(kInt | kFloat | kByte | kBool)) == 0) {
                    c.fall(s);  // a number in a register: never null
                    return;
                }
                LOp& l = c.op("notnull");
                l.slot = c.objectSlot(top - 1);
                c.leaves(l);
                c.fall(s);
                return;
            }
            case Op::RGetIndexPropSS:
            case Op::RGetIndexPropSK: {  // u8 A, u8 B, u8 C, u8 T, u8 S, u16 cache, i32 slow
                // `items[i].x`: a byte, an int or a float field of an item of a list of a value
                // type's items, read where it is, of the class the frame's list has as the region
                // is made (the step checks it); anything else the VM's.
                const int a = u8(x, 0);
                const int list = c.objectSlot(u8(x, 1));
                const ObjClass* cls = nullptr;
                const ValueField* field = itemField(list, u16(x, 5), i32(x, 7), cls);
                if (field == nullptr) island();
                const bool isByte = field->kind == ValueField::Narrow && field->narrow == Narrow::Byte;
                if (!isByte && field->kind != ValueField::Int && field->kind != ValueField::Float) island();
                const bool isFloat = field->kind == ValueField::Float;
                Reg index{};
                if (x.op == Op::RGetIndexPropSK) {
                    index = Reg{kScratchI, CI};
                    c.op("setik", {index}, {}, {index}).ki = c.intConstant(u8(x, 2));
                } else {
                    index = c.read(u8(x, 2), CI, true);
                }
                const Reg dst{a, isFloat ? CF : CI};
                LOp& l = c.op(isFloat ? "idxpf" : isByte ? "idxpb" : "idxpi", {dst, index}, {index}, {dst});
                l.slot = list;
                l.slot2 = field->offset;
                l.ki = static_cast<int64_t>(reinterpret_cast<intptr_t>(cls));
                c.leaves(l);
                c.write(a, K{static_cast<uint16_t>(isFloat ? kFloat : isByte ? kByte : kInt)});
                s.top = u8(x, 3);
                c.fall(s);
                return;
            }
            case Op::SetIndex: {  // list index value -> value
                const int top = s.top;
                const int list = c.objectSlot(top - 3);
                if (const ObjClass* const cls = itemsClass(s, list)) {
                    // An item of a list of a value type's items kept in the value itself (B3): the
                    // value's bytes, where the region holds a value of the type in memory (or a
                    // copy of a slot as it was entered: a parameter, read there).
                    const ValueLayout* const layout = cls->layout.load(std::memory_order_acquire);
                    if (cls->smallId == 0 || layout == nullptr || layout->size > 8) island();
                    const int valueSlot = c.objectSlot(top - 1);
                    const Reg index = c.read(top - 2, CI, true);
                    LOp& l = c.op("setsm", {index}, {index});
                    l.slot = list;
                    l.slot2 = static_cast<int64_t>((static_cast<uint64_t>(cls->smallId) << 32) | (static_cast<uint64_t>(layout->size) << 24) |
                                                   static_cast<uint64_t>(valueSlot));
                    l.ki = static_cast<int64_t>(reinterpret_cast<intptr_t>(cls));
                    c.leaves(l);
                    move(c, top - 1, top - 3);
                    s.top = top - 2;
                    c.fall(s);
                    return;
                }
                const Reg index = c.read(top - 2, CI, true);
                const K v = s.kind(top - 1);
                // (A whole number read from a list, an int or a byte, goes as an int: into bytes
                // where it is from 0 to 255, into a list of ints as the int it is.)
                const char* stem = (v.bits == kFloat || v.bits == kCopyF) ? "setf" : (v.bits == kInt || v.bits == kWhole) ? "setwi" : v.bits == kByte ? "setwb" : nullptr;
                Reg value{top - 1, stem != nullptr && stem[3] == 'f' ? CF : CI};
                K result = v;
                if (stem == nullptr) {
                    // A value the region did not make (a variable pushed): what the list holds in
                    // the call the region is made in says what it is taken to be, checked in memory.
                    int root = list;
                    if (const K lk = s.kind(list); (lk.bits & ~kCopies) == 0 && lk.src >= 0) root = lk.src;
                    const int cls = frameClass(root);
                    const K vk = s.kind(top - 1);
                    if (cls < 0 || !(vk.bits == kEntry || inMemory(vk) || ((vk.bits & ~kCopies) == 0 && vk.src >= 0))) island();
                    value = c.number(top - 1, static_cast<Cls>(cls), cls == CI);
                    stem = cls == CF ? "setf" : "setwi";
                    result = K{static_cast<uint16_t>(cls == CF ? kFloat : kInt)};
                }
                LOp& l = c.op(stem, {value, index}, {value, index});
                l.slot = list;
                c.leaves(l);
                const Reg resultReg{top - 3, value.cls};
                if (!(resultReg == value)) c.op(value.cls == CF ? "movf" : "movi", {resultReg, value}, {value}, {resultReg});
                c.write(top - 3, value.cls == CF ? K{kFloat} : result);
                s.top = top - 2;
                c.fall(s);
                return;
            }
            case Op::Numeric: numeric(c); return;
            case Op::Null:
            case Op::True:
            case Op::False: {  // pushed, in memory
                c.writeMemory(s.top);
                c.op(x.op == Op::Null ? "setnull" : x.op == Op::True ? "settrue" : "setfalse").slot = s.top;
                if (x.op == Op::Null) s.kinds[s.top].fact = 0;  // null itself (joinFacts)
                s.top++;
                c.fall(s);
                return;
            }
            case Op::IterNextRef:
            case Op::IterNext: {  // u16 slot, i32 target: the next item pushed, or to the target
                const int it = c.objectSlot(u16(x, 0));
                const int top = s.top;
                if (x.op == Op::IterNextRef) {
                    // A list of a value type's items: each item where it is, a Ref (M9.5 B1). Any
                    // other list as IterNext, which the VM's IterNextRef is for it.
                    if (const ObjClass* const cls = iteratedClass(it, true)) {
                        const State none = s;
                        c.writeMemory(top);
                        LOp& l = c.op("iterref");
                        l.slot = it;
                        l.slot2 = top;
                        l.ki = static_cast<int64_t>(reinterpret_cast<intptr_t>(cls));
                        c.go(static_cast<uint32_t>(i32(x, 2)), none, l);
                        c.leaves(l);
                        noteValue(s, top, cls);
                        s.top = top + 1;
                        c.fall(s);
                        return;
                    }
                    if (iteratesItems(it)) island();
                }
                if (const ObjClass* const cls = x.op == Op::IterNext ? iteratedClass(it) : nullptr) {
                    // A list of a value type's items kept in the value itself (a Color), as the
                    // frame's iterator goes through as the region is made (M9.5 B1): each item a
                    // copy, in memory.
                    const State none = s;
                    c.writeMemory(top);
                    LOp& l = c.op("itersm");
                    l.slot = it;
                    l.slot2 = static_cast<int64_t>((static_cast<uint64_t>(cls->smallId) << 32) | (static_cast<uint64_t>(top) << 8) |
                                                   cls->layout.load(std::memory_order_acquire)->size);
                    l.ki = static_cast<int64_t>(reinterpret_cast<intptr_t>(cls));
                    c.go(static_cast<uint32_t>(i32(x, 2)), none, l);
                    c.leaves(l);
                    noteValue(s, top, cls);
                    s.top = top + 1;
                    c.fall(s);
                    return;
                }
                int cls = useClass(c.i, top, top + 1);
                if (cls < 0) cls = useClass(c.i, top, top + 1, 0, true);
                const State none = s;  // no more: nothing pushed
                LOp* l;
                if (cls >= 0) {
                    const Reg dst{top, static_cast<Cls>(cls)};
                    l = &c.op(cls == CF ? "iterf" : "iteri", {dst}, {}, {dst});
                    l->slot = it;
                    c.go(static_cast<uint32_t>(i32(x, 2)), none, *l);
                    c.leaves(*l);
                    c.write(top, K{static_cast<uint16_t>(cls == CF ? kFloat : kInt)});
                } else {
                    c.writeMemory(top);
                    l = &c.op("iterv");
                    l->slot = it;
                    l->slot2 = top;
                    c.go(static_cast<uint32_t>(i32(x, 2)), none, *l);
                    c.leaves(*l);
                }
                s.top = top + 1;
                c.fall(s);
                return;
            }
            case Op::RGetProp: {  // u8 A, u8 B, u8 T, u8 S, u16 cache, i32 slow
                const int a = u8(x, 0);
                const int obj = c.objectSlot(u8(x, 1));
                const int64_t cache = cacheAddress(u16(x, 4));
                // A list's or bytes' size (a while loop's `i < items.size`), where the frame holds
                // one as the region is made: read from the object, which the step checks.
                if (sizeRead(obj, i32(x, 6))) {
                    const Reg dst{a, CI};
                    LOp& l = c.op("sizew", {dst}, {}, {dst});
                    c.write(a, K{static_cast<uint16_t>(kInt)});
                    l.slot = obj;
                    c.leaves(l);
                    s.top = u8(x, 2);
                    c.fall(s);
                    return;
                }
                // A field of a value of a value type (M9.5 B1): a Small value or a Ref to an item,
                // of the type the region or the frame says, the field the cache saw.
                if (const ObjClass* const vcls = valueClass(s, obj)) {
                    if (const ValueField* const field = plainField(vcls, u16(x, 4), i32(x, 6))) {
                        const bool isFloat = field->kind == ValueField::Float, isByte = field->kind == ValueField::Narrow;
                        const Reg dst{a, isFloat ? CF : CI};
                        LOp& l = c.op(isFloat ? "valpf" : isByte ? "valpb" : "valpi", {dst}, {}, {dst});
                        l.slot = obj;
                        l.slot2 = static_cast<int64_t>((static_cast<uint64_t>(vcls->smallId) << 32) | field->offset);
                        l.ki = static_cast<int64_t>(reinterpret_cast<intptr_t>(vcls));
                        c.leaves(l);
                        c.write(a, K{static_cast<uint16_t>(isFloat ? kFloat : isByte ? kByte : kInt)});
                        s.top = u8(x, 2);
                        c.fall(s);
                        return;
                    }
                }
                int cls = useClass(c.i, a, u8(x, 2));
                if (cls < 0) {
                    // What the place's cache saw, where the VM has read the field before (the
                    // regions are made as a loop starts the second time): a field kept as its
                    // payload alone says its kind; one kept as a value may hold anything, and is
                    // read as it is. A guess the step checks; and where the cache saw nothing,
                    // how the value is used next.
                    const uint64_t seen = f.caches[u16(x, 4)].load(std::memory_order_relaxed);
                    if ((seen & 0x80) != 0 && (seen & 3) == static_cast<uint64_t>(ValueKind::Float)) cls = CF;
                    else if ((seen & 0x80) != 0 && (seen & 3) == static_cast<uint64_t>(ValueKind::Int)) cls = CI;
                    else if (seen == 0) cls = useClass(c.i, a, u8(x, 2), 0, true);
                }
                LOp* l;
                if (cls >= 0) {
                    const Reg dst{a, static_cast<Cls>(cls)};
                    l = &c.op(cls == CF ? "getpf" : "getpi", {dst}, {}, {dst});
                    c.write(a, K{static_cast<uint16_t>(cls == CF ? kFloat : kInt)});
                } else {
                    c.writeMemory(a);
                    l = &c.op("getpv");
                    l->slot2 = a;
                    noteFact(s, a, [&](Fact& f) { f.from = c.i; });
                    // A list read from a field: what it holds, for the items read from it.
                    const int held = fieldClass(s, obj, u16(x, 4), i32(x, 6));
                    if (held >= 0) {
                        auto [it, added] = fieldLists.emplace(a, held);
                        if (!added && it->second != held) it->second = -2;
                    }
                    if (Value field; fieldValue(s, obj, u16(x, 4), i32(x, 6), field)) {
                        if (field.kind == ValueKind::Obj) noteFieldValue(s, a, field);
                        if (field.isObjType(ObjType::List) && asList(field)->store() == ListStore::Items && asList(field)->pack->itemClass != nullptr)
                            noteItems(s, a, asList(field)->pack->itemClass);
                    }
                }
                l->slot = obj;
                l->ki = cache;
                l->field = slowName(i32(x, 6));
                c.leaves(*l);
                s.top = u8(x, 2);
                c.fall(s);
                return;
            }
            case Op::RSetPropSS:
            case Op::RSetPropSK: {  // u8 A, u8 B, u8 C, u8 T, u8 S, u16 cache, i32 slow: B's field = C, and A
                const int a = u8(x, 0);
                const int obj = c.objectSlot(u8(x, 1));
                const int64_t cache = cacheAddress(u16(x, 5));
                Reg value{};
                K kind;
                if (x.op == Op::RSetPropSK) {
                    const Value& v = constant(u8(x, 2));
                    if (v.isFloat()) {
                        value = Reg{kScratchF, CF};
                        c.op("setfk", {value}, {}, {value}).kf = v.as.d;
                        kind = K{kFloat};
                    } else if (v.isInt()) {
                        value = Reg{kScratchI, CI};
                        c.op("setik", {value}, {}, {value}).ki = v.as.i;
                        kind = constantInt(v.as.i);
                    } else {
                        island();
                    }
                } else {
                    // As the VM's quick way: an int or a float (as entered, checked as the region is);
                    // one the region has not typed (a parameter of an inline body, copied) as the
                    // typed instructions or the frame say, checked.
                    const uint16_t w = static_cast<uint16_t>(s.kind(u8(x, 2)).bits & ~kEntry);
                    if (w == kFloat || w == kInt) {
                        kind = K{w};
                        value = Reg{u8(x, 2), w == kFloat ? CF : CI};
                    } else {
                        const Cls cls = c.guess(u8(x, 2));
                        value = c.read(u8(x, 2), cls, true);
                        kind = K{cls == CF ? kFloat : kInt};
                    }
                }
                LOp& l = c.op(value.cls == CF ? "setpf" : "setpi", {value}, {value});
                l.slot = obj;
                l.ki = cache;
                l.field = slowName(i32(x, 7), true);
                c.leaves(l);
                const Reg result{a, value.cls};
                if (!(result == value)) c.op(value.cls == CF ? "movf" : "movi", {result, value}, {value}, {result});
                c.write(a, kind);
                s.top = u8(x, 3);
                c.fall(s);
                return;
            }
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
            case Op::RBitOrSK:
            case Op::RShlSS:
            case Op::RShrSS: bitwise(c); return;
            default: island();
        }
    }

    // A value whose kind the region does not know (an object, say), moved as memory holds it:
    // copied whole, slot to slot, the copy then held in memory as its source is.
    static void copyInMemory(Ctx& c, int from, int to) {
        if (from == to) return;
        // What it copies, as entered: the slot itself, or the one it was copied from.
        const K fk = c.s.kind(from);
        const int16_t root = fk.bits == kEntry ? static_cast<int16_t>(from) : inMemory(fk) && fk.src >= 0 ? fk.src : static_cast<int16_t>(-1);
        c.writeMemory(to);
        LOp& l = c.op("copyslot");
        l.slot = to;
        l.slot2 = from;
        c.write(to, K{kMem, root});
    }

    // A move of a slot, or its push: what it holds goes with it, and what the region knows of it
    // (a Fact).
    void move(Ctx& c, int from, int to) {
        const Fact* known = factOf(c.s, from);
        const int16_t fact = known != nullptr ? static_cast<int16_t>(known - facts.data()) : static_cast<int16_t>(-1);
        moveSlot(c, from, to);
        if (fact >= 0) c.s.kinds[to].fact = fact;
    }
    void moveSlot(Ctx& c, int from, int to) {
        const K k = c.s.kind(from);
        if (inMemory(k)) {
            // Written in memory: a copy, read there; or, read next as a number, loaded and checked.
            int cls = useClass(c.i, to, to >= c.s.top ? to + 1 : c.s.top);
            if (cls < 0) {
                // Pushed for the SET_INDEX right after to set (`cells.cells[i] = color` in a body
                // put in place, its argument pushed): a copy of the slot, read there, and made in
                // fact only where the region leaves while it stands, as a slot as entered is.
                if (to >= c.s.top && c.i + 1 < static_cast<int>(ins.size()) && ins[static_cast<size_t>(c.i + 1)].op == Op::SetIndex) {
                    c.write(to, K{kCopy, static_cast<int16_t>(from)});
                    return;
                }
                return copyInMemory(c, from, to);
            }
            const Reg src = c.number(from, static_cast<Cls>(cls), cls == CI), dst{to, static_cast<Cls>(cls)};
            if (!(src == dst)) c.op(cls == CF ? "movf" : "movi", {dst, src}, {src}, {dst});
            c.write(to, K{static_cast<uint16_t>(cls == CF ? kFloat : kInt)});
            return;
        }
        if (k.bits == kEntry) {
            // As entered: a copy, read where it is, its payload in a register where it is read
            // next as a number.
            // An int or a float as read next by a typed instruction, or as the typed instructions
            // read and write the slot elsewhere (a guess): checked in memory to be one (an int,
            // not a byte), so that it is one here as it is once the region has written the slot.
            // (Not the slot's hints: where slots are shared, they may be another variable's.)
            const int cls = useClass(c.i, to, to >= c.s.top ? to + 1 : c.s.top);
            const bool sure = cls >= 0;
            if (cls < 0) {
                // Pushed (a list for its item to be set, say): a copy of the slot as entered,
                // read there, and made in fact only where the region leaves while it stands.
                if (to >= c.s.top) {
                    c.write(to, K{kCopy, static_cast<int16_t>(from)});
                    return;
                }
                return copyInMemory(c, from, to);
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

    // Shifts, and & and | of whole numbers.
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
        } else if (x.op == Op::RShlSS || x.op == Op::RShrSS) {
            // A count from 0 to 63 (any other left to the VM), then the shift of b by it: into
            // dst from b, or through the scratch register where dst is the count.
            if (c.guess(u8(x, 2)) != CI) island();
            const Reg r = c.read(u8(x, 2), CI, true);
            const char* stem = x.op == Op::RShlSS ? "shli" : "shri";
            c.leaves(c.op("chkshift", {r}, {r}));
            if (dst == b) {
                c.op(stem, {dst, r}, {dst, r}, {dst});
            } else if (dst != r) {
                c.op("movi", {dst, b}, {b}, {dst});
                c.op(stem, {dst, r}, {dst, r}, {dst});
            } else {
                const Reg scratch{kScratchI, CI};
                c.op("movi", {scratch, b}, {b}, {scratch});
                c.op(stem, {scratch, r}, {scratch, r}, {scratch});
                c.op("movi", {dst, scratch}, {scratch}, {dst});
            }
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
            if (steps > 40 * static_cast<long>(n) + 2000) refuse("the analysis takes too long");
            const int i = work.back();
            work.pop_back();
            done[static_cast<size_t>(i)] = true;
            Result r = transfer(i, in[static_cast<size_t>(i)], false);
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
        maybeNullFacts = &maybeNull;
        entries = {0};
        tops[ins[0].at] = headTop;
        // A round finds the ways in after the islands its states reach, often one more each round
        // (GIF's LZW writer takes 24): up to 32, the analysis's time held as everywhere else.
        constexpr int kRounds = 32;
        for (int round = 0; round < kRounds; round++) {
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
            // Growing only, so that the rounds end: ways in kept, what is known there joined.
            std::set<int> more = entries;
            for (size_t i = 0; i < ins.size(); i++) {
                if (!in[i].known || !results[i].island) continue;
                for (int e : results[i].resumes) {
                    if (e <= 0 || e >= static_cast<int>(ins.size()) || topAt(e) < 0) continue;
                    if (in[static_cast<size_t>(e)].known && results[static_cast<size_t>(e)].island) continue;
                    more.insert(e);
                    join(nextComeBack[e], afterIsland(in[i], static_cast<int>(i), topAt(e)));
                }
            }
            for (const auto& [e, st] : comeBack) join(nextComeBack[e], st);
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
            if (round == kRounds - 1) refuse("what is known where the VM comes back in does not settle");
            entries = std::move(more);
            comeBack = nextComeBack;
        }
        // The VM's work inside an inner loop: that loop's own region, made as it starts, leaves for
        // it with a few registers to load again; this one would load all of its own each time
        // round the inner loop (Path.flatten's whole loop, once it settled, 35 ms to 41).
        for (size_t j = 0; j < ins.size(); j++) {
            if (ins[j].op != Op::Loop || !in[j].known) continue;
            const auto t = index.find(static_cast<uint32_t>(i32(ins[j], 0)));
            if (t == index.end() || t->second <= 0) continue;
            for (size_t k = static_cast<size_t>(t->second); k <= j; k++)
                if (in[k].known && results[k].island && !results[k].ends) refuse("the VM's work inside an inner loop, which has a region of its own");
        }
        // No way in at an island: the VM runs it as it comes to it. (Its ways there within the
        // region, made as to a way in, still write back and load: so it may stay one.)
        for (auto e = entries.begin(); e != entries.end();) {
            if (*e != 0 && (!in[static_cast<size_t>(*e)].known || results[static_cast<size_t>(*e)].island)) e = entries.erase(e);
            else ++e;
        }
        if (results[0].island) refuse("its first instruction is the VM's");
        // The steps, made once, from the states the analysis settled on.
        for (size_t i = 0; i < ins.size(); i++)
            if (in[i].known) results[i] = transfer(static_cast<int>(i), in[i]);
        madeWhereMoved();
        // What each way out writes back.
        for (size_t i = 0; i < ins.size(); i++)
            if (in[i].known)
                for (ExitPoint& e : results[i].exits) {
                    e.stores = stores(e.state, e.at, e.bad);
                    if (!e.bad.empty()) refuse(e.bad);
                }
    }

    // ---- Values made where they are moved to, and none written for nothing ----

    // A value made in memory, then moved down by one POP_UNDER or more (the end of a body put in
    // place: `val c = grid[x, y]` is an item copied out, then moved over the body's receiver and
    // indexes, and over Image's as well for `image[x, y]`; `image[x, y].red` a Ref to it, read
    // through), is made where it ends up: the step
    // that makes it, the last of its instruction, writes the slot the last move takes it to, and
    // the moves (a copyslot each, nothing else) go. Only where each POP_UNDER is come to from the
    // instruction before alone, so that nothing reads the slot in between; the slot moved from
    // is above the stack's top after, which nothing reads.
    static bool madeAt(const LOp& l, int64_t& slot) {
        if (l.stem == "idxsm") slot = (static_cast<uint64_t>(l.slot2) >> 8) & 0xFFFFFF;
        else if (l.stem == "mksm") slot = static_cast<int64_t>(static_cast<uint64_t>(l.slot2) & 0xFFFFFFFF);
        else if (l.stem == "getpv" || l.stem == "iref") slot = l.slot2;
        else return false;
        return true;
    }
    static void makeAt(LOp& l, int64_t slot) {
        const uint64_t was = static_cast<uint64_t>(l.slot2), to = static_cast<uint64_t>(slot);
        if (l.stem == "idxsm") l.slot2 = static_cast<int64_t>((was & ~(0xFFFFFFull << 8)) | to << 8);
        else if (l.stem == "mksm") l.slot2 = static_cast<int64_t>((was & ~0xFFFFFFFFull) | to);
        else l.slot2 = slot;
    }
    void madeWhereMoved() {
        std::vector<int> into(ins.size(), 0);
        for (size_t i = 0; i < ins.size(); i++)
            if (in[i].known)
                for (const Edge& e : results[i].edges)
                    if (e.to >= 0 && static_cast<size_t>(e.to) < ins.size()) into[static_cast<size_t>(e.to)]++;
        for (size_t j = 0; j + 1 < ins.size(); j++) {
            const Result& made = results[j];
            if (!in[j].known || made.island || made.ops.empty() || !made.fallsThrough || made.fallExit >= 0) continue;
            int64_t at;
            if (!madeAt(made.ops.back(), at)) continue;
            std::vector<size_t> moves;
            int64_t to = at;
            for (size_t k = j + 1; k < ins.size(); k++) {
                const Result& r = results[k];
                if (ins[k].op != Op::PopUnder || !in[k].known || r.island || into[k] != 1 || entries.count(static_cast<int>(k)) ||
                    resumed.count(static_cast<int>(k)) || r.ops.size() != 1 || r.ops[0].stem != "copyslot" || r.ops[0].slot2 != to ||
                    !r.fallsThrough || r.fallExit >= 0)
                    break;
                to = r.ops[0].slot;
                moves.push_back(k);
            }
            if (moves.empty()) continue;
            makeAt(results[j].ops.back(), to);
            for (size_t m : moves) results[m].ops.clear();
        }
        // And a value written to a slot (a copy, a null) that the POP right after takes off, come
        // to from there alone: written for nothing (`a[i] = v`'s value, a body's null, as a
        // statement), so not written.
        for (size_t j = 0; j + 1 < ins.size(); j++) {
            Result& wrote = results[j];
            const size_t k = j + 1;
            if (!in[j].known || wrote.island || wrote.ops.empty() || !wrote.fallsThrough || wrote.fallExit >= 0) continue;
            const LOp& last = wrote.ops.back();
            const bool plain = last.stem == "copyslot" || last.stem == "setnull" || last.stem == "settrue" || last.stem == "setfalse";
            if (!plain || last.exit >= 0 || last.jumpTo >= 0 || last.jumpExit >= 0) continue;
            if (ins[k].op != Op::Pop || !in[k].known || results[k].island || into[k] != 1 || entries.count(static_cast<int>(k)) ||
                resumed.count(static_cast<int>(k)) || !results[k].ops.empty() || in[k].top - 1 != last.slot)
                continue;
            wrote.ops.pop_back();
        }
    }

    // ---- Lists and fields looked at once ----

    // In a region where nothing is the VM's, nothing can change where a list's items are or how
    // many there are, and it is entered afresh after each way out: each list read or written by
    // index is looked at once as it starts (its kind, its core, its items), and each read and
    // write then checks the index alone. A list as entered, never written in the region, its
    // slot's own or the one a copy of it is of, or one a field looked at once holds; at most
    // kRegionLists of them.
    std::vector<LOp> prologue;
    // Looked at where the VM's only work is the way to a return or a throw, which leaves the
    // region for good: no loop made ahead then.
    bool lookedBeforeEnds = false;

    // Whether slot `root` is as entered everywhere in the region: never written in it.
    bool untouched(int root) const {
        for (size_t i = 0; i < ins.size(); i++)
            if (in[i].known && !(in[i].kind(root).bits == kEntry && (!results[i].fallsThrough || results[i].after.kind(root).bits == kEntry)))
                return false;
        return true;
    }

    // In such a region, too, a field that no step writes (by its name, of any object: SET_PROP's
    // setpi and setpf, the only steps that write a field) of an object the region never changes
    // (a slot as entered, never written in it, or what another such field holds: `image.cells`,
    // then its `cells`) is the same each time round. Each is read once as the region starts
    // (lookp, lookph: as getpv reads it, into the context's `held`), and each read of it then
    // takes it from there with nothing checked but its kind (hgetpi, hgetpf, hgetpv). Another
    // thread changes nothing meanwhile: one waiting stops the loop at its end, and the region is
    // entered afresh.
    //
    // A field the region writes, where every step that reads or writes a field of that name does
    // so of the one object (2026-11-12: an image's changed rectangle, which `image[x, y] = c`
    // widens for each pixel), is held the same way, looked at as one that may be written as SET_PROP
    // writes it (lookpw, lookpwh: its object its core's own, the field a number of no other kind
    // or set once); each read and write of it is of what is held (hsetpi, hsetpf), and each way
    // out writes it back (putp) before the VM goes on. Nothing reads it meanwhile: no other step
    // reads a field of its name, the VM does nothing in the region, and another thread looks only
    // once the loop has stopped. So a loop that sets many pixels writes the rectangle once.
    //
    // At most kRegionFields of them. `heldBy`: each getpv's instruction looked at so, and where
    // its field is held.
    void lookAtFieldsOnce(std::map<int, int>& heldBy) {
        std::vector<const ObjString*> written;
        for (size_t i = 0; i < ins.size(); i++) {
            if (!in[i].known) continue;
            for (const LOp& l : results[i].ops)
                if (l.stem == "setpi" || l.stem == "setpf") {
                    if (l.field == nullptr) return;  // a field not known by its name: none looked at
                    written.push_back(l.field);
                }
        }
        auto writes = [&](const ObjString* name) {
            for (const ObjString* w : written)
                if (w == name || w->utf8() == name->utf8()) return true;
            return false;
        };
        // (source, field) -> where it is held; a source >= 0 a slot, else -2 - k for held k.
        std::map<std::pair<int, std::string>, int> held;
        std::map<int, bool> asEntered;
        // A field written: each step of its name, and the object each is of (-1: not known).
        std::map<std::string, std::vector<std::pair<LOp*, int>>> changed;
        auto sourceOf = [&](size_t i, const LOp& l) -> int {
            const int slot = static_cast<int>(l.slot);
            const K k = in[i].kind(slot);
            int source = -1;
            if (k.bits == kEntry) {
                source = slot;
            } else if ((inMemory(k) || (k.bits & ~kCopies) == 0) && k.src >= 0) {
                source = k.src;
            } else if (const Fact* known = factOf(in[i], slot); known != nullptr && known->from >= 0) {
                if (auto h = heldBy.find(known->from); h != heldBy.end()) source = -2 - h->second;
            }
            if (source >= 0) {
                auto e = asEntered.find(source);
                if (e == asEntered.end()) e = asEntered.emplace(source, untouched(source)).first;
                if (!e->second) source = -1;
            }
            return source;
        };
        auto holding = [&](int source, const LOp& l, bool changes) -> int {
            const auto key = std::make_pair(source, l.field->utf8());
            auto found = held.find(key);
            if (found != held.end()) return found->second;
            if (static_cast<int>(held.size()) == kRegionFields) return -1;
            found = held.emplace(key, static_cast<int>(held.size())).first;
            LOp look;
            look.stem = std::string(changes ? "lookpw" : "lookp") + (source >= 0 ? "" : "h");
            look.slot = source >= 0 ? source : -2 - source;
            look.ki = l.ki;  // the place's cache: where the field is in an object of its layout
            look.slot2 = found->second;
            prologue.push_back(look);
            return found->second;
        };
        for (size_t i = 0; i < ins.size(); i++) {
            if (!in[i].known) continue;
            for (LOp& l : results[i].ops) {
                const bool reads = l.stem == "getpi" || l.stem == "getpf" || l.stem == "getpv";
                if (!reads && l.stem != "setpi" && l.stem != "setpf") continue;
                if (l.field == nullptr) continue;
                if (writes(l.field)) {
                    changed[l.field->utf8()].emplace_back(&l, sourceOf(i, l));
                    continue;
                }
                const int source = sourceOf(i, l);
                if (source == -1) continue;
                const int k = holding(source, l, false);
                if (k < 0) continue;
                if (l.stem == "getpv") heldBy[static_cast<int>(i)] = k;
                l.stem = "h" + l.stem;
                l.slot = k;
            }
        }
        std::vector<int> putBack;
        for (auto& [name, steps] : changed) {
            const int source = steps[0].second;
            bool one = source != -1;
            for (const auto& [l, s] : steps) one = one && s == source;
            if (!one) continue;
            const int k = holding(source, *steps[0].first, true);
            if (k < 0) continue;
            for (auto& [l, s] : steps) {
                l->stem = "h" + l->stem;
                l->slot = k;
            }
            putBack.push_back(k);
        }
        if (putBack.empty()) return;
        for (size_t i = 0; i < ins.size(); i++) {
            if (!in[i].known) continue;
            for (ExitPoint& e : results[i].exits)
                for (int k : putBack) {
                    LOp put;
                    put.stem = "putp";
                    put.slot = k;
                    e.stores.push_back(put);
                }
        }
    }

    void lookAtListsOnce() {
        for (size_t i = 0; i < ins.size(); i++) {
            if (!in[i].known || !results[i].island) continue;
            if (!results[i].ends) return;
            lookedBeforeEnds = true;
        }
        if (entries.size() != 1) return;
        std::map<int, int> heldBy;
        lookAtFieldsOnce(heldBy);
        // The slot each list step reads, as entered; or -2 - k, where a field held as k holds it.
        auto rootOf = [&](size_t i, int slot) -> int {
            const K k = in[i].kind(slot);
            if (k.bits == kEntry) return slot;
            if ((inMemory(k) || (k.bits & ~kCopies) == 0) && k.src >= 0) return k.src;
            if (const Fact* known = factOf(in[i], slot); known != nullptr && known->from >= 0)
                if (auto h = heldBy.find(known->from); h != heldBy.end()) return -2 - h->second;
            return -1;
        };
        auto isRead = [](const std::string& stem) { return stem == "idxf" || stem == "idxfk" || stem == "idxw" || stem == "idxwk"; };
        auto isWrite = [](const std::string& stem) { return stem == "setf" || stem == "setwi" || stem == "setwb"; };
        auto isFloat = [](const std::string& stem) { return stem == "idxf" || stem == "idxfk" || stem == "setf"; };
        // A list of a value type's items (M9.5 B1): its items' fields read, or a Ref to one made,
        // and the fields of the values those Refs are in read (valp); an item copied out, set to
        // a value, or a Ref to one made to be changed (B2 for op []=).
        auto isPlain = [](const std::string& stem) { return stem == "idxsm" || stem == "setsm" || stem == "placei"; };
        auto isItems = [&](const std::string& stem) { return stem.rfind("idxp", 0) == 0 || stem == "iref" || stem == "iterref" || isPlain(stem); };
        // root -> 1 floats, 2 whole numbers, 4 items, 8 an iterator through items; any two not looked at
        std::map<int, int> kindOf;
        std::map<int, const ObjClass*> itemClass;  // root -> its items' class (null for two)
        std::set<int> plain;                       // roots whose items are copied out or set: none of a type of its own
        std::map<int, int> valueRoot;              // a slot a Ref is made in -> the list's root (-1 for two)
        std::vector<LOp*> values;
        std::vector<std::pair<LOp*, int>> steps;
        for (size_t i = 0; i < ins.size(); i++) {
            if (!in[i].known) continue;
            for (LOp& l : results[i].ops) {
                if (isItems(l.stem)) {
                    const int root = rootOf(i, static_cast<int>(l.slot));
                    if (root == -1) continue;
                    kindOf[root] |= l.stem == "iterref" ? 8 : 4;
                    const auto* const cls = reinterpret_cast<const ObjClass*>(static_cast<intptr_t>(l.ki));
                    if (auto [it, added] = itemClass.emplace(root, cls); !added && it->second != cls) it->second = nullptr;
                    if (isPlain(l.stem)) plain.insert(root);
                    if (l.stem == "iterref") {
                        if (auto [it, added] = valueRoot.emplace(static_cast<int>(l.slot2), root); !added && it->second != root) it->second = -1;
                        continue;  // itself as it is: the iterator moves on
                    }
                    if (l.stem == "iref")
                        if (auto [it, added] = valueRoot.emplace(static_cast<int>(l.slot2), root); !added && it->second != root) it->second = -1;
                    steps.emplace_back(&l, root);
                    continue;
                }
                if (l.stem.rfind("valp", 0) == 0) {
                    values.push_back(&l);
                    continue;
                }
                if (!isRead(l.stem) && !isWrite(l.stem)) continue;
                const int root = rootOf(i, static_cast<int>(l.slot));
                if (root == -1) continue;
                kindOf[root] |= isFloat(l.stem) ? 1 : 2;
                steps.emplace_back(&l, root);
            }
        }
        // Never written in the region: as entered everywhere (a field's, as it is held).
        std::map<int, int> index;
        for (const auto& [root, kind] : kindOf) {
            if ((kind != 1 && kind != 2 && kind != 4 && kind != 8) || static_cast<int>(index.size()) == kRegionLists) continue;
            if (kind == 8 && root < 0) continue;
            const ObjClass* const cls = kind >= 4 ? itemClass[root] : nullptr;
            const ValueLayout* const layout = cls != nullptr ? cls->layout.load(std::memory_order_acquire) : nullptr;
            if (kind >= 4 && (layout == nullptr || layout->size == 0 || layout->size > 0xFFFF)) continue;
            if (root >= 0 && !untouched(root)) continue;
            const int k = static_cast<int>(index.size());
            index[root] = k;
            LOp look;
            look.stem = kind == 1 ? "lookf" : kind == 2 ? "lookw" : kind == 4 ? "looki" : "lookit";
            if (root < 0) look.stem += "h";  // from a field held as -2 - root
            look.slot = root >= 0 ? root : -2 - root;
            look.ki = k;
            if (kind >= 4) {
                look.ki = static_cast<int64_t>(reinterpret_cast<intptr_t>(cls));
                look.slot2 = static_cast<int64_t>(static_cast<uint64_t>(layout->size) << 32 | (plain.count(root) ? 0x100u : 0u) | static_cast<uint64_t>(k));
            }
            prologue.push_back(look);
        }
        // The width of a root's items, where it is looked at as a list of items.
        auto widthOf = [&](int root) -> uint64_t { return itemClass[root]->layout.load(std::memory_order_acquire)->size; };
        for (auto& [l, root] : steps) {
            auto k = index.find(root);
            if (k == index.end()) continue;
            if (l->stem.rfind("idxp", 0) == 0) l->slot2 = static_cast<int64_t>(static_cast<uint64_t>(l->slot2) | widthOf(root) << 32);
            if (isPlain(l->stem)) l->ki = static_cast<int64_t>(widthOf(root));  // the class checked as it was looked at
            l->stem = "h" + l->stem;
            l->slot = k->second;
        }
        // A field of a Ref made from a list looked at once: read through what was looked at, where
        // the field is within the first 64 KB of its item.
        for (LOp* l : values) {
            auto v = valueRoot.find(static_cast<int>(l->slot));
            if (v == valueRoot.end() || v->second == -1) continue;
            auto k = index.find(v->second);
            if (k == index.end() || reinterpret_cast<const ObjClass*>(static_cast<intptr_t>(l->ki)) != itemClass[v->second]) continue;
            const uint64_t what = static_cast<uint64_t>(l->slot2), offset = what & 0xFFFFFFFF;
            if (offset > 0xFFFF) continue;
            l->stem = "h" + l->stem;
            l->ki = k->second;
            l->slot2 = static_cast<int64_t>((what & 0xFFFFFFFF00000000ull) | widthOf(v->second) << 16 | offset);
        }
    }

    // ---- Whole loops made ahead (loop templates) ----

    // A loop's body followed through its steps, each register's value an expression of what the
    // loop starts with: the counter, a register it does not change, a constant, an item of a list
    // looked at once, and the arithmetic of them. Where a loop stores one such expression a time
    // round to a list, or adds one to a total, in one of the shapes stencils.cpp's runLoop knows,
    // the whole loop is run there, before its own steps (which then find nothing left, or do the
    // item it stopped at).
    struct Expr {
        enum Kind { Counter, Inv, KInt, KFloat, Load, Bin, Min, Max, ToByte } kind;
        Cls cls = CI;
        Reg reg{0, CI};  // Inv
        int64_t ki = 0;  // KInt
        double kf = 0;   // KFloat
        int list = -1;   // Load: which list looked at once
        char op = 0;     // Bin: + - * /
        const Expr* a = nullptr;
        const Expr* b = nullptr;  // Load: the index
    };
    std::deque<Expr> exprs;
    const Expr* mk(Expr e) {
        exprs.push_back(e);
        return &exprs.back();
    }
    static bool same(const Expr* x, const Expr* y) {
        if (x == y) return true;
        if (x == nullptr || y == nullptr || x->kind != y->kind || x->cls != y->cls) return false;
        switch (x->kind) {
            case Expr::Counter: return true;
            case Expr::Inv: return x->reg == y->reg;
            case Expr::KInt: return x->ki == y->ki;
            case Expr::KFloat: return std::memcmp(&x->kf, &y->kf, 8) == 0;
            case Expr::Load: return x->list == y->list && same(x->b, y->b);
            default: return x->op == y->op && same(x->a, y->a) && same(x->b, y->b);
        }
    }
    // Whether an expression is the same each time round: no counter, no item in it.
    static bool invariant(const Expr* e) {
        if (e == nullptr) return false;
        if (e->kind == Expr::Counter || e->kind == Expr::Load) return false;
        return (e->a == nullptr || invariant(e->a)) && (e->b == nullptr || invariant(e->b));
    }
    static bool usesReg(const Expr* e, const Reg& r) {
        if (e == nullptr) return false;
        if (e->kind == Expr::Inv && e->reg == r) return true;
        return usesReg(e->a, r) || usesReg(e->b, r);
    }
    static bool itemAtCounter(const Expr* e, Cls cls) { return e != nullptr && e->kind == Expr::Load && e->cls == cls && e->b != nullptr && e->b->kind == Expr::Counter; }

    // The body's steps, followed: each register's value; the stores (list, index, value, the
    // step's stem); false where a step is one this does not follow.
    struct Followed {
        std::map<Reg, const Expr*> val;
        struct Store {
            int list;
            const Expr* index;
            const Expr* value;
            std::string stem;
        };
        std::vector<Store> stores;
    };
    const Expr* valueOf(Followed& fw, const Reg& r) {
        auto v = fw.val.find(r);
        if (v != fw.val.end()) return v->second;
        Expr e{Expr::Inv};
        e.cls = r.cls;
        e.reg = r;
        return mk(e);
    }
    bool follow(Followed& fw, const LOp& l) {
        const std::string& s = l.stem;
        auto set = [&](const Reg& d, const Expr* e) { fw.val[d] = e; };
        auto bin = [&](char op, Cls cls, const Expr* x, const Expr* y) {
            Expr e{Expr::Bin};
            e.cls = cls, e.op = op, e.a = x, e.b = y;
            return mk(e);
        };
        auto kint = [&](int64_t v) {
            Expr e{Expr::KInt};
            e.ki = v;
            return mk(e);
        };
        auto kfloat = [&](double v) {
            Expr e{Expr::KFloat};
            e.cls = CF, e.kf = v;
            return mk(e);
        };
        const auto& n = l.named;
        auto arith = [&](const std::string& stem, char& op) {
            if (stem.rfind("add", 0) == 0) op = '+';
            else if (stem.rfind("sub", 0) == 0) op = '-';
            else if (stem.rfind("mul", 0) == 0) op = '*';
            else if (stem.rfind("div", 0) == 0) op = '/';
            else return false;
            return true;
        };
        char op = 0;
        if (s == "setik") return set(n[0], kint(l.ki)), true;
        if (s == "setfk") return set(n[0], kfloat(l.kf)), true;
        if (s == "movi" || s == "movf") return set(n[0], valueOf(fw, n[1])), true;
        if (s == "inci") return set(n[0], bin('+', CI, valueOf(fw, n[0]), kint(1))), true;
        if ((s == "addi" || s == "subi" || s == "muli" || s == "addf" || s == "subf" || s == "mulf" || s == "divf") && arith(s, op))
            return set(n[0], bin(op, s.back() == 'f' ? CF : CI, valueOf(fw, n[1]), valueOf(fw, n[2]))), true;
        if ((s == "addik" || s == "subik" || s == "mulik") && arith(s, op)) return set(n[0], bin(op, CI, valueOf(fw, n[1]), kint(l.ki))), true;
        if ((s == "addikr" || s == "subikr" || s == "mulikr") && arith(s, op)) return set(n[0], bin(op, CI, kint(l.ki), valueOf(fw, n[1]))), true;
        if ((s == "addfk" || s == "subfk" || s == "mulfk" || s == "divfk") && arith(s, op)) return set(n[0], bin(op, CF, valueOf(fw, n[1]), kfloat(l.kf))), true;
        if ((s == "addfkr" || s == "subfkr" || s == "mulfkr" || s == "divfkr") && arith(s, op)) return set(n[0], bin(op, CF, kfloat(l.kf), valueOf(fw, n[1]))), true;
        if (s == "mini" || s == "maxi") {
            Expr e{s == "mini" ? Expr::Min : Expr::Max};
            e.a = valueOf(fw, n[0]), e.b = valueOf(fw, n[1]);
            return set(n[0], mk(e)), true;
        }
        if (s == "chkbyte") {
            Expr e{Expr::ToByte};
            e.a = valueOf(fw, n[0]);
            return set(n[0], mk(e)), true;
        }
        if (s == "hidxf" || s == "hidxw" || s == "hidxfk" || s == "hidxwk") {
            Expr e{Expr::Load};
            e.cls = s[4] == 'f' ? CF : CI;
            e.list = static_cast<int>(l.slot);
            e.b = s.back() == 'k' ? kint(l.ki) : valueOf(fw, n[1]);
            return set(n[0], mk(e)), true;
        }
        if (s == "hsetf" || s == "hsetwi" || s == "hsetwb") {
            fw.stores.push_back(Followed::Store{static_cast<int>(l.slot), valueOf(fw, n[1]), valueOf(fw, n[0]), s});
            return true;
        }
        return false;
    }

    // The template, its words and where it goes, for the loop from head h to its Loop e.
    struct Ahead {
        uint64_t shape = 0;
        int out = -1, a = -1, b = -1;
        const Expr* p1 = nullptr;  // parameters, worked out before the loop
        const Expr* p2 = nullptr;
        int64_t words[6] = {0, 0, 0, 0, 0, 0};  // 7 to 12, where they are constants
        bool bytesOut = false;
        Reg acc{0, CI};
        bool hasAcc = false;
        const Expr* addend = nullptr;
        int checkSlot = -1, checkKind = 0;  // a variable's kind the body checks each time round  // kLoopWMap: c where it is a register the loop does not change
    };

    // The loop from head h (its RangeNext, the step 1) to its Loop e: a template where it fits.
    bool recognise(int h, int e, Ahead& ah, Reg& next, Reg& left) {
        const Result& head = results[static_cast<size_t>(h)];
        if (head.ops.size() != 4 || head.ops[0].stem != "jngtik" || head.ops[2].stem != "movi" || head.ops[3].stem != "addwik" || head.ops[3].ki != 1)
            return false;
        left = head.ops[0].named[0];
        const Reg counter = head.ops[2].named[0];
        next = head.ops[2].named[1];
        Followed fw;
        {
            Expr c{Expr::Counter};
            fw.val[counter] = mk(c);
        }
        std::set<Reg> defined;
        for (int i = h + 1; i < e;) {
            const Result& r = results[static_cast<size_t>(i)];
            if (!in[static_cast<size_t>(i)].known || r.island || r.fallExit >= 0) return false;
            // An int chosen by a comparison with a constant: x > K ? K : x and the like.
            const LOp* last = r.ops.empty() ? nullptr : &r.ops.back();
            if (last != nullptr && last->stem.size() == 6 && last->stem.rfind("jn", 0) == 0 && last->stem.substr(4) == "ik" && last->jumpTo > i && last->jumpTo < e) {
                for (size_t k = 0; k + 1 < r.ops.size(); k++)
                    if (!follow(fw, r.ops[k])) return false;
                for (const LOp& l : r.ops)
                    for (const Reg& d : l.defs) defined.insert(d);
                const int x = last->jumpTo;
                // The way it falls: to a jump past the other way.
                int t = i + 1, y = -1;
                Followed then = fw, other = fw;
                std::set<Reg> thenDefs, otherDefs;
                for (; t < x; t++) {
                    const Result& b = results[static_cast<size_t>(t)];
                    if (!in[static_cast<size_t>(t)].known || b.island) return false;
                    for (const LOp& l : b.ops) {
                        if (l.stem == "jump") {
                            if (t != x - 1 || l.jumpTo <= x || l.jumpTo >= e) return false;
                            y = l.jumpTo;
                            continue;
                        }
                        if (!follow(then, l)) return false;
                        for (const Reg& d : l.defs) thenDefs.insert(d);
                    }
                }
                if (y < 0) return false;
                for (int u = x; u < y; u++) {
                    const Result& b = results[static_cast<size_t>(u)];
                    if (!in[static_cast<size_t>(u)].known || b.island) return false;
                    for (const LOp& l : b.ops) {
                        if (l.jumpTo >= 0 || l.jumpExit >= 0) return false;
                        if (!follow(other, l)) return false;
                        for (const Reg& d : l.defs) otherDefs.insert(d);
                    }
                }
                if (thenDefs.size() != 1 || thenDefs != otherDefs || then.stores.size() != fw.stores.size() || other.stores.size() != fw.stores.size()) return false;
                const Reg d = *thenDefs.begin();
                const Expr* subject = valueOf(fw, last->named[0]);
                const Expr* vThen = then.val[d];
                const Expr* vOther = other.val[d];
                const std::string cmp = last->stem.substr(2, 2);
                // Falls where `subject cmp K` holds: then K, otherwise the subject.
                if (vThen->kind != Expr::KInt || vThen->ki != last->ki || !same(vOther, subject)) return false;
                Expr pick{cmp == "gt" || cmp == "ge" ? Expr::Min : cmp == "lt" || cmp == "le" ? Expr::Max : Expr::Bin};
                if (pick.kind == Expr::Bin) return false;
                pick.a = subject;
                pick.b = vThen;
                fw.val[d] = mk(pick);
                defined.insert(d);
                i = y;
                continue;
            }
            for (const LOp& l : r.ops) {
                if (l.jumpTo >= 0 || l.jumpExit >= 0) return false;
                if (l.stem == "chkf" || l.stem == "chkint" || l.stem == "chki") {
                    // Of a variable the loop never writes: checked once, by the template.
                    const int slot = static_cast<int>(l.slot);
                    for (int u = h; u <= e; u++)
                        if (in[static_cast<size_t>(u)].kind(slot).bits != kEntry) return false;
                    const int kind = l.stem == "chkf" ? 1 : l.stem == "chkint" ? 2 : 3;
                    if (ah.checkSlot >= 0 && (ah.checkSlot != slot || ah.checkKind != kind)) return false;
                    ah.checkSlot = slot, ah.checkKind = kind;
                    continue;
                }
                if (!follow(fw, l)) return false;
                for (const Reg& d : l.defs) defined.insert(d);
            }
            i++;
        }
        if (defined.count(next) || defined.count(left) || defined.count(counter)) return false;
        // What goes on round the loop: at most one total.
        const std::set<Reg>& live = liveIn[static_cast<size_t>(h)];
        for (const Reg& d : defined) {
            if (d == counter || !live.count(d)) continue;
            if (ah.hasAcc) return false;
            ah.acc = d;
            ah.hasAcc = true;
        }
        // A value of an earlier time round read: the total's alone, by itself.
        for (const auto& [r, v] : fw.val)
            for (const Reg& d : defined)
                if (!(ah.hasAcc && d == ah.acc) && usesReg(v, d)) return false;
        for (const auto& st : fw.stores)
            for (const Reg& d : defined)
                if (usesReg(st.value, d) || usesReg(st.index, d)) return false;
        if (ah.hasAcc) {
            if (!fw.stores.empty()) return false;
            const Expr* v = fw.val[ah.acc];
            if (v->kind != Expr::Bin || v->op != '+' || v->a->kind != Expr::Inv || !(v->a->reg == ah.acc) || !itemAtCounter(v->b, ah.acc.cls)) return false;
            ah.shape = ah.acc.cls == CF ? 8 /* kLoopFSum */ : 11 /* kLoopWSum */;
            ah.a = v->b->list;
            return true;
        }
        if (fw.stores.size() != 1) return false;
        const Followed::Store& st = fw.stores[0];
        if (st.index->kind != Expr::Counter) return false;
        ah.out = st.list;
        const Expr* v = st.value;
        if (st.stem == "hsetf") {
            auto bin = [&](const Expr* e, char op) { return e->kind == Expr::Bin && e->cls == CF && e->op == op; };
            if (itemAtCounter(v, CF)) {
                ah.shape = 1, ah.a = v->list;
            } else if (invariant(v)) {
                ah.shape = 2, ah.p1 = v;
            } else if (v->kind == Expr::Bin && itemAtCounter(v->a, CF) && invariant(v->b)) {
                ah.shape = 3, ah.a = v->a->list, ah.p1 = v->b, ah.words[2] = v->op;
            } else if (v->kind == Expr::Bin && invariant(v->a) && itemAtCounter(v->b, CF)) {
                ah.shape = 4, ah.a = v->b->list, ah.p1 = v->a, ah.words[2] = v->op;
            } else if (v->kind == Expr::Bin && itemAtCounter(v->a, CF) && itemAtCounter(v->b, CF)) {
                ah.shape = 5, ah.a = v->a->list, ah.b = v->b->list, ah.words[2] = v->op;
            } else if (bin(v, '+') && bin(v->a, '*') && itemAtCounter(v->a->a, CF) && invariant(v->a->b) && invariant(v->b)) {
                ah.shape = 6, ah.a = v->a->a->list, ah.p1 = v->a->b, ah.p2 = v->b;
            } else if (bin(v, '+') && bin(v->a, '*') && bin(v->b, '*') && itemAtCounter(v->a->a, CF) && invariant(v->a->b) && itemAtCounter(v->b->a, CF) &&
                       invariant(v->b->b)) {
                ah.shape = 7, ah.a = v->a->a->list, ah.b = v->b->a->list, ah.p1 = v->a->b, ah.p2 = v->b->b;
            } else {
                return false;
            }
            return true;
        }
        // Whole numbers: into bytes (a byte's value, setwb) or a list of ints or bytes (setwi).
        if (st.stem == "hsetwb") {
            if (v->kind != Expr::ToByte) return false;
            ah.bytesOut = true;
            v = v->a;
        }
        if (itemAtCounter(v, CI)) {
            ah.shape = 9, ah.a = v->list;
            return true;
        }
        if (v->kind == Expr::KInt) {
            ah.shape = 10, ah.words[0] = v->ki;
            return true;
        }
        if (v->kind == Expr::Load && v->cls == CI && itemAtCounter(v->b, CI)) {
            ah.shape = 13, ah.a = v->b->list, ah.b = v->list;
            return true;
        }
        // sign * item + c, then up to two of min and max with constants.
        std::vector<std::pair<int, int64_t>> clamps;
        while ((v->kind == Expr::Min || v->kind == Expr::Max) && v->b->kind == Expr::KInt && clamps.size() < 2) {
            clamps.insert(clamps.begin(), {v->kind == Expr::Min ? 1 : 2, v->b->ki});
            v = v->a;
        }
        int64_t sign = 1, c = 0;
        auto addend = [](const Expr* e) { return e->kind == Expr::Inv && e->cls == CI; };
        if (itemAtCounter(v, CI)) {
        } else if (v->kind == Expr::Bin && v->op == '+' && itemAtCounter(v->a, CI) && addend(v->b)) {
            ah.addend = v->b, v = v->a;
        } else if (v->kind == Expr::Bin && v->op == '+' && addend(v->a) && itemAtCounter(v->b, CI)) {
            ah.addend = v->a, v = v->b;
        } else if (v->kind == Expr::Bin && v->op == '-' && addend(v->a) && itemAtCounter(v->b, CI)) {
            sign = -1, ah.addend = v->a, v = v->b;
        } else if (v->kind == Expr::Bin && v->op == '+' && itemAtCounter(v->a, CI) && v->b->kind == Expr::KInt) {
            c = v->b->ki, v = v->a;
        } else if (v->kind == Expr::Bin && v->op == '+' && v->a->kind == Expr::KInt && itemAtCounter(v->b, CI)) {
            c = v->a->ki, v = v->b;
        } else if (v->kind == Expr::Bin && v->op == '-' && itemAtCounter(v->a, CI) && v->b->kind == Expr::KInt && v->b->ki != INT64_MIN) {
            c = -v->b->ki, v = v->a;
        } else if (v->kind == Expr::Bin && v->op == '-' && v->a->kind == Expr::KInt && itemAtCounter(v->b, CI)) {
            sign = -1, c = v->a->ki, v = v->b;
        } else {
            return false;
        }
        ah.shape = 12, ah.a = v->list;
        ah.words[0] = sign, ah.words[1] = c;
        for (size_t k = 0; k < clamps.size(); k++) ah.words[2 + 2 * k] = clamps[k].first, ah.words[3 + 2 * k] = clamps[k].second;
        return true;
    }

    // An invariant float worked out before the loop: a constant, or a register (its steps added
    // to `ops`, scratch registers below -200).
    bool workOut(const Expr* e, std::vector<LOp>& ops, bool& isConst, double& k, Reg& reg) {
        if (e->kind == Expr::KFloat) return isConst = true, k = e->kf, true;
        if (e->kind == Expr::Inv && e->cls == CF) return isConst = false, reg = e->reg, true;
        if (e->kind != Expr::Bin || e->cls != CF) return false;
        bool ca, cb;
        double ka = 0, kb = 0;
        Reg ra{0, CF}, rb{0, CF};
        if (!workOut(e->a, ops, ca, ka, ra) || !workOut(e->b, ops, cb, kb, rb)) return false;
        const char* stem = e->op == '+' ? "add" : e->op == '-' ? "sub" : e->op == '*' ? "mul" : "div";
        if (ca && cb) {
            k = e->op == '+' ? ka + kb : e->op == '-' ? ka - kb : e->op == '*' ? ka * kb : ka / kb;
            return isConst = true, true;
        }
        const Reg d{-200 - static_cast<int>(exprs.size()), CF};
        LOp l;
        l.defs = {d};
        if (!ca && !cb) {
            l.stem = std::string(stem) + "f", l.named = {d, ra, rb}, l.uses = {ra, rb};
        } else if (!ca) {
            l.stem = std::string(stem) + "fk", l.named = {d, ra}, l.uses = {ra}, l.kf = kb;
        } else {
            l.stem = std::string(stem) + "fkr", l.named = {d, rb}, l.uses = {rb}, l.kf = ka;
        }
        ops.push_back(l);
        return isConst = false, reg = d, true;
    }

    // The steps that run a recognised loop's template, before it: its words set, the template,
    // the counter and the total read back.
    bool templateSteps(const Ahead& ah, const Reg& next, const Reg& left, std::vector<LOp>& ops) {
        auto word = [&](int at, int64_t v) {
            LOp l;
            l.stem = "ctxk", l.slot = at, l.ki = v;
            ops.push_back(l);
        };
        auto store = [&](int at, const Reg& r) {
            LOp l;
            l.stem = r.cls == CF ? "ctxsf" : "ctxsi", l.named = {r}, l.uses = {r}, l.slot = at;
            ops.push_back(l);
        };
        auto load = [&](int at, const Reg& r) {
            LOp l;
            l.stem = r.cls == CF ? "ctxlf" : "ctxli", l.named = {r}, l.defs = {r}, l.slot = at;
            ops.push_back(l);
        };
        auto param = [&](int at, const Expr* e) {
            bool isConst;
            double k;
            Reg r{0, CF};
            if (!workOut(e, ops, isConst, k, r)) return false;
            if (isConst) {
                int64_t bits;
                std::memcpy(&bits, &k, 8);
                word(at, bits);
            } else {
                store(at, r);
            }
            return true;
        };
        word(0, static_cast<int64_t>(ah.shape));
        word(4, ah.out), word(5, ah.a), word(6, ah.b);
        for (int k = 0; k < 6; k++) word(7 + k, ah.words[k]);
        word(13, ah.bytesOut ? 1 : 0);
        word(14, ah.checkSlot), word(15, ah.checkKind);
        if (ah.p1 != nullptr && !param(7, ah.p1)) return false;
        if (ah.p2 != nullptr && !param(8, ah.p2)) return false;
        if (ah.addend != nullptr) store(8, ah.addend->reg);
        store(1, next), store(2, left);
        if (ah.hasAcc) store(3, ah.acc);
        LOp run;
        run.stem = "tloop";
        ops.push_back(run);
        load(1, next), load(2, left);
        if (ah.hasAcc) load(3, ah.acc);
        return true;
    }

    // Each loop of the region (a for over a range, step 1, nothing of the VM's in the region)
    // whose body fits a template: its template's steps put where the loop is entered, before its
    // first instruction (headOps, for the region's own loop; the end of the instruction before,
    // for a loop inside it, entered by falling into it alone).
    std::vector<LOp> headOps;
    int templates = 0;

    void makeAhead() {
        if (prologue.empty() || lookedBeforeEnds) return;  // lists looked at once: none, or the VM's in the region
        for (size_t e = 0; e < ins.size(); e++) {
            if (!in[e].known || ins[e].op != Op::Loop) continue;
            const LOp& back = results[e].ops.back();
            const int h = back.jumpTo;
            if (h < 0 || static_cast<size_t>(h) >= e || ins[static_cast<size_t>(h)].op != Op::RangeNext) continue;
            // Entered by falling in from the instruction before alone (or the region's start).
            bool alone = true;
            for (size_t i = 0; i < ins.size() && alone; i++) {
                if (!in[i].known || i == e) continue;
                for (const LOp& l : results[i].ops)
                    if (l.jumpTo == h) alone = false;
            }
            if (!alone) continue;
            if (h > 0) {
                const Result& pre = results[static_cast<size_t>(h) - 1];
                if (!in[static_cast<size_t>(h) - 1].known || pre.island || !pre.fallsThrough || pre.fallExit >= 0) continue;
            }
            Ahead ah;
            Reg next{0, CI}, left{0, CI};
            if (!recognise(h, static_cast<int>(e), ah, next, left)) continue;
            std::vector<LOp> ops;
            if (!templateSteps(ah, next, left, ops)) continue;
            if (h == 0) headOps = ops;
            else results[static_cast<size_t>(h) - 1].ops.insert(results[static_cast<size_t>(h) - 1].ops.end(), ops.begin(), ops.end());
            templates++;
        }
    }

    // ---- Liveness: which registers each instruction reads before writing ----

    std::vector<std::set<Reg>> liveIn;

    // What a way out reads: the registers it writes back. (A way to an instruction the VM comes
    // back in at loads what that reads after.)
    static void addUses(std::set<Reg>& live, const ExitPoint& e) {
        for (const LOp& l : e.stores) live.insert(l.uses.begin(), l.uses.end());
    }

    // Each value written to a slot and read only on the straight way after, before anything
    // else comes in, leaves or writes the slot again (the stack's slots, mostly, which every
    // expression of the loop shares), given a register name of its own: one slot's values then
    // no longer clash as one wherever any of them is wanted. A value a way out writes back, or
    // that is wanted where a jump goes, keeps the slot's name.
    int renamed = 0;
    void renameValues() {
        std::vector<bool> joined(ins.size(), false);
        if (!joined.empty()) joined[0] = true;
        for (int e : entries)
            if (e >= 0 && static_cast<size_t>(e) < joined.size()) joined[static_cast<size_t>(e)] = true;
        for (size_t i = 0; i < ins.size(); i++) {
            if (!in[i].known) continue;
            const Result& r = results[i];
            for (const LOp& l : r.ops)
                if (l.jumpTo >= 0) joined[static_cast<size_t>(l.jumpTo)] = true;
            for (const ExitPoint& e : r.exits)
                if (e.toEntry >= 0) joined[static_cast<size_t>(e.toEntry)] = true;
            for (const Edge& e : r.edges)
                if (e.to >= 0 && static_cast<size_t>(e.to) != i + 1) joined[static_cast<size_t>(e.to)] = true;
        }
        auto has = [](const std::vector<Reg>& v, const Reg& d) { return std::find(v.begin(), v.end(), d) != v.end(); };
        auto exitReads = [&](const ExitPoint& e, const Reg& d) {
            for (const LOp& l : e.stores)
                if (has(l.uses, d)) return true;
            return false;
        };
        // The value written at (i, p): the steps reading it, the ways out writing it back, and
        // for each instruction it is in, the steps it is the slot's value at (`hi` past the last
        // step: on falling through, or out by the fall's way out). A way out that writes it back
        // is renamed with it where every step leaving by it is one of those.
        struct Web {
            std::vector<std::pair<size_t, size_t>> sites;
            std::set<std::pair<size_t, int>> outs;
            std::map<size_t, std::pair<size_t, size_t>> cover;
        };
        auto follow = [&](size_t i, size_t p, const Reg& d, Web& w) {
            size_t ii = i, pp = p + 1;
            for (;;) {
                const Result& r = results[ii];
                const size_t lo = pp;
                for (; pp < r.ops.size(); pp++) {
                    const LOp& l = r.ops[pp];
                    const bool reads = has(l.uses, d), writes = has(l.defs, d);
                    if (reads) w.sites.emplace_back(ii, pp);
                    if (l.exit >= 0 && exitReads(r.exits[static_cast<size_t>(l.exit)], d)) w.outs.emplace(ii, l.exit);
                    if (writes && !reads) {
                        w.cover[ii] = {lo, pp};  // its way out is before the write; its jump after
                        return true;
                    }
                    if (l.jumpTo >= 0 && liveIn[static_cast<size_t>(l.jumpTo)].count(d)) return false;
                    if (l.jumpExit >= 0 && exitReads(r.exits[static_cast<size_t>(l.jumpExit)], d)) w.outs.emplace(ii, l.jumpExit);
                }
                w.cover[ii] = {lo, r.ops.size()};
                if (r.fallExit >= 0) {
                    if (exitReads(r.exits[static_cast<size_t>(r.fallExit)], d)) w.outs.emplace(ii, r.fallExit);
                    return true;
                }
                if (!r.fallsThrough || ii + 1 >= ins.size()) return true;
                if (!liveIn[ii + 1].count(d)) return true;
                if (joined[ii + 1] || !in[ii + 1].known || results[ii + 1].island) return false;
                ii++;
                pp = 0;
            }
        };
        // Every step leaving by a way out the value is written back on is one it is the value at.
        auto outsHold = [&](const Web& w, const Reg& d) {
            for (const auto& [ii, k] : w.outs) {
                const Result& r = results[ii];
                if (r.exits[static_cast<size_t>(k)].toEntry >= 0) return false;
                const auto [lo, hi] = w.cover.at(ii);
                for (size_t q = 0; q < r.ops.size(); q++) {
                    const LOp& l = r.ops[q];
                    if (l.exit == k && (q < lo || q > hi)) return false;
                    // A jump goes after the step's writes: past the value where the step writes it.
                    if (l.jumpExit == k && (q < lo || q > hi || (q == hi && has(l.defs, d)))) return false;
                }
                if (r.fallExit == k && hi != r.ops.size()) return false;
            }
            return true;
        };
        for (size_t i = 0; i < ins.size(); i++) {
            if (!in[i].known || results[i].island) continue;
            for (size_t p = 0; p < results[i].ops.size(); p++) {
                const std::vector<Reg> defs = results[i].ops[p].defs;
                for (const Reg& d : defs) {
                    if (d.slot < 0 || has(results[i].ops[p].uses, d)) continue;
                    Web w;
                    if (!follow(i, p, d, w) || w.sites.empty() || !outsHold(w, d)) continue;
                    const Reg to{-1000000 - renamed++, d.cls};
                    auto rename = [&](LOp& l) {
                        for (auto* list : {&l.named, &l.uses, &l.defs})
                            for (Reg& x : *list)
                                if (x == d) x = to;
                    };
                    // The write, each read (one that writes it again, as an increment does, keeps
                    // it the same value), and the ways out writing it back (to its slot, as before).
                    for (Reg& x : results[i].ops[p].named) if (x == d) x = to;
                    for (Reg& x : results[i].ops[p].defs) if (x == d) x = to;
                    for (const auto& [si, sp] : w.sites) rename(results[si].ops[sp]);
                    for (const auto& [ii, k] : w.outs)
                        for (LOp& l : results[ii].exits[static_cast<size_t>(k)].stores) rename(l);
                }
            }
        }
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
        // Where the region starts, before its first instruction: the steps of its loop's template.
        liveStart = liveIn.empty() ? std::set<Reg>{} : liveIn[0];
        for (auto l = headOps.rbegin(); l != headOps.rend(); ++l) {
            for (const Reg& d : l->defs) liveStart.erase(d);
            liveStart.insert(l->uses.begin(), l->uses.end());
        }
        for (int e : entries)
            for (const Reg& r : e == 0 ? liveStart : liveIn[static_cast<size_t>(e)])
                if (r.slot < 0) refuse("a scratch register read before it is written");
    }
    std::set<Reg> liveStart;
    const std::set<Reg>& liveAt(int entry) const { return entry == 0 ? liveStart : liveIn[static_cast<size_t>(entry)]; }

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
        // What each way in loads, at once; and the steps before the region's first instruction.
        for (int e : entries) together(liveAt(e));
        {
            std::set<Reg> live = liveIn.empty() ? std::set<Reg>{} : liveIn[0];
            for (auto l = headOps.rbegin(); l != headOps.rend(); ++l) {
                for (const Reg& d : l->defs) {
                    note(d);
                    for (const Reg& x : live) meet(d, x);
                }
                for (const Reg& d : l->defs) live.erase(d);
                live.insert(l->uses.begin(), l->uses.end());
                for (const Reg& u : l->named) note(u);
            }
        }
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
        // The order (Chaitin's, as Briggs has it): one clashing with fewer than there are
        // registers set aside, those it meets then clashing with one fewer, and again; where
        // none is, the one clashing most set aside all the same, in hope. Registers given the
        // other way round, each the lowest none of those it meets has: one set aside with fewer
        // clashes than registers always finds one, and the hoped-for often do.
        std::vector<Reg> order;
        {
            std::map<Reg, int> degree;
            std::set<Reg> left;
            for (const auto& [r, others] : clash) {
                degree[r] = static_cast<int>(others.size());
                left.insert(r);
            }
            while (!left.empty()) {
                auto pick = left.end();
                for (auto it = left.begin(); it != left.end(); ++it)
                    if (degree[*it] < (it->cls == CI ? 9 : 8)) {
                        pick = it;
                        break;
                    }
                if (pick == left.end())
                    for (auto it = left.begin(); it != left.end(); ++it)
                        if (pick == left.end() || degree[*it] > degree[*pick]) pick = it;
                const Reg r = *pick;
                left.erase(pick);
                order.push_back(r);
                for (const Reg& o : clash[r])
                    if (left.count(o)) degree[o]--;
            }
            std::reverse(order.begin(), order.end());
        }
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

    // The slots some state takes another slot to be a copy of, as entered: their memory must
    // hold what was entered until a way out has copied it, so none of them is written there
    // early.
    std::set<int> copySources() const {
        std::set<int> from;
        auto add = [&](const State& st) {
            for (const auto& [slot, k] : st.kinds)
                if ((k.bits & kCopies) != 0 && k.src >= 0) from.insert(k.src);
        };
        for (size_t i = 0; i < ins.size(); i++) {
            if (!in[i].known) continue;
            add(in[i]);
            add(results[i].after);
            for (const ExitPoint& e : results[i].exits) add(e.state);
        }
        return from;
    }

    // One more value kept in its slot: of those that can be (a slot's, one kind wherever it is
    // written, not changed in place by Math's functions, not one another is a copy of as
    // entered), the one clashing most for how often it is used. False where there is none.
    bool spillOne() {
        std::map<Reg, int> used;
        std::set<Reg> barred;
        for (const int slot : copySources()) {
            barred.insert(Reg{slot, CI});
            barred.insert(Reg{slot, CF});
        }
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
        // The steps before the region's first instruction are not rewritten: what they use stays.
        for (const LOp& l : headOps)
            for (const auto* list : {&l.named, &l.uses, &l.defs})
                for (const Reg& r : *list) barred.insert(r);
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
    // would leave too often for what it does between, the VM alone is as quick. A call counts as
    // two (2026-11-13, docs/calls-plan.html): the VM's quick call keeps everything in the frame's
    // slots, where the region writes its registers back and loads them again around it; a loop of
    // a call by name and six steps of its own ran 35% slower as a region than in the VM alone.
    // But one as one alone: the call a small method put in place makes where its object is of
    // another class (behind JUMP_IF_NOT_CLASS), which is seldom made, if ever.
    bool worthIt() const {
        int work = 0, islands = 0;
        const std::vector<bool> ending = endingRuns(static_cast<int>(ins.size()), [&](int i) { return ins[static_cast<size_t>(i)].op; });
        const std::vector<bool> otherClass = behindClassChecks();
        for (size_t i = 0; i < ins.size(); i++) {
            if (!in[i].known) continue;
            if (results[i].island) {
                // On a way out of the loop by return or throw: not its work.
                if (!ending[i]) islands += isCall(ins[i].op) && !otherClass[i] ? 2 : 1;
                continue;
            }
            for (const LOp& l : results[i].ops)
                if (l.stem != "jump" && l.stem != "movi" && l.stem != "movf") work++;
        }
        static const int ratio = [] {
            const char* given = std::getenv("JADE_NATIVE_RATIO");
            return given != nullptr ? std::atoi(given) : 6;
        }();
        // JADE_WORTH: what each loop does itself and leaves to the VM, as weighed here.
        static const bool show = std::getenv("JADE_WORTH") != nullptr;
        if (show) std::fprintf(stderr, "worth %s at %u: work %d islands %d\n", f.name.c_str(), ins[0].at, work, islands);
        return work >= 3 && work >= ratio * islands;
    }
    // The instructions run only where a small method's object is of another class: from where a
    // JUMP_IF_NOT_CLASS jumps to, the call made as before, up to where the JUMP over them (the end
    // of the body put in place, just before) goes (ModuleCompiler::compileGuardedCall).
    std::vector<bool> behindClassChecks() const {
        std::vector<bool> out(ins.size(), false);
        auto targetOf = [&](size_t k) -> int {
            const Ins& x = ins[k];
            const int t = targetOperand(x.op);
            if (t <= 0) return -1;
            auto to = index.find(static_cast<uint32_t>(i32(x, t - 1)));
            return to == index.end() ? -1 : to->second;
        };
        for (size_t k = 0; k < ins.size(); k++) {
            if (ins[k].op != Op::JumpIfNotClass) continue;
            const int slow = targetOf(k);
            if (slow <= static_cast<int>(k) + 1 || ins[static_cast<size_t>(slow) - 1].op != Op::Jump) continue;
            const int end = targetOf(static_cast<size_t>(slow) - 1);
            for (int i = slow; i < end; i++) out[static_cast<size_t>(i)] = true;
        }
        return out;
    }
    static bool isCall(Op op) {
        switch (op) {
            case Op::Call:
            case Op::CallNamed:
            case Op::CallOn:
            case Op::CallSpread:
            case Op::Invoke:
            case Op::InvokeImplicit:
            case Op::InvokeOwn:
            case Op::InvokeOwnTrailing:
            case Op::InvokeTrailing:
            case Op::RCall:
            case Op::RInvoke:
            case Op::SuperInvoke:
            case Op::SuperInvokeTrailing: return true;
            default: return false;
        }
    }
};

// ---- Making the machine code ----

// What a way in loads: each register it reads before writing, from its slot. Where the region
// takes a slot as entered to hold what it made there on another way (an int, a float, a byte),
// the slot is checked to hold that first, and the region leaves at once where it does not (to
// `leave`): what it made may rest on a guess, which says nothing of what it is entered with.
void loads(Assembler& a, const Region& r, int entry, std::vector<std::string>* names, int leave) {
    const State& st = r.in[static_cast<size_t>(entry)];
    for (const Reg& reg : r.liveAt(entry)) {
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
    for (const Reg& reg : r.liveAt(entry)) {
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
    for (const LOp& l : r.prologue) {
        Step& st = add(l.stem);
        st.slot = l.slot;
        st.ki = l.ki;
        st.slot2 = l.slot2;
    }
    loads(a, r, 0, names, leaveAt(0));
    for (const LOp& l : r.headOps) {
        if ((l.stem == "movi" || l.stem == "movf") && l.named.size() == 2 && r.regs.at(l.named[0]) == r.regs.at(l.named[1])) continue;
        Step& st = add(r.name(l));
        st.ki = l.ki;
        st.kf = l.kf;
        st.slot = l.slot;
    }
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

// Whether a region may do an instruction itself, as far as its kind says (it may still leave it
// to the VM, as the analysis finds what its operands are).
bool mayDo(Op op) {
    if (typed(op).kind != Typed::None || untyped(op).kind != Typed::None) return true;
    switch (op) {
        case Op::MoveS:
        case Op::GetLocal:
        case Op::MoveK:
        case Op::Const:
        case Op::Null:
        case Op::True:
        case Op::False:
        case Op::SetTop:
        case Op::Pop:
        case Op::Dup2:
        case Op::RNot:
        case Op::RJumpIfFalse:
        case Op::And:
        case Op::Or:
        case Op::Jump:
        case Op::Loop:
        case Op::RangeNext:
        case Op::RangeLoop:
        case Op::RGetIndexSS:
        case Op::RGetIndexSK:
        case Op::RGetIndexPropSS:
        case Op::RGetIndexPropSK:
        case Op::GetIndexRef:
        case Op::KeepLocal:
        case Op::OwnLocal:
        case Op::PopUnder:
        case Op::GetProp:
        case Op::GetOwn:
        case Op::GetPropRef:
        case Op::GetOwnRef:
        case Op::AssertNotNull:
        case Op::RGetUpvalue:
        case Op::JumpIfNotClass:
        case Op::RGetGlobal:
        case Op::RCall:
        case Op::WithDestinationAt:
        case Op::PlaceIndex:
        case Op::Swap:
        case Op::SetIndex:
        case Op::Numeric:
        case Op::RNeg:
        case Op::RShlSK:
        case Op::RShrSK:
        case Op::RBitAndSS:
        case Op::RBitAndSK:
        case Op::RBitOrSS:
        case Op::RBitOrSK:
        case Op::RShlSS:
        case Op::RShrSS:
        case Op::IterNext:
        case Op::IterNextRef:
        case Op::RGetProp:
        case Op::RSetPropSS:
        case Op::RSetPropSK: return true;
        default: return false;
    }
}

// The loops of a function's run code: each Loop jumping back, with where it jumps.
struct Candidate {
    int first, last;  // instructions, the loop's start and its Loop
};

void makeRegionsTimed(FunctionProto& f, const Value* frame, int frameSlots, uint32_t head, std::vector<FunctionProto::NativeRegion>& made);

// How long making regions took, said as the program ends where JADE_TIME_NATIVE is set.
double phaseTimes[4] = {};  // each phase's time, for JADE_TIME_NATIVE
long refusals = 0;
double setupTime = 0;
struct NativeTimes {
    double seconds = 0;
    long functions = 0;
    ~NativeTimes() {
        if (std::getenv("JADE_TIME_NATIVE") != nullptr)
            std::fprintf(stderr, "  hints %.1f, analysis %.1f, liveness %.1f, registers %.1f ms; %ld refused; setup %.1f ms\n", phaseTimes[0] * 1000, phaseTimes[1] * 1000, phaseTimes[2] * 1000, phaseTimes[3] * 1000, refusals, setupTime * 1000);
        if (std::getenv("JADE_TIME_NATIVE") != nullptr) std::fprintf(stderr, "native code: %ld functions, %.1f ms making regions\n", functions, seconds * 1000);
    }
} nativeTimes;

void makeRegions(FunctionProto& f, const Value* frame, int frameSlots, uint32_t head, std::vector<FunctionProto::NativeRegion>& out) {
    const auto start = std::chrono::steady_clock::now();
    makeRegionsTimed(f, frame, frameSlots, head, out);
    const double took = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    nativeTimes.seconds += took;
    static const bool each = std::getenv("JADE_TIME_NATIVE") != nullptr && std::string(std::getenv("JADE_TIME_NATIVE")) == "each";
    if (each && took > 0.0003) std::fprintf(stderr, "  %.1f ms %s (%zu bytes of run code)\n", took * 1000, f.name.c_str(), f.run.size());
    nativeTimes.functions++;
}

void makeRegionsTimed(FunctionProto& f, const Value* frame, int frameSlots, uint32_t head, std::vector<FunctionProto::NativeRegion>& out) {
    if (!enabled()) return;
    // A function with no loop: nothing to make, found at once.
    bool anyLoop = false;
    for (size_t at = 0; at < f.run.size() && !anyLoop;) {
        const char* name = nullptr;
        anyLoop = opAt(f.run.data() + at + kThreaded) == Op::Loop;
        at += kThreaded + runSize(f.run, at + kThreaded, name);
    }
    if (!anyLoop) return;
    const auto setupStart = std::chrono::steady_clock::now();
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
    setupTime += std::chrono::duration<double>(std::chrono::steady_clock::now() - setupStart).count();
    static const char* const dump = std::getenv("JADE_DUMP_NATIVE");
    const bool dumping = dump != nullptr && (f.name == dump || std::strcmp(dump, "*") == 0);
    for (const Candidate& c : loops) {
        bool inside = false;
        for (const Candidate& m : made) inside = inside || (c.first <= m.last && c.last >= m.first);
        if (inside) continue;
        if (all[static_cast<size_t>(c.first)].at != head) continue;  // another loop's: made as it starts
        auto headTop = tops.find(all[static_cast<size_t>(c.first)].at);
        if (headTop == tops.end() || headTop->second < 0) continue;
        // At a glance, before the analysis: a loop that starts with an instruction no region does,
        // or has too many of them for what it would do itself (Region::worthIt), stays the VM's.
        {
            int can = 0, cannot = 0;
            const std::vector<bool> ending = endingRuns(c.last - c.first + 1, [&](int i) { return all[static_cast<size_t>(c.first + i)].op; });
            for (int i = c.first; i <= c.last; i++)
                if (!ending[static_cast<size_t>(i - c.first)]) (mayDo(all[static_cast<size_t>(i)].op) ? can : cannot)++;
            if (!mayDo(all[static_cast<size_t>(c.first)].op) || can < 3 || cannot * 6 > can) {
                if (dumping)
                    std::fprintf(stderr, "== %s: no region for the loop at %u: %d of its instructions the VM's, %d a region's\n", f.name.c_str(),
                                 all[static_cast<size_t>(c.first)].at, cannot, can);
                continue;
            }
        }
        if (c.last - c.first > 600) {
            if (dumping) std::fprintf(stderr, "== %s: no region for the loop at %u: more than 600 instructions\n", f.name.c_str(), all[static_cast<size_t>(c.first)].at);
            continue;
        }
        // Made a second time where the first found lists whose items must be read as ints.
        std::set<int> intLists;
        for (int attempt = 0; attempt < 2; attempt++) {
        Region r(f);
        r.frame = frame;
        r.frameSlots = frameSlots;
        r.tops = tops;
        r.intLists = intLists;
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
            auto t0 = std::chrono::steady_clock::now();
            auto lap = [&](int k) {
                const auto t1 = std::chrono::steady_clock::now();
                phaseTimes[k] += std::chrono::duration<double>(t1 - t0).count();
                t0 = t1;
            };
            r.learnHints();
            lap(0);
            r.analyse(headTop->second);
            lap(1);
            if (!r.worthIt()) refuse("too little done between what the VM does");
            r.lookAtListsOnce();
            r.findLive();
            static const bool ahead = [] {
                const char* asked = std::getenv("JADE_NATIVE_TEMPLATES");
                return asked == nullptr || std::string(asked) != "0";
            }();
            if (ahead) {
                r.makeAhead();
                if (r.templates > 0) r.findLive();
            }
            r.renameValues();
            r.findLive();
            lap(2);
            while (!r.giveRegisters()) {
                if (!r.spillOne()) refuse("more values at once than registers, and none can be kept in its slot");
                r.findLive();
            }
            lap(3);
            std::vector<std::string> names;
            const auto ways = make(r, dumping ? &names : nullptr);
            if (ways.empty()) break;
            uint32_t* misses = new uint32_t(0);  // kept as the region's code is, for as long as the program runs
            for (const auto& [at, code] : ways) out.push_back(FunctionProto::NativeRegion{at, code, misses});
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
            if (attempt == 0 && !r.wantInts.empty()) {
                intLists = r.wantInts;
                continue;
            }
            refusals++;
            if (dumping) {
                std::fprintf(stderr, "== %s: no region for the loop at %u: %s\n", f.name.c_str(), all[static_cast<size_t>(c.first)].at, e.what());
                for (size_t i = 0; i < r.results.size() && i < r.in.size(); i++)
                    if (r.in[i].known && r.results[i].island) std::fprintf(stderr, "  the VM's: %s at %u\n", opName(r.ins[i].op), r.ins[i].at);
            }
        }
        break;
        }
    }
}

// Where a function's loops start, found as its run code is finished: their regions are made the
// first time one of them starts (VM::nativeFirst).
bool findLoops(FunctionProto& f) {
    if (!enabled()) return false;
    for (size_t at = 0; at < f.run.size();) {
        const char* name = nullptr;
        const size_t size = runSize(f.run, at + kThreaded, name);
        if (opAt(f.run.data() + at + kThreaded) == Op::Loop) {
            int32_t target;
            std::memcpy(&target, f.run.data() + at + kThreaded + kOpBytes, 4);
            if (target >= 0 && static_cast<size_t>(target) <= at &&
                std::find(f.nativeHeads.begin(), f.nativeHeads.end(), static_cast<uint32_t>(target)) == f.nativeHeads.end())
                f.nativeHeads.push_back(static_cast<uint32_t>(target));
        }
        at += kThreaded + size;
    }
    f.nativeArrivals.assign(f.nativeHeads.size(), 0);
    return !f.nativeHeads.empty();
}

#if JADE_NATIVE
const bool hooked = (findNativeLoops = &findLoops, makeNativeRegions = &makeRegions, true);
#endif

}  // namespace

}  // namespace jade::native
