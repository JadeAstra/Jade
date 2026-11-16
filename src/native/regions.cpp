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
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "../bytecode.h"
#include "../value.h"
#include "native.h"

namespace ww::native {

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
    if (into.top != from.top) refuse("two depths of the stack meet: " + std::to_string(into.top) + " and " + std::to_string(from.top));
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
// The float registers given out (stencils.cpp): f0 to f15, two to each XMM register.
constexpr int kFloatRegisters = 16;
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
    int64_t ret = 0;     // a third word for the step (mkplace: the place)
    int jumpTo = -1;     // an instruction of the region
    int jumpExit = -1;   // or a way out (an index into the instruction's exits)
    int exit = -1;       // where it leaves for the VM when it cannot do what it does (an exit's index)
    const ObjString* field = nullptr;  // the field a getp or setp step reads or writes, where its name is known
    bool whole = false;  // its float register, a low half, written whole (Region::markWhole)
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
    // A whole function's region (2026-11-15): its loops are left out (`outside`: a way there
    // leaves for the VM, which goes into the loop's own region as it would), and the region is
    // come back into where each loop ends (`seeds`, more ways in). Each loop keeps the region it
    // has (lists looked at once, fields held), which a region with other ways in cannot have.
    std::set<int> outside;
    std::set<int> seeds;
    std::vector<Result> results;              // each instruction's, in its state

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
    // Whether the frame's slot holds a List<bool> (packed, none null) where the region is made
    // (2026-11-15: its items read into the flag register and set from it; the steps check it).
    bool frameBools(int slot) const {
        if (frame == nullptr || slot < 0 || slot >= frameSlots || slot >= f.maxStack) return false;
        const Value& v = frame[slot];
        return v.isObjType(ObjType::List) && asList(v)->packed() && asList(v)->store() == ListStore::Bools && !asList(v)->pack->nullable;
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
    // The class an RCall at `i` makes a value of in a box from its header alone (2026-11-15:
    // `Rect(x, y, w, h)`), where slot `callee` holds it as the region is made: a value type kept
    // in a box (no Small one), made from its header, each of its fields an int, a float or a
    // byte, of no type given before and with no other place for it given (WithTypes or
    // WithDestinationAt since the call before, which the VM keeps for the call that follows:
    // made here, it would be left for the next). Where WithPlace gave it a variable's place
    // (`place`, its number), one kept in the value itself too, made there. Null for anything else.
    const ObjClass* boxedValue(const State& s, int callee, int argc, int i, int& place) const {
        const Value* v = knownValue(s, callee);
        return v != nullptr && v->isObjType(ObjType::Class) ? boxedClass(asClass(*v), argc, i, place) : (place = -1, nullptr);
    }
    const ObjClass* boxedClass(const ObjClass* cls, int argc, int i, int& place) const {
        place = -1;
        for (int back = i - 1; back >= 0 && back >= i - 64; back--) {
            const Op o = ins[static_cast<size_t>(back)].op;
            if (o == Op::WithTypes || o == Op::WithDestinationAt) return nullptr;
            if (o == Op::WithPlace) {
                place = u8(ins[static_cast<size_t>(back)], 0);
                break;
            }
            if (isCall(o)) break;
        }
        if (!cls->isValue || !cls->madeFromHeader || cls->staticInit != nullptr || cls->fieldTypesMaker != nullptr || (cls->smallId != 0 && place < 0) ||
            cls->isAbstract || cls->isInterface || cls->enumSealed || cls->components.size() != static_cast<size_t>(argc) || argc == 0)
            return nullptr;
        const ValueLayout* const layout = cls->layout.load(std::memory_order_acquire);
        if (layout == nullptr || (place >= 0 && layout->size > ObjPlace::kBytes)) return nullptr;
        for (int k = 0; k < argc; k++) {
            const ValueField* field = headerFieldOf(cls, layout, k);
            if (field == nullptr || !(field->kind == ValueField::Int || field->kind == ValueField::Float ||
                                      (field->kind == ValueField::Narrow && field->narrow == Narrow::Byte)))
                return nullptr;
        }
        return cls;
    }
    // The RCalls made in a variable's place, each with that place's number: the SetPlace after
    // one only holds what it made (Ctx::step's SetPlace).
    struct MadeInPlace {
        int place;
        const ObjClass* cls;
        int slot;  // where the Ref to it is
    };
    mutable std::map<int, MadeInPlace> madeInPlace;
    // The field of a value type's header's component `k`: the field of its name.
    static const ValueField* headerFieldOf(const ObjClass* cls, const ValueLayout* layout, int k) {
        if (layout == nullptr || k < 0 || static_cast<size_t>(k) >= cls->components.size()) return nullptr;
        const ObjString* const name = cls->components[static_cast<size_t>(k)];
        for (const ValueField& field : layout->fields)
            if (field.name == name || (field.name != nullptr && name != nullptr && field.name->utf8() == name->utf8())) return &field;
        return nullptr;
    }
    // Whether the region makes a closure, which may hold slots of the frame open.
    // Whether the region gives the VM objects to look in for a bare name first (ImplicitFirst,
    // a block run on another object), which its class alone does not say.
    bool hasImplicitFirst() const {
        for (const Ins& x : ins)
            if (x.op == Op::ImplicitFirst) return true;
        return false;
    }
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
        // A value in a place of a frame's (made where a call leaves it, 2026-11-13).
        if (v.isRef() && v.refDepth == 0 && v.refOffset == 0 && v.as.obj->type == ObjType::Places) {
            const ObjPlace* place = static_cast<const ObjPlace*>(v.as.obj);
            if (place->type == nullptr) return place->cls;
        }
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
            if (found == r.index.end() || r.outside.count(found->second)) {
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
            if (r.outside.count(i + 1)) {
                // On into a loop left out: a way out to the VM there.
                op("jump").jumpExit = exitTo(r.ins[static_cast<size_t>(i) + 1].at, st);
                out.fallsThrough = false;
                return;
            }
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
            static const bool trace = std::getenv("WW_TRACE_ISLANDS") != nullptr;
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

    // A value of the value type `boxed` made from its header (2026-11-15): in a box, or in the
    // frame's place `place` (WithPlace's) where it is not -1, into slot `a`, where slot `callee`
    // holds the class. `arg(k)` gives part k: its slot, or a constant's number where the second
    // is true. Each part read as its field's kind (checked where the region only guesses it),
    // the box made (mkbox: the VM's where a collection is due) or the place made ready (mkplace),
    // then each field written into it.
    // The bare name of constant `nameAt` no member of the object the function runs on, as the VM
    // finds it as the region is made (GlobalsSeen::noMember, by its class alone): the step that
    // checks it still is (nomember: the object of that class, no Table's keys changed since),
    // where the VM does the instruction otherwise. Anything else, the instruction the VM's.
    void noMemberHere(Ctx& c, int nameAt) {
        const Value& name = constant(nameAt);
        const GlobalsSeen& seen = nativeGlobalsSeen;
        if (!name.isObjType(ObjType::String) || seen.noMember == nullptr || frame == nullptr || frameSlots < 1 || c.s.kind(0).bits != kEntry ||
            hasImplicitFirst())
            island();
        const Value& self = frame[0];
        if (!(self.isObjType(ObjType::Instance) || self.isObjType(ObjType::Value)) || !seen.noMember(seen.vm, f, asString(name), self)) island();
        LOp& check = c.op("nomember");
        check.ki = static_cast<int64_t>(reinterpret_cast<intptr_t>(classOf(self)));
        check.ret = static_cast<int64_t>(tableKeysChanged.load(std::memory_order_relaxed));
        c.leaves(check);
    }
    template <typename Arg>
    void makeValue(Ctx& c, const ObjClass* boxed, int place, int a, int callee, int argc, Arg arg) {
        State& s = c.s;
        const ValueLayout* const layout = boxed->layout.load(std::memory_order_acquire);
        struct Part {
            const ValueField* field;
            Reg reg;
            bool constant;
            Value value;
        };
        std::vector<Part> parts;
        for (int k = 0; k < argc; k++) {
            const ValueField* field = headerFieldOf(boxed, layout, k);
            const auto [xk, isConstant] = arg(k);
            const bool isFloat = field->kind == ValueField::Float, isByte = field->kind == ValueField::Narrow;
            if (isConstant) {
                const Value& v = constant(xk);
                if (isFloat ? !v.isFloat() : !(v.isInt() || v.isByte())) island();
                if (isByte && (v.as.i < 0 || v.as.i > 255)) island();
                parts.push_back(Part{field, Reg{}, true, v});
            } else if (isByte) {
                if (s.kind(xk).bits != kByte && s.kind(xk).bits != kWhole) island();
                const Reg part = c.read(xk, CI, false);
                if (s.kind(xk).bits == kWhole) c.leaves(c.op("chkbyte", {part}, {part}));
                parts.push_back(Part{field, part, false, {}});
            } else {
                parts.push_back(Part{field, c.number(xk, isFloat ? CF : CI, !isFloat), false, {}});
            }
        }
        if (place >= 0) madeInPlace[c.i] = MadeInPlace{place, boxed, a};
        c.writeMemory(a);
        LOp& made = c.op(place >= 0 ? "mkplace" : "mkbox");
        made.slot = callee;
        made.slot2 = a;
        made.ki = static_cast<int64_t>(reinterpret_cast<intptr_t>(boxed));
        made.ret = place;
        c.leaves(made);
        for (const Part& p : parts) {
            const bool isFloat = p.field->kind == ValueField::Float, isByte = p.field->kind == ValueField::Narrow;
            Reg r = p.reg;
            if (p.constant) {
                r = isFloat ? Reg{kScratchF, CF} : Reg{kScratchI, CI};
                if (isFloat) c.op("setfk", {r}, {}, {r}).kf = p.value.as.d;
                else c.op("setik", {r}, {}, {r}).ki = p.value.as.i;
            }
            LOp& l = c.op(isFloat ? "boxf" : isByte ? "boxb" : "boxi", {r}, {r});
            l.slot = a;
            l.ki = static_cast<int64_t>(p.field->offset);
        }
        noteValue(s, a, boxed);
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
            case Op::WithPlace:
                // A variable's place for the call after to make its value in (2026-11-15): the
                // RCall reads which from here (madeInPlace), and the VM, doing a call the region
                // does not, makes it elsewhere and SetPlace copies it in, the same value.
                c.fall(s);
                return;
            case Op::SetPlace: {  // u16 slot, u8 place: the variable holds what was made in its place
                const int slot = u16(x, 0), top = s.top - 1;
                // Only right after a call the region made its value for in a place (mkplace,
                // which also makes sure the frame has places): copied into the variable's own,
                // where it was made in another (setplace).
                auto made = madeInPlace.find(c.i - 1);
                if (made == madeInPlace.end() || made->second.slot != top || top < 0) island();
                if (slot != top) c.writeMemory(slot);
                LOp& l = c.op("setplace");
                l.slot = slot;
                l.slot2 = top;
                l.ret = u8(x, 2);
                noteValue(s, slot, made->second.cls);
                c.fall(s);
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
                // A List<bool>'s item into the flag register (2026-11-15), the list checked by the step.
                if (c.r.frameBools(list)) {
                    Reg index{};
                    if (x.op == Op::RGetIndexSK) {
                        index = Reg{kScratchI, CI};
                        c.op("setik", {index}, {}, {index}).ki = c.intConstant(u8(x, 2));
                    } else {
                        index = c.read(u8(x, 2), CI, true);
                    }
                    LOp& l = c.op("idxbool", {index}, {index});
                    l.slot = list;
                    c.leaves(l);
                    c.write(a, K{kBool});
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
            case Op::GetImplicit: {  // u16 name, u16 global -> the global, where the name is no member
                // A bare name read in a method that the VM, as the region is made, finds no member
                // of the object the function runs on (2026-11-15: `Rect` for a result's check):
                // that checked (nomember), then the global read as RGetGlobal reads it.
                const int top = s.top;
                noMemberHere(c, u16(x, 0));
                c.writeMemory(top);
                LOp& l = c.op("getg");
                l.ki = u16(x, 2);
                l.slot2 = top;
                c.leaves(l);
                if (const Value* g = globalSeen(u16(x, 2)); g != nullptr && g->kind == ValueKind::Obj) noteFieldValue(s, top, *g);
                s.top = top + 1;
                c.fall(s);
                return;
            }
            case Op::InvokeImplicit: {  // u16 name, u16 global, u8 argc: receiver, arguments -> result
                // A bare name called in a method (2026-11-15: `Rect(...)` in Image.inkBox): where
                // the VM, as the region is made, finds the name no member of the class of the
                // object the function runs on, and the global is a value type a region makes (as
                // RCall's): the object checked to be of that class with no Table's keys changed
                // since (nomember), the global read into the receiver's slot (getg), and the value
                // made there. Anything else the VM's.
                madeInPlace.erase(c.i);
                const int argc = u8(x, 4), top = s.top, at = top - argc - 1;
                if (at < 1 || argc == 0) island();
                const Value* global = globalSeen(u16(x, 2));
                if (global == nullptr || !global->isObjType(ObjType::Class)) island();
                int place = -1;
                const ObjClass* const boxed = boxedClass(asClass(*global), argc, c.i, place);
                if (boxed == nullptr) island();
                noMemberHere(c, u16(x, 0));
                c.writeMemory(at);
                LOp& read = c.op("getg");
                read.ki = u16(x, 2);
                read.slot2 = at;
                c.leaves(read);
                makeValue(c, boxed, place, at, at, argc, [&](int k) { return std::pair<int, bool>(top - argc + k, false); });
                s.top = at + 1;
                c.fall(s);
                return;
            }
            case Op::RCall: {  // u8 A, u8 F, u8 T, u8 S, u8 argc, u8 flags, u8 X1..X4, i32 slow, i32 resume
                // A value of a value type kept in the value itself, made from its header's byte
                // fields (`Color(r, g, b, a)`, B3), where slot F holds its class as the region is
                // made (the step checks it), nothing of a type given right before, and each part a
                // byte the region knows: its bytes put together. Any other call the VM's.
                madeInPlace.erase(c.i);
                const int a = u8(x, 0), argc = u8(x, 4), flags = u8(x, 5);
                const int callee = c.objectSlot(u8(x, 1));
                int place = -1;
                const ObjClass* const boxed = boxedValue(s, callee, argc, c.i, place);
                const ObjClass* const cls = place >= 0 ? nullptr : madeValue(s, callee, argc, c.i);
                if (cls == nullptr) {
                    // In a box, or in the variable's place WithPlace gave (2026-11-15): each part
                    // read as its field's kind (checked where the region only guesses it), the
                    // box made (mkbox: the VM's where a collection is due) or the place made
                    // ready (mkplace), then each field written into it.
                    if (boxed == nullptr) island();
                    makeValue(c, boxed, place, a, callee, argc, [&](int k) { return std::pair<int, bool>(u8(x, 6 + k), (flags & (1 << k)) != 0); });
                    s.top = u8(x, 2);
                    c.fall(s);
                    return;
                }
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
                        // A byte; or a whole number of a list (an item of bytes, mapChannels' table
                        // read, 2026-11-13), checked to be one first, the VM's where it is not.
                        if (s.kind(xk).bits != kByte && s.kind(xk).bits != kWhole) island();
                        const Reg part = c.read(xk, CI, false);
                        if (s.kind(xk).bits == kWhole) c.leaves(c.op("chkbyte", {part}, {part}));
                        parts.emplace_back(part, static_cast<int>(field->offset));
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
            case Op::RInvoke: {  // u8 A, u8 B, u8 T, u8 S, u8 argc, u8 flags, u8 X1..X4, u16 name, u16 cache, i32 slow, i32 resume
                // `list.add(x)` (2026-11-15): x written at the end of a packed list where its block
                // has room (pushi, pushb, pushf, pushsm), the step checking the list is one of x's
                // kind; anything else the VM's, the call made as before. Any other call the VM's.
                const int a = u8(x, 0), argc = u8(x, 4), flags = u8(x, 5);
                const Value& name = constant(u16(x, 10));
                if (argc != 1 || !name.isObjType(ObjType::String) || asString(name)->utf8() != "add") island();
                const int list = c.objectSlot(u8(x, 1));
                const int xk = u8(x, 6);
                const char* stem = nullptr;
                Reg value{};
                if (flags & 1) {
                    const Value& v = constant(xk);
                    if (v.isFloat()) {
                        value = Reg{kScratchF, CF};
                        c.op("setfk", {value}, {}, {value}).kf = v.as.d;
                        stem = "pushf";
                    } else if (v.isInt() || v.isByte()) {
                        value = Reg{kScratchI, CI};
                        c.op("setik", {value}, {}, {value}).ki = v.as.i;
                        stem = v.isByte() ? "pushb" : "pushi";
                    } else {
                        island();
                    }
                } else if (const uint16_t w = static_cast<uint16_t>(s.kind(xk).bits & ~kEntry); w == kInt || w == kFloat || w == kByte) {
                    value = c.read(xk, w == kFloat ? CF : CI, false);
                    stem = w == kFloat ? "pushf" : w == kByte ? "pushb" : "pushi";
                } else if (const ObjClass* const cls = valueClass(s, xk); cls != nullptr && s.kind(xk).bits != kWhole) {
                    // A value of a value type: a Small one, or a Ref to an item (from[i]).
                    const ValueLayout* const layout = cls->layout.load(std::memory_order_acquire);
                    if (cls->smallId == 0 || cls->smallId >= (1u << 24) || layout == nullptr || layout->size > 8 || a > 0xFFFF) island();
                    const int valueSlot = c.objectSlot(xk);
                    // Slot A written by the step: a copy of it wanted is made first; the way out
                    // keeps A as it was, which the VM's call reads (the list's own slot, maybe).
                    const K before = s.kind(a);
                    const bool had = s.kinds.find(a) != s.kinds.end();
                    const int flag = s.flag;
                    c.writeMemory(a);
                    if (had) s.kinds[a] = before;
                    else s.kinds.erase(a);
                    s.flag = flag;
                    LOp& l = c.op("pushsm");
                    l.slot = list;
                    l.slot2 = static_cast<int64_t>((static_cast<uint64_t>(cls->smallId) << 40) | (static_cast<uint64_t>(a) << 24) |
                                                   static_cast<uint64_t>(valueSlot));
                    l.ki = static_cast<int64_t>(reinterpret_cast<intptr_t>(cls));
                    c.leaves(l);
                    c.write(a, K{kMem});
                    s.top = u8(x, 2);
                    c.fall(s);
                    return;
                } else {
                    // Not typed in the region (a parameter): an int or a float as the typed
                    // instructions or the frame say, checked to be one.
                    const Cls guessed = c.guess(xk);
                    value = c.number(xk, guessed, true);
                    stem = guessed == CF ? "pushf" : "pushi";
                }
                const K before = s.kind(a);
                const bool had = s.kinds.find(a) != s.kinds.end();
                const int flag = s.flag;
                c.writeMemory(a);
                if (had) s.kinds[a] = before;
                else s.kinds.erase(a);
                s.flag = flag;
                LOp& l = c.op(stem, {value}, {value});
                l.slot = list;
                l.slot2 = a;
                c.leaves(l);
                c.write(a, K{kMem});
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
                const ObjClass* const cls = itemsClass(s, list);
                if (cls == nullptr || c.copiedElsewhere(top - 2)) island();
                const Reg index = c.read(top - 1, CI, true);
                LOp& l = c.op("placei", {index}, {index});
                l.slot = list;
                l.slot2 = top - 2;
                // Its items' class, as the list's other steps give theirs (lookAtListsOnce: one
                // class for every step, or the list is not looked at once).
                l.ki = static_cast<int64_t>(reinterpret_cast<intptr_t>(cls));
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
                // A copy of a slot as entered (`this`, pushed for the check): that slot, the same object.
                if (const K k = s.kind(static_cast<int>(l.slot)); k.bits == kCopy && k.src >= 0 && s.kind(k.src).bits == kEntry) l.slot = k.src;
                l.ki = u16(x, 0);
                State after = s;
                after.top = top - 1;
                after.flag = -1;  // the flag holds the answer, no slot's bool
                // An object as the region was entered (an extension's `this`: no step before it
                // writes the slot) is of the class every time round or none (2026-11-13): where it
                // is not, a way out, the VM's way left to the VM, not taken into the region as an
                // island (which would bar lists looked at once and loop templates).
                // A miss where it is not, as a guess that fails is: past the VM's limit of them the
                // region is left to the VM (whose own lookup of the call does as well).
                const bool entered = s.kind(static_cast<int>(l.slot)).bits == kEntry;
                s = after;
                if (entered) {
                    LOp& j = c.op("jfalse");
                    j.jumpExit = c.exitTo(static_cast<uint32_t>(i32(x, 2)), after);
                    if (c.lowering) c.out.exits.back().miss = true;
                } else {
                    c.go(static_cast<uint32_t>(i32(x, 2)), after, c.op("jfalse"));
                }
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
                if (c.r.frameBools(list)) {
                    // A List<bool>'s item set from the flag register (2026-11-15), the value made
                    // a bool there (as it is, or read from its slot, checked to be one).
                    const Reg index = c.read(top - 2, CI, true);
                    c.flagOf(top - 1);
                    LOp& l = c.op("setbool", {index}, {index});
                    l.slot = list;
                    c.leaves(l);
                    c.write(top - 3, K{kBool});
                    s.top = top - 2;
                    c.fall(s);
                    return;
                }
                const Reg index = c.read(top - 2, CI, true);
                const K v = s.kind(top - 1);
                // (A whole number read from a list, an int or a byte, goes as an int: into bytes
                // where it is from 0 to 255, into a list of ints as the int it is; so does an int on
                // one way and such a number on another, 2026-11-15: union-find's root, the pixel
                // itself or a parent read from the list.)
                const bool whole = v.bits == kInt || v.bits == kWhole || v.bits == (kInt | kWhole);
                const char* stem = (v.bits == kFloat || v.bits == kCopyF) ? "setf" : whole ? "setwi" : v.bits == kByte ? "setwb" : nullptr;
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
            case Op::RShrSS:
            case Op::RBitXorSS:
            case Op::RBitXorSK:
            case Op::RUshrSK:
            case Op::RUshrSS:
            case Op::RAddWrapSS:
            case Op::RAddWrapSK:
            case Op::RSubWrapSS:
            case Op::RSubWrapSK:
            case Op::RMulWrapSS:
            case Op::RMulWrapSK: bitwise(c); return;
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
        int16_t fact = known != nullptr ? static_cast<int16_t>(known - facts.data()) : static_cast<int16_t>(-1);
        // A value of a value type as entered, as the frame has it: the copy known as one too (its
        // own slot may hold another variable where the frame was read: a whole function's region
        // is made at a return, 2026-11-15). A guess the steps check.
        if (fact < 0 && (c.s.kind(from).bits == kEntry || inMemory(c.s.kind(from))))
            if (const ObjClass* const cls = valueClass(c.s, from)) {
                Fact v;
                v.value = cls;
                fact = intern(v);
            }
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

    // Whether == of two values of the value type `cls` is their bytes' alone, and how many: no ==
    // of its own, and every field an int, a bool or a narrow whole number, together covering the
    // value byte for byte (no float, whose == is not its bits'; no padding, whose bytes say
    // nothing; no object, which compares by more than where it is).
    static bool bytesCompared(const ObjClass* cls, uint32_t& size) {
        const Table::Entry* entries = cls->methods.entries();
        for (uint32_t k = 0; entries != nullptr && k < cls->methods.capacity(); k++)
            if (entries[k].key != nullptr && entries[k].key->utf8() == "==") return false;
        const ValueLayout* layout = cls->layout.load(std::memory_order_acquire);
        if (layout == nullptr || layout->size == 0 || layout->size > 64) return false;
        std::vector<bool> covered(layout->size, false);
        for (const ValueField& f : layout->fields) {
            const uint32_t width = f.kind == ValueField::Int ? 8 : f.kind == ValueField::Bool ? 1 : f.kind == ValueField::Narrow ? narrowSize(f.narrow) : 0;
            if (width == 0 || f.offset + width > layout->size) return false;
            for (uint32_t b = f.offset; b < f.offset + width; b++) {
                if (covered[b]) return false;
                covered[b] = true;
            }
        }
        for (bool b : covered)
            if (!b) return false;
        size = layout->size;
        return true;
    }

    // The typed instructions, and the untyped ones where the region knows (or guesses) their
    // operands: an int beside a float made a float, as the VM does.
    void arithmetic(Ctx& c, const Typed& t, bool untypedOp) {
        const Ins& x = c.x;
        State& s = c.s;
        const int at = t.kind == Typed::Jump ? 0 : 1;
        const bool bConst = t.form == 2, cConst = t.form == 1;
        const int bOp = u8(x, at), cOp = u8(x, at + 1);
        // == of two values of one value type whose == is their bytes' (2026-11-14: an Image's ==,
        // its pixels compared one by one): the bytes compared where they are, each a value in its
        // slot or an item a Ref there is to (eqsm).
        if (untypedOp && t.kind == Typed::Compare && t.op == '=' && t.form == 0) {
            // Where the slot's value is in memory: the slot, or the one it is a copy of as entered.
            auto inSlot = [&](int slot) {
                K k = s.kind(slot);
                if ((k.bits & kCopies) != 0 && (k.bits & ~kCopies) == 0 && k.src >= 0) slot = k.src, k = s.kind(slot);
                return k.bits == kEntry || inMemory(k) ? slot : -1;
            };
            const ObjClass* const vb = valueClass(s, bOp);
            const int from = inSlot(bOp), to = inSlot(cOp);
            uint32_t size = 0;
            if (vb != nullptr && vb == valueClass(s, cOp) && from >= 0 && to >= 0 && bytesCompared(vb, size)) {
                LOp& l = c.op("eqsm");
                l.slot = from;
                l.slot2 = to;
                l.ki = static_cast<int64_t>(reinterpret_cast<intptr_t>(vb));
                const uint64_t what = static_cast<uint64_t>(vb->smallId) << 32 | size;
                std::memcpy(&l.kf, &what, 8);
                c.leaves(l);
                c.write(u8(x, 0), K{kBool});
                s.top = u8(x, 3);
                c.fall(s);
                return;
            }
        }
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

    // Shifts, and &, |, ^ of whole numbers; &+, &- and &* of ints (2026-11-14: ^, >>> and the
    // wrapping three, for a hash written in WinterWolf).
    void bitwise(Ctx& c) {
        const Ins& x = c.x;
        const int a = u8(x, 0);
        const Op o = x.op;
        const bool k = o == Op::RShlSK || o == Op::RShrSK || o == Op::RBitAndSK || o == Op::RBitOrSK || o == Op::RBitXorSK || o == Op::RUshrSK ||
                       o == Op::RAddWrapSK || o == Op::RSubWrapSK || o == Op::RMulWrapSK;
        // The wrapping three take ints alone (a byte is the VM's, which throws).
        const bool wrap = o == Op::RAddWrapSS || o == Op::RAddWrapSK || o == Op::RSubWrapSS || o == Op::RSubWrapSK || o == Op::RMulWrapSS ||
                          o == Op::RMulWrapSK;
        if (c.guess(u8(x, 1)) != CI) island();
        const Reg b = wrap ? c.number(u8(x, 1), CI, true) : c.read(u8(x, 1), CI, true), dst{a, CI};
        if (k) {
            const int64_t v = c.intConstant(u8(x, 2));
            const bool shift = o == Op::RShlSK || o == Op::RShrSK || o == Op::RUshrSK;
            if (shift && (v < 0 || v >= 64)) island();
            const char* stem = o == Op::RShlSK      ? "shlik"
                               : o == Op::RShrSK    ? "shrik"
                               : o == Op::RUshrSK   ? "ushrik"
                               : o == Op::RBitAndSK ? "andik"
                               : o == Op::RBitOrSK  ? "orik"
                               : o == Op::RBitXorSK ? "xorik"
                               : o == Op::RAddWrapSK ? "waddik"
                               : o == Op::RSubWrapSK ? "wsubik"
                                                     : "wmulik";
            if (o == Op::RUshrSK && v == 32) stem = "ushr32";  // its shift an immediate (stencils.cpp)
            c.op(stem, {dst, b}, {b}, {dst}).ki = v;
        } else if (o == Op::RShlSS || o == Op::RShrSS || o == Op::RUshrSS || o == Op::RSubWrapSS) {
            // A count from 0 to 63 (any other left to the VM), then the shift of b by it (or b less
            // the int): into dst from b, or through the scratch register where dst is the other.
            if (c.guess(u8(x, 2)) != CI) island();
            const Reg r = o == Op::RSubWrapSS ? c.number(u8(x, 2), CI, true) : c.read(u8(x, 2), CI, true);
            const char* stem = o == Op::RShlSS ? "shli" : o == Op::RShrSS ? "shri" : o == Op::RUshrSS ? "ushri" : "wsubi";
            if (o != Op::RSubWrapSS) c.leaves(c.op("chkshift", {r}, {r}));
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
            const Reg r = wrap ? c.number(u8(x, 2), CI, true) : c.read(u8(x, 2), CI, true);
            const char* stem = o == Op::RBitAndSS    ? "andi"
                               : o == Op::RBitOrSS   ? "ori"
                               : o == Op::RBitXorSS  ? "xori"
                               : o == Op::RAddWrapSS ? "waddi"
                                                     : "wmuli";
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
                else if (k.bits == kEntry || inMemory(k)) own(CI, false);  // in memory: read, checked a whole number (2026-11-15)
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
            // No longer given anything (the island before it, say): not reached, and so giving
            // nothing on (stepped through, its state's top unknown, it gave the next a top of -1).
            Result r = in[static_cast<size_t>(i)].known ? transfer(i, in[static_cast<size_t>(i)], false) : Result{};
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
        for (int e : seeds)
            if (e > 0 && e < static_cast<int>(ins.size()) && !outside.count(e) && topAt(e) >= 0) entries.insert(e);
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
                    if (e <= 0 || e >= static_cast<int>(ins.size()) || topAt(e) < 0 || outside.count(e)) continue;
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
        bytesAtOnce();
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

    // ---- A value's bytes read as one int ----

    // `c.red.toInt() | c.green.toInt() << 8 | c.blue.toInt() << 16 | c.alpha.toInt() << 24`
    // (2026-11-14, an Image's hashCode): byte fields of one value at offsets one after another,
    // each shifted 8 bits further, put together with |. Where steps do that one right after another
    // in a straight run of instructions, each run of bytes is read at once (valph, valpw, valpi: 2,
    // 4 or 8 bytes), shifted where it goes and |'d in: three steps or fewer for each run, where
    // each byte took three. The register they are put together in starts with the first byte (a
    // shift of it allowed); each further byte is read into another register, shifted, and |'d in,
    // and that other register holds nothing wanted after. A read that fails leaves at the first
    // read's way out, where the VM reads them all again (reading a field changes nothing).
    void bytesAtOnce() {
        std::vector<int> into(ins.size(), 0);
        for (size_t i = 0; i < ins.size(); i++)
            if (in[i].known)
                for (const Edge& e : results[i].edges)
                    if (e.to >= 0 && static_cast<size_t>(e.to) < ins.size()) into[static_cast<size_t>(e.to)]++;
        auto plainSteps = [&](size_t k) {
            for (const LOp& l : results[k].ops)
                if (l.jumpTo >= 0 || l.jumpExit >= 0) return false;
            return true;
        };
        for (size_t j = 0; j < ins.size();) {
            if (!in[j].known || results[j].island || !plainSteps(j)) {
                j++;
                continue;
            }
            // A straight run: each instruction falling into the next, come to from there alone.
            size_t e = j;
            while (e + 1 < ins.size() && results[e].fallsThrough && results[e].fallExit < 0 && in[e + 1].known && !results[e + 1].island &&
                   into[e + 1] == 1 && !entries.count(static_cast<int>(e + 1)) && !resumed.count(static_cast<int>(e + 1)) && plainSteps(e + 1))
                e++;
            std::vector<std::pair<size_t, size_t>> flat;  // (instruction, step)
            for (size_t k = j; k <= e; k++)
                for (size_t o = 0; o < results[k].ops.size(); o++) flat.emplace_back(k, o);
            fuseBytes(flat);
            j = e + 1;
        }
    }
    struct ByteRun {
        int64_t value, cls, id, offset, count, shift;
    };
    void fuseBytes(const std::vector<std::pair<size_t, size_t>>& flat) {
        auto at = [&](size_t p) -> LOp& { return results[flat[p].first].ops[flat[p].second]; };
        auto byteRead = [&](const LOp& l) { return l.stem == "valpb" && l.named.size() == 1 && l.named[0].cls == CI; };
        std::set<std::pair<size_t, size_t>> gone;
        std::map<std::pair<size_t, size_t>, std::vector<LOp>> instead;
        for (size_t p = 0; p < flat.size();) {
            if (!byteRead(at(p))) {
                p++;
                continue;
            }
            const LOp first = at(p);
            const Reg into = first.named[0];
            auto runOf = [&](const LOp& l) {
                const uint64_t what = static_cast<uint64_t>(l.slot2);
                return ByteRun{l.slot, l.ki, static_cast<int64_t>(what >> 32), static_cast<int64_t>(what & 0xFFFFFFFF), 1, 0};
            };
            std::vector<ByteRun> runs{runOf(first)};
            size_t q = p + 1;
            if (q < flat.size() && at(q).stem == "shlik" && at(q).named.size() == 2 && at(q).named[0] == into && at(q).named[1] == into && at(q).ki > 0 &&
                at(q).ki < 64)
                runs[0].shift = at(q).ki, q++;
            Reg other{0, CI};
            bool otherSet = false;
            size_t pieces = 0;
            for (;;) {
                // A piece: a byte read into the other register, maybe shifted, then |'d in.
                size_t u = q;
                if (u >= flat.size() || !byteRead(at(u)) || at(u).named[0] == into || (otherSet && !(at(u).named[0] == other))) break;
                const Reg t = at(u).named[0];
                ByteRun piece = runOf(at(u));
                u++;
                if (u < flat.size() && at(u).stem == "shlik" && at(u).named.size() == 2 && at(u).named[0] == t && at(u).named[1] == t && at(u).ki > 0 &&
                    at(u).ki < 64)
                    piece.shift = at(u).ki, u++;
                if (u >= flat.size() || at(u).stem != "ori" || at(u).named.size() != 2 || !(at(u).named[0] == into) || !(at(u).named[1] == t)) break;
                u++;
                other = t, otherSet = true;
                ByteRun& last = runs.back();
                if (piece.value == last.value && piece.cls == last.cls && piece.id == last.id && piece.offset == last.offset + last.count &&
                    piece.shift == last.shift + 8 * last.count && last.count < 8)
                    last.count++;
                else
                    runs.push_back(piece);
                pieces++;
                q = u;
            }
            bool worth = false;
            for (const ByteRun& run : runs) worth = worth || run.count > 1;
            // The other register holds nothing wanted after: a slot of the stack above its top
            // once the last instruction of these is done.
            const size_t lastIns = flat[q - 1].first;
            if (!worth || pieces == 0 || lastIns + 1 >= ins.size() || other.slot < in[lastIns + 1].top) {
                p = q > p + 1 ? q : p + 1;
                continue;
            }
            // Each run as reads of 8, 4, 2 or 1 bytes.
            std::vector<LOp> made;
            bool firstRead = true;
            for (const ByteRun& run : runs) {
                int64_t offset = run.offset, left = run.count, shift = run.shift;
                while (left > 0) {
                    const int64_t n = left >= 8 ? 8 : left >= 4 ? 4 : left >= 2 ? 2 : 1;
                    const Reg reg = firstRead ? into : other;
                    LOp read = first;
                    read.stem = n == 8 ? "valpi" : n == 4 ? "valpw" : n == 2 ? "valph" : "valpb";
                    read.named = {reg}, read.uses = {}, read.defs = {reg};
                    read.slot = run.value, read.ki = run.cls;
                    read.slot2 = static_cast<int64_t>(static_cast<uint64_t>(run.id) << 32 | static_cast<uint64_t>(offset));
                    made.push_back(read);
                    if (shift != 0) {
                        LOp sh;
                        sh.stem = "shlik", sh.named = {reg, reg}, sh.uses = {reg}, sh.defs = {reg}, sh.ki = shift;
                        made.push_back(sh);
                    }
                    if (!firstRead) {
                        LOp o;
                        o.stem = "ori", o.named = {into, other}, o.uses = {into, other}, o.defs = {into};
                        made.push_back(o);
                    }
                    firstRead = false;
                    offset += n, left -= n, shift += 8 * n;
                }
            }
            for (size_t k = p; k < q; k++) gone.insert(flat[k]);
            instead[flat[p]] = made;
            p = q;
        }
        if (gone.empty()) return;
        // Each instruction's steps again: the reads where the first was, the rest gone.
        std::set<size_t> touched;
        for (const auto& g : gone) touched.insert(g.first);
        for (size_t k : touched) {
            std::vector<LOp> kept;
            for (size_t o = 0; o < results[k].ops.size(); o++) {
                const std::pair<size_t, size_t> here{k, o};
                if (auto put = instead.find(here); put != instead.end()) kept.insert(kept.end(), put->second.begin(), put->second.end());
                else if (!gone.count(here)) kept.push_back(results[k].ops[o]);
            }
            results[k].ops = std::move(kept);
        }
    }

    // ---- Neighbouring items read as one span ----

    // `val p = px[i]; val q = px[i + 1]` and then p's bytes and q's put together into one int,
    // q's above p's (2026-11-15, an Image's hashCode, two pixels a word): from a list looked at
    // once, item i a Ref in p's slot and item i + 1 one in q's, then (bytesAtOnce's) each read
    // whole, q's shifted up by its width and |'d in. Where items are 1, 2 or 4 bytes wide, that
    // is one read of 2, 4 or 8 bytes at item i (hidxp2h, hidxp2w, hidxp2i), both indexes checked
    // at once, made where p's Ref was; neither Ref is made. Only where nothing reads p's slot or
    // q's after: no step of the region but these names either, none is the VM's, and each way
    // out of the loop leaves both above the stack's top. Between the read and the last step it
    // stands for, nothing kept leaves for the VM (which would find p and q unmade): i + 1, made
    // for q's index, goes where nothing else reads it, and otherwise can no longer overflow (the
    // read found item i + 1). A read that fails leaves where p's Ref was made: the VM reads both.
    bool twoItemsAtOnce() {
        if (templates > 0) return false;
        for (size_t i = 0; i < ins.size(); i++)
            if (in[i].known && (results[i].island || results[i].ends)) return false;
        std::vector<int> into(ins.size(), 0);
        for (size_t i = 0; i < ins.size(); i++)
            if (in[i].known)
                for (const Edge& e : results[i].edges)
                    if (e.to >= 0 && static_cast<size_t>(e.to) < ins.size()) into[static_cast<size_t>(e.to)]++;
        auto plainSteps = [&](size_t k) {
            for (const LOp& l : results[k].ops)
                if (l.jumpTo >= 0 || l.jumpExit >= 0) return false;
            return true;
        };
        bool changed = false;
        for (size_t j = 0; j < ins.size();) {
            if (!in[j].known || !plainSteps(j)) {
                j++;
                continue;
            }
            size_t e = j;
            while (e + 1 < ins.size() && results[e].fallsThrough && results[e].fallExit < 0 && in[e + 1].known && into[e + 1] == 1 &&
                   !entries.count(static_cast<int>(e + 1)) && !resumed.count(static_cast<int>(e + 1)) && plainSteps(e + 1))
                e++;
            for (bool again = true; again;) {
                std::vector<std::pair<size_t, size_t>> flat;
                for (size_t k = j; k <= e; k++)
                    for (size_t o = 0; o < results[k].ops.size(); o++) flat.emplace_back(k, o);
                again = false;
                for (size_t p = 0; p < flat.size() && !again; p++)
                    if (twoItemsFrom(flat, p)) again = changed = true;
            }
            j = e + 1;
        }
        return changed;
    }
    // Whether step `l` may read or write slot `slot` in memory, in any of the ways a step names
    // one (a register is not its slot: loads and stores name the slot as `slot`).
    static bool names(const LOp& l, int slot) {
        const uint64_t w = static_cast<uint64_t>(l.slot2), s = static_cast<uint64_t>(slot);
        return l.slot == slot || l.slot2 == slot || (w & 0xFFFFFF) == s || ((w >> 8) & 0xFFFFFF) == s || (w & 0xFFFFFFFF) == s ||
               ((w >> 24) & 0xFFFF) == s;
    }
    // Whether step `l` reads or writes register `r`.
    static bool touches(const LOp& l, const Reg& r) {
        for (const auto* regs : {&l.named, &l.uses, &l.defs})
            if (std::find(regs->begin(), regs->end(), r) != regs->end()) return true;
        return false;
    }
    bool twoItemsFrom(const std::vector<std::pair<size_t, size_t>>& flat, size_t p) {
        auto at = [&](size_t x) -> LOp& { return results[flat[x].first].ops[flat[x].second]; };
        const LOp& first = at(p);
        if (first.stem != "hiref" || first.named.size() != 1 || first.named[0].cls != CI) return false;
        const Reg counter = first.named[0];
        const int64_t list = first.slot;
        const int pSlot = static_cast<int>(first.slot2);
        // The index may be a copy of the counter made just before (`px[i]` reads i pushed), and
        // i + 1 made from the counter itself.
        Reg alias = counter;
        for (size_t x = p; x-- > 0;) {
            const LOp& l = at(x);
            if (std::find(l.defs.begin(), l.defs.end(), counter) == l.defs.end()) continue;
            if (l.stem == "movi" && l.named.size() == 2 && l.named[0] == counter && l.named[1].cls == CI) {
                alias = l.named[1];
                for (size_t y = x + 1; y < p; y++)
                    if (std::find(at(y).defs.begin(), at(y).defs.end(), alias) != at(y).defs.end()) alias = counter;
            }
            break;
        }
        // i + 1 into another register, then item i + 1's Ref (its index that register, or a copy
        // of it made after: `val j = i + 1; val q = px[j]`). Neither i nor the index read for p
        // written before i + 1 is made (after, they may be: `px[j]` pushes j where i was).
        size_t plus = 0, second = 0;
        std::optional<Reg> plusCopy;
        for (size_t x = p + 1; x < flat.size() && second == 0; x++) {
            const LOp& l = at(x);
            if (plus == 0) {
                if (l.stem == "addik" && l.ki == 1 && l.named.size() == 2 && (l.named[1] == counter || l.named[1] == alias)) {
                    plus = x;
                    continue;
                }
                const bool recopied = l.stem == "movi" && l.named.size() == 2 && l.named[0] == counter && l.named[1] == alias && !(alias == counter);
                for (const Reg& d : l.defs)
                    if (d == alias || (d == counter && !recopied)) return false;
                continue;
            }
            const Reg plusReg = at(plus).named[0];
            if (l.stem == "hiref" && l.slot == list && l.named.size() == 1 && (l.named[0] == plusReg || (plusCopy && l.named[0] == *plusCopy)) &&
                l.slot2 != pSlot) {
                second = x;
                continue;
            }
            for (const Reg& d : l.defs) {
                if (d == plusReg) return false;
                if (plusCopy && d == *plusCopy) plusCopy.reset();
            }
            if (!plusCopy && l.stem == "movi" && l.named.size() == 2 && l.named[1] == plusReg && l.named[0].cls == CI) plusCopy = l.named[0];
        }
        if (second == 0) return false;
        const int qSlot = static_cast<int>(at(second).slot2);
        const Reg plusReg = at(plus).named[0];
        // p's bytes read whole, then q's, shifted up by the width, |'d in.
        size_t read = 0;
        for (size_t x = second + 1; x + 3 < flat.size() && read == 0; x++)
            if (at(x).stem.rfind("hvalp", 0) == 0 && at(x).slot == pSlot) read = x;
        if (read == 0) return false;
        const LOp &rp = at(read), &rq = at(read + 1), &sh = at(read + 2), &orr = at(read + 3);
        auto sizeOf = [](const std::string& s) { return s == "hvalpb" ? 1 : s == "hvalph" ? 2 : s == "hvalpw" ? 4 : 0; };
        const int size = sizeOf(rp.stem);
        if (size == 0 || rq.stem != rp.stem || rq.slot != qSlot || rp.ki != list || rq.ki != list || rp.slot2 != rq.slot2 || rp.named.size() != 1 ||
            rq.named.size() != 1 || rp.named[0].cls != CI || rq.named[0].cls != CI || rp.named[0] == rq.named[0])
            return false;
        const uint64_t what = static_cast<uint64_t>(rp.slot2);
        if ((what & 0xFFFF) != 0 || ((what >> 16) & 0xFFFF) != static_cast<uint64_t>(size)) return false;  // the whole item, from its start
        const Reg into = rp.named[0], other = rq.named[0];
        if (sh.stem != "shlik" || sh.named.size() != 2 || !(sh.named[0] == other) || !(sh.named[1] == other) || sh.ki != 8 * size) return false;
        if (orr.stem != "ori" || orr.named.size() != 2 || !(orr.named[0] == into) || !(orr.named[1] == other)) return false;
        // The other register holds nothing wanted after: above the stack's top once these are done.
        const size_t lastIns = flat[read + 3].first;
        if (lastIns + 1 >= ins.size() || other.slot < in[lastIns + 1].top) return false;
        // Nothing else names p's slot or q's, anywhere in the region, nor on any way out.
        std::set<std::pair<size_t, size_t>> ours{flat[p], flat[second], flat[read], flat[read + 1]};
        std::set<std::pair<size_t, size_t>> keeps;
        for (size_t i = 0; i < ins.size(); i++) {
            if (!in[i].known) continue;
            for (size_t o = 0; o < results[i].ops.size(); o++) {
                const LOp& l = results[i].ops[o];
                if (ours.count({i, o}) || (!names(l, pSlot) && !names(l, qSlot))) continue;
                if (l.stem == "keepl" && (l.slot == pSlot || l.slot == qSlot)) {
                    keeps.insert({i, o});
                    continue;
                }
                return false;
            }
            for (const ExitPoint& x : results[i].exits) {
                for (LOp l : x.stores) {  // what it writes (`slot`) is the VM's value; what it reads
                    l.slot = -1;
                    if (names(l, pSlot) || names(l, qSlot)) return false;
                }
                if (!index.count(x.at) && x.state.top > std::min(pSlot, qSlot)) return false;  // out of the loop, p or q still there
            }
        }
        for (const LOp& l : prologue)
            if (names(l, pSlot) || names(l, qSlot)) return false;
        for (const auto& k : keeps)
            if (k < flat[p] || k > flat[read + 3]) return false;
        // Between the read (where p's Ref was) and the last step it stands for: nothing kept
        // leaves for the VM or touches the register read into; i + 1 dropped where nothing reads
        // it before it is written again, otherwise one that cannot overflow.
        bool plusDropped = false;
        for (size_t x = plus + 1; x < flat.size(); x++) {
            const LOp& l = at(x);
            if (x == second || keeps.count(flat[x])) continue;
            if (x == read) {
                plusDropped = plusReg == into;
                break;
            }
            if (std::find(l.uses.begin(), l.uses.end(), plusReg) != l.uses.end() || l.exit >= 0) break;
            if (std::find(l.defs.begin(), l.defs.end(), plusReg) != l.defs.end()) {
                plusDropped = true;
                break;
            }
        }
        if (!plusDropped && plusReg == into) return false;
        // Where something kept between uses the register read into (a slot of the stack used
        // again: `val j = i + 1; val q = px[j]`), the read goes into the scratch register, moved
        // across where the last step it stands for was.
        const Reg scratch{kScratchI, CI};
        bool viaScratch = false;
        for (size_t x = p + 1; x <= read + 3; x++) {
            if ((x == plus && plusDropped) || x == second || (x >= read && x <= read + 3) || keeps.count(flat[x])) continue;
            const LOp& l = at(x);
            if (x != plus && (l.exit >= 0 || l.jumpTo >= 0 || l.jumpExit >= 0)) return false;
            if (touches(l, scratch)) return false;
            if (touches(l, into)) viaScratch = true;
        }
        // Made: the read where p's Ref was.
        const Reg to = viaScratch ? scratch : into;
        LOp two = first;
        two.stem = size == 4 ? "hidxp2i" : size == 2 ? "hidxp2w" : "hidxp2h";
        two.named = {to, counter}, two.uses = {counter}, two.defs = {to};
        two.slot = list, two.slot2 = size, two.ki = 0;
        LOp moved;
        moved.stem = "movi", moved.named = {into, scratch}, moved.uses = {scratch}, moved.defs = {into};
        std::set<std::pair<size_t, size_t>> gone{flat[second], flat[read], flat[read + 1], flat[read + 2], flat[read + 3]};
        gone.insert(keeps.begin(), keeps.end());
        if (plusDropped) gone.insert(flat[plus]);
        else {
            LOp& l = at(plus);
            l.stem = "waddik", l.exit = -1;
        }
        std::set<size_t> touched{flat[p].first};
        for (const auto& g : gone) touched.insert(g.first);
        for (size_t k : touched) {
            std::vector<LOp> kept;
            for (size_t o = 0; o < results[k].ops.size(); o++) {
                const std::pair<size_t, size_t> here{k, o};
                if (here == flat[p]) kept.push_back(two);
                else if (here == flat[read + 3] && viaScratch) kept.push_back(moved);
                else if (!gone.count(here)) kept.push_back(results[k].ops[o]);
            }
            results[k].ops = std::move(kept);
        }
        return true;
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
        auto isRead = [](const std::string& stem) { return stem == "idxf" || stem == "idxfk" || stem == "idxw" || stem == "idxwk" || stem == "idxbool"; };
        auto isWrite = [](const std::string& stem) { return stem == "setf" || stem == "setwi" || stem == "setwb" || stem == "setbool"; };
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
                kindOf[root] |= l.stem == "idxbool" || l.stem == "setbool" ? 16 : isFloat(l.stem) ? 1 : 2;  // 16: bools (2026-11-15)
                steps.emplace_back(&l, root);
            }
        }
        // A list added to (push...) is not looked at once: its count changes as the region runs.
        // (Another slot holding the same list, looked at, sees fewer items than it has: those
        // added are read by the VM, as items out of range are; nothing moves them meanwhile, as
        // the region adds only where the list's block has room.)
        std::set<int> addedTo;
        for (size_t i = 0; i < ins.size(); i++) {
            if (!in[i].known) continue;
            for (const LOp& l : results[i].ops)
                if (l.stem.rfind("push", 0) == 0) addedTo.insert(rootOf(i, static_cast<int>(l.slot)));
        }
        // Never written in the region: as entered everywhere (a field's, as it is held).
        std::map<int, int> index;
        for (const auto& [root, kind] : kindOf) {
            if ((kind != 1 && kind != 2 && kind != 4 && kind != 8 && kind != 16) || (kind == 16 && root < 0) || static_cast<int>(index.size()) == kRegionLists) continue;
            if (addedTo.count(root) != 0) continue;
            if (kind == 8 && root < 0) continue;
            const bool items = kind == 4 || kind == 8;  // not 16, a list of bools
            const ObjClass* const cls = items ? itemClass[root] : nullptr;
            const ValueLayout* const layout = cls != nullptr ? cls->layout.load(std::memory_order_acquire) : nullptr;
            if (items && (layout == nullptr || layout->size == 0 || layout->size > 0xFFFF)) continue;
            if (root >= 0 && !untouched(root)) continue;
            const int k = static_cast<int>(index.size());
            index[root] = k;
            LOp look;
            look.stem = kind == 1 ? "lookf" : kind == 2 ? "lookw" : kind == 4 ? "looki" : kind == 16 ? "lookbool" : "lookit";
            if (root < 0) look.stem += "h";  // from a field held as -2 - root
            look.slot = root >= 0 ? root : -2 - root;
            look.ki = k;
            if (items) {
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
        // Held: the int field held for the region as ki, the same each time round. Field: a field of
        // an item of the list of items looked at once as `list`, the item at index b; ki the
        // field's width (high half), offset (bits 8 on) and size (1 a byte, 8 an int).
        // Unknown: a register set on one way through the body alone, which nothing may read after.
        // ToFloat, ToInt: an int as a float (cvtif), a float cut to a whole number (cvtfi).
        // Floor: a float floored to a whole number (floori), an int. Abs: an int's size (absi).
        enum Kind { Counter, Inv, KInt, KFloat, Load, Bin, Min, Max, ToByte, Held, Field, Unknown, ToFloat, ToInt, Floor, Abs } kind;
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
    int loopTop = 0;  // the VM's top as the loop being recognised starts
    const Expr* mk(Expr e) {
        exprs.push_back(e);
        return &exprs.back();
    }
    const Expr* kfloatOf(double v) {
        Expr e{Expr::KFloat};
        e.cls = CF, e.kf = v;
        return mk(e);
    }
    const Expr* kintOf(int64_t v) {
        Expr e{Expr::KInt};
        e.ki = v;
        return mk(e);
    }
    static bool same(const Expr* x, const Expr* y) {
        if (x == y) return true;
        if (x == nullptr || y == nullptr || x->kind != y->kind || x->cls != y->cls) return false;
        switch (x->kind) {
            case Expr::Counter: return true;
            case Expr::Inv: return x->reg == y->reg;
            case Expr::KInt: return x->ki == y->ki;
            case Expr::Held: return x->ki == y->ki;
            case Expr::Field: return x->list == y->list && x->ki == y->ki && same(x->b, y->b);
            case Expr::KFloat: return std::memcmp(&x->kf, &y->kf, 8) == 0;
            case Expr::Load: return x->list == y->list && same(x->b, y->b);
            default: return x->op == y->op && same(x->a, y->a) && same(x->b, y->b);
        }
    }
    // Whether an expression is the same each time round: no counter, no item in it.
    static bool invariant(const Expr* e) {
        if (e == nullptr) return false;
        if (e->kind == Expr::Counter || e->kind == Expr::Load || e->kind == Expr::Field) return false;
        return (e->a == nullptr || invariant(e->a)) && (e->b == nullptr || invariant(e->b));
    }
    static bool usesReg(const Expr* e, const Reg& r) {
        if (e == nullptr) return false;
        if (e->kind == Expr::Inv && e->reg == r) return true;
        return usesReg(e->a, r) || usesReg(e->b, r);
    }
    static bool itemAtCounter(const Expr* e, Cls cls) { return e != nullptr && e->kind == Expr::Load && e->cls == cls && e->b != nullptr && e->b->kind == Expr::Counter; }
    // A guard's side: the counter plus a constant, a constant, a register the loop does not change
    // or an int field held for the region.
    static bool guardSide(const Expr* e) {
        if (e == nullptr || e->cls != CI) return false;
        if (e->kind == Expr::Counter || e->kind == Expr::KInt || e->kind == Expr::Inv || e->kind == Expr::Held) return true;
        if (e->kind != Expr::Bin || e->op != '+') return false;
        const Expr* k = e->a->kind == Expr::KInt ? e->a : e->b->kind == Expr::KInt ? e->b : nullptr;
        const Expr* x = k == e->a ? e->b : e->a;
        if (k == nullptr || k->ki < -(int64_t{1} << 40) || k->ki > (int64_t{1} << 40)) return false;
        return x->kind == Expr::Counter || x->kind == Expr::Inv || (x->kind == Expr::Held && x->op != 'b');
    }
    // An operand of a template's, the same each time round: a constant, a register the loop does
    // not change, or an int field held for the region.
    static bool fixedOperand(const Expr* e) {
        return e != nullptr && e->cls == CI && (e->kind == Expr::KInt || e->kind == Expr::Inv || e->kind == Expr::Held);
    }
    // An index of the counter times a constant of 1 or more, plus a constant of 0 or more: its
    // stride and offset.
    static bool strided(const Expr* e, int64_t& stride, int64_t& offset) {
        if (e == nullptr || e->cls != CI) return false;
        auto times = [&](const Expr* m) {
            if (m->kind == Expr::Counter) return stride = 1, true;
            if (m->kind != Expr::Bin || m->op != '*' || m->cls != CI) return false;
            const Expr* k = m->a->kind == Expr::KInt ? m->a : m->b;
            const Expr* c = m->a->kind == Expr::KInt ? m->b : m->a;
            if (k->kind != Expr::KInt || c->kind != Expr::Counter || k->ki < 1) return false;
            return stride = k->ki, true;
        };
        offset = 0;
        if (e->kind == Expr::Bin && e->op == '+') {
            const Expr* k = e->a->kind == Expr::KInt ? e->a : e->b;
            const Expr* m = e->a->kind == Expr::KInt ? e->b : e->a;
            if (k->kind != Expr::KInt || k->ki < 0 || !times(m)) return false;
            offset = k->ki;
            return true;
        }
        return times(e);
    }

    // The body's steps, followed: each register's value; the stores (list, index, value, the
    // step's stem); false where a step is one this does not follow.
    struct Followed {
        std::map<Reg, const Expr*> val;
        struct Store {
            int list;
            const Expr* index;
            const Expr* value;
            std::string stem;
            int64_t what = 0, width = 0, source = -1;  // hsetsm: what it is given, the items' width, the value's slot
            int64_t cls = 0;                           // pushsm: the items' class (`list` the slot of the list added to)
        };
        std::vector<Store> stores;
        // A guard's comparison (2026-11-13): the last one followed, for a jump on its flag.
        struct Cmp {
            int op = 0;  // 1 <, 2 <=, 3 >, 4 >=, 5 ==, 6 !=; 7 and 8: itemCmp's two items the same, not
            const Expr* a = nullptr;
            const Expr* b = nullptr;
        };
        Cmp last;
        // The two items an eqsm compares (2026-11-14): each list looked at once and the index, and
        // how many bytes.
        struct ItemCmp {
            int listA = -1, listB = -1;
            const Expr* indexA = nullptr;
            const Expr* indexB = nullptr;
            int64_t size = 0;
        };
        ItemCmp itemCmp;
        // Items in slots: a Ref (hiref) or a copy (hidxsm, and copyslot of either), by the list,
        // the index and the item's width where known; and the slots the body writes values into,
        // beside its registers (each a temporary of the VM's stack, or a variable of the body's).
        struct Ref {
            int list;
            const Expr* index;
            int64_t width;
            int64_t what = 0;  // hidxsm: what it is given (the bytes read, the value's type number)
        };
        std::map<int64_t, Ref> refs;
        std::vector<int64_t> slots;
        // Slots holding a field held for the region (hgetpv), by the field; slots holding a value the
        // same each time round: a slot as the loop entered it (1) or a captured variable (2).
        std::map<int64_t, int64_t> heldCopies;
        struct Value {
            int kind;
            int64_t at;
            int64_t cls = 0;  // the class an ownsm on it was given (a value in a place of that class taken too)
        };
        std::map<int64_t, Value> values;
        // Held int fields the body writes (hsetpi), each the value it last writes.
        std::map<int64_t, const Expr*> heldWrites;
        // Slots a global was read into (getg), by the global; and values of a value type made
        // from their bytes into slots (mksm): the bytes, the type's number, the class and the
        // global it was checked against.
        std::map<int64_t, int64_t> globals;
        struct Made {
            const Expr* bytes;
            int64_t id, cls, global;
        };
        std::map<int64_t, Made> made;
        // Places made at items to be set (hplacei: `to[i] = Color(...)`, the value made where the
        // item is): the list and the index, each the item a store of the body's sets.
        std::vector<std::pair<int, const Expr*>> places;
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
        if (s == "mini" || s == "maxi" || s == "minf" || s == "maxf") {
            Expr e{s[1] == 'i' ? Expr::Min : Expr::Max};
            e.cls = s.back() == 'f' ? CF : CI;
            e.a = valueOf(fw, n[0]), e.b = valueOf(fw, n[1]);
            return set(n[0], mk(e)), true;
        }
        if (s == "cvtif" || s == "cvtfi") {  // d = s as a float, or cut to a whole number
            Expr e{s == "cvtif" ? Expr::ToFloat : Expr::ToInt};
            e.cls = s == "cvtif" ? CF : CI;
            e.a = valueOf(fw, n[1]);
            return set(n[0], mk(e)), true;
        }
        if (s == "floori") {  // d = s floored, an int (2026-11-15: bilinear sampling's Math.floor)
            Expr e{Expr::Floor};
            e.a = valueOf(fw, n[1]);
            return set(n[0], mk(e)), true;
        }
        if (s == "absi") {  // an int's size, in place (2026-11-15: how far apart two pixels are)
            Expr e{Expr::Abs};
            e.a = valueOf(fw, n[0]);
            return set(n[0], mk(e)), true;
        }
        // A comparison of ints: its flag, for a guard's jump.
        if ((s.size() == 4 || s.size() == 5) && (s[2] == 'i')) {
            static const char* const ops[] = {"lt", "le", "gt", "ge", "eq", "ne"};
            for (int k = 0; k < 6; k++)
                if (s.compare(0, 2, ops[k]) == 0 && (s.substr(2) == "ii" || s.substr(2) == "ik")) {
                    fw.last.op = k + 1;
                    fw.last.a = valueOf(fw, n[0]);
                    fw.last.b = s.substr(2) == "ik" ? kint(l.ki) : valueOf(fw, n[1]);
                    return true;
                }
        }
        if (s == "hgetpi") {
            if (fw.heldWrites.count(l.slot) != 0) return false;  // read after the body wrote it
            Expr e{Expr::Held};
            e.ki = l.slot;
            return set(n[0], mk(e)), true;
        }
        if (s == "hgetpv") {
            fw.heldCopies[l.slot2] = l.slot;
            fw.slots.push_back(l.slot2);
            return true;
        }
        // A held bool field read into the flag (Grid's writable, say): a guard's comparison.
        if (s == "loadflag") {
            auto held = fw.heldCopies.find(l.slot);
            if (held == fw.heldCopies.end()) return false;
            Expr e{Expr::Held};
            e.ki = held->second;
            e.op = 'b';
            fw.last.op = 6;  // != 0
            fw.last.a = mk(e);
            fw.last.b = kint(0);
            return true;
        }
        if (s == "eqsm") {  // two items compared (kLoopWSameItems)
            auto x = fw.refs.find(l.slot), y = fw.refs.find(l.slot2);
            if (x == fw.refs.end() || y == fw.refs.end() || x->second.list < 0 || y->second.list < 0) return false;
            uint64_t what;
            std::memcpy(&what, &l.kf, 8);
            fw.itemCmp = Followed::ItemCmp{x->second.list, y->second.list, x->second.index, y->second.index, static_cast<int64_t>(what & 0xFF)};
            fw.last = Followed::Cmp{7, nullptr, nullptr};
            return true;
        }
        if (s == "notflag") {
            static const int turned[] = {0, 4, 3, 2, 1, 6, 5, 8, 7};  // < >=, <= >, > <=, >= <, == !=, != ==, same not
            if (fw.last.op == 0) return false;
            fw.last.op = turned[fw.last.op];
            return true;
        }
        if (s == "getuv") {
            fw.values[l.slot2] = Followed::Value{2, l.ki};
            fw.slots.push_back(l.slot2);
            return true;
        }
        if (s == "hsetpi") {
            fw.heldWrites[l.slot] = valueOf(fw, n[0]);
            return true;
        }
        if (s == "getg") {
            fw.globals[l.slot2] = l.ki;
            fw.slots.push_back(l.slot2);
            return true;
        }
        if (s == "packb") {  // a byte into a value's bytes, ki bits up
            Expr e{Expr::Bin};
            e.op = 'p';
            e.a = valueOf(fw, n[0]);
            e.b = valueOf(fw, n[1]);
            e.ki = l.ki;
            return set(n[0], mk(e)), true;
        }
        if (s == "mksm") {
            auto g = fw.globals.find(l.slot);
            if (g == fw.globals.end()) return false;
            const int64_t to = static_cast<int64_t>(static_cast<uint64_t>(l.slot2) & 0xFFFFFFFF);
            fw.made[to] = Followed::Made{valueOf(fw, n[0]), static_cast<int64_t>(static_cast<uint64_t>(l.slot2) >> 32), l.ki, g->second};
            fw.slots.push_back(to);
            return true;
        }
        if (s == "hsetsm") {
            const int64_t from = l.slot2 & 0xFFFFFF;
            if (fw.values.count(from) == 0 && fw.made.count(from) == 0 && fw.refs.count(from) == 0) return false;
            Followed::Store st{static_cast<int>(l.slot), valueOf(fw, n[0]), nullptr, s};
            st.what = l.slot2, st.width = l.ki, st.source = from;
            fw.stores.push_back(st);
            return true;
        }
        if (s == "pushsm") {
            // An item of another list added at the end of the list in slot l.slot (2026-11-15:
            // to.add(from[i])); the true it gives written to a slot of the stack's.
            const int64_t from = l.slot2 & 0xFFFFFF;
            if (fw.refs.count(from) == 0) return false;
            Followed::Store st{static_cast<int>(l.slot), nullptr, nullptr, s};
            st.what = l.slot2, st.source = from, st.cls = l.ki;
            fw.stores.push_back(st);
            fw.slots.push_back((l.slot2 >> 24) & 0xFFFF);
            return true;
        }
        if (s == "hiref") {
            fw.refs[l.slot2] = Followed::Ref{static_cast<int>(l.slot), valueOf(fw, n[0]), 0};
            fw.slots.push_back(l.slot2);
            return true;
        }
        if (s == "hidxsm") {  // an item copied out into a slot: its fields as an item's (2026-11-13)
            const int64_t to = (l.slot2 >> 8) & 0xFFFFFF;
            fw.refs[to] = Followed::Ref{static_cast<int>(l.slot), valueOf(fw, n[0]), l.ki, l.slot2};
            fw.slots.push_back(to);
            return true;
        }
        if (s == "hplacei") {
            // Its check of the index is the store's at the same item, which recognise asks for.
            fw.places.emplace_back(static_cast<int>(l.slot), valueOf(fw, n[0]));
            fw.slots.push_back(l.slot2);
            return true;
        }
        if (s == "copyslot") {
            if (auto m = fw.made.find(l.slot2); m != fw.made.end()) {  // a value made, moved down
                const Followed::Made copied = m->second;
                fw.made[l.slot] = copied;
                fw.slots.push_back(l.slot);
                return true;
            }
            auto value = fw.values.find(l.slot2);
            if (value != fw.values.end()) {
                const Followed::Value v = value->second;
                fw.values[l.slot] = v;
                fw.slots.push_back(l.slot);
                return true;
            }
            if (fw.refs.count(l.slot2) == 0 && l.slot2 < loopTop) {  // a slot as the loop entered it
                fw.values[l.slot] = Followed::Value{1, l.slot2};
                fw.slots.push_back(l.slot);
                return true;
            }
            auto from = fw.refs.find(l.slot2);
            if (from == fw.refs.end()) return false;
            const Followed::Ref copied = from->second;
            fw.refs[l.slot] = copied;
            fw.slots.push_back(l.slot);
            return true;
        }
        // On an item in a slot: nothing to the item (it is no place of the frame's, never null).
        if (s == "ownsm") {
            auto value = fw.values.find(l.slot);
            if (value != fw.values.end()) value->second.cls = l.ki;
        }
        if (s == "keepl" || s == "ownsm" || s == "notnull") return fw.refs.count(l.slot) != 0 || fw.values.count(l.slot) != 0 || fw.made.count(l.slot) != 0;
        if (s == "hidxpb" || s == "hidxpi") {
            // `items[i].x` read where it is, a byte or an int field (2026-11-15: a histogram's
            // `px[p].red`): the field of item n[1] of the list looked at once as k, at
            // (slot2 & 0xFFFFFFFF) bytes into an item (slot2 >> 32) bytes wide.
            const uint64_t what = static_cast<uint64_t>(l.slot2);
            if ((what & 0xFFFFFFFF) > 0xFFFFFF || (what >> 32) > 0xFFFF) return false;
            Expr e{Expr::Field};
            e.list = static_cast<int>(l.slot);
            e.b = valueOf(fw, n[1]);
            e.ki = static_cast<int64_t>((what >> 32) << 32 | (what & 0xFFFFFFFF) << 8 | (s == "hidxpb" ? 1u : 8u));
            return set(n[0], mk(e)), true;
        }
        if (s == "hvalpb" || s == "hvalpi" || s == "hvalph" || s == "hvalpw") {
            auto ref = fw.refs.find(l.slot);
            if (ref == fw.refs.end()) return false;
            if (ref->second.list == -2) ref->second.list = static_cast<int>(l.ki);  // the iterated list, looked at once as k
            if (ref->second.list != l.ki) return false;
            const uint64_t what = static_cast<uint64_t>(l.slot2);
            Expr e{Expr::Field};
            e.list = ref->second.list;
            e.b = ref->second.index;
            e.ki = static_cast<int64_t>(((what >> 16) & 0xFFFF) << 32 | (what & 0xFFFF) << 8 |
                                        (s == "hvalpb" ? 1u : s == "hvalph" ? 2u : s == "hvalpw" ? 4u : 8u));
            return set(n[0], mk(e)), true;
        }
        if (s == "valpb" || s == "valpi" || s == "valph" || s == "valpw") {
            auto ref = fw.refs.find(l.slot);
            const uint64_t offset = static_cast<uint64_t>(l.slot2) & 0xFFFFFFFF;
            if (ref == fw.refs.end() || ref->second.width <= 0 || ref->second.width > 0xFFFF || offset >= 0xFFFFFF) return false;
            Expr e{Expr::Field};
            e.list = ref->second.list;
            e.b = ref->second.index;
            e.ki = static_cast<int64_t>(static_cast<uint64_t>(ref->second.width) << 32 | offset << 8 |
                                        (s == "valpb" ? 1u : s == "valph" ? 2u : s == "valpw" ? 4u : 8u));
            return set(n[0], mk(e)), true;
        }
        if (s == "divik" && l.ki > 0) return set(n[0], bin('/', CI, valueOf(fw, n[1]), kint(l.ki))), true;
        if (s == "shrik") {
            Expr e{Expr::Bin};
            e.cls = CI, e.op = '>', e.a = valueOf(fw, n[1]), e.b = kint(l.ki);
            return set(n[0], mk(e)), true;
        }
        // A shift left by a constant ('<') and an or ('|') of two ints (2026-11-15: a palette's
        // key, (r >> 3) << 10 | (g >> 3) << 5 | b >> 3, for kLoopWCountItems alone).
        if (s == "shlik" && l.ki >= 0 && l.ki < 64) {
            Expr e{Expr::Bin};
            e.cls = CI, e.op = '<', e.a = valueOf(fw, n[1]), e.b = kint(l.ki);
            return set(n[0], mk(e)), true;
        }
        if (s == "ori") {
            Expr e{Expr::Bin};
            e.cls = CI, e.op = '|', e.a = valueOf(fw, n[0]), e.b = valueOf(fw, n[1]);
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
        int checkSlot2 = -1, checkKind2 = 0;  // and a second's
        std::vector<Followed::Cmp> guards;  // conditions of ways to a throw or a return, none to hold
        int baseMode = 0;                   // kLoopWSumField: base + counter, base none (0), a (1), a * b (2)
        const Expr* baseA = nullptr;
        const Expr* baseB = nullptr;
        std::vector<Followed::Cmp> skips;  // kLoopWCountItems: an item skipped where its comparison holds (one at most)
        int outMode = 0;                    // kLoopWMixItems into another list: where it writes, as baseMode (words 56 to 60)
        const Expr* outA = nullptr;
        const Expr* outB = nullptr;
        std::vector<int64_t> fields;  // kLoopWSumField: each field's width, offset and size
        int64_t iterSlot = -1;        // a for over a list of items: the iterator's slot
        struct Update {
            int64_t field = 0;
            int how = 0;  // 1 min, 2 max, 3 a constant added
            const Expr* with = nullptr;
            int64_t constant = 0;
        };
        std::vector<Update> updates;  // held int fields the loop changes
        std::vector<int64_t> fill;    // kLoopWFillItems: the items' width, where the value is (kind, which), what hsetsm is given
        std::vector<int64_t> mixWords;  // kLoopWMixItems: words 96 on
        int64_t mixDivisor = 1;         // kLoopWMix: words 61 and 62
        const Expr* fromBase = nullptr;  // kLoopWCopyItems: where the items read start (baseA: where those written do)
        int yMode = 0;                   // kLoopWSampleBilinear: where the y places are read, as baseMode (words 63 to 67)
        const Expr* yA = nullptr;
        const Expr* yB = nullptr;
        const Expr* sizeW = nullptr;     // kLoopWSampleBilinear: the picture's width and height (words 130 to 133)
        const Expr* sizeH = nullptr;
        // kLoopFMulAdd4: the four reads' bases and constants, and their weights.
        const Expr* fourBase[4] = {nullptr, nullptr, nullptr, nullptr};
        int64_t fourK[4] = {0, 0, 0, 0};
        const Expr* fourWeight[4] = {nullptr, nullptr, nullptr, nullptr};
        // kLoopWSameItems: the two items compared, the template stopping before the first that
        // differ (the loop's own steps go on from there, into the way out they fall into).
        Followed::ItemCmp stopAt;
        // kLoopWFindWord (2026-11-15): items, a word each, compared with one word until one is not
        // it; the list, the item's index and the word.
        int stopList = -1;
        const Expr* stopIndex = nullptr;
        const Expr* stopWord = nullptr;
        int64_t copyWhat[3] = {0, 0, 0};  // kLoopWCopyItems: the items' width, what hidxsm and hsetsm are given
        bool mixDivideFirst = false;
        // kLoopWMixItems' float mixes (kinds 0x60 + j): each lane's terms (the byte's offset and the
        // weight), its constant (or none), and the bounds it is held between; words 128 on.
        struct FloatLane {
            std::vector<std::pair<int64_t, const Expr*>> terms;
            const Expr* c = nullptr;
            double lo = 0, hi = 0;
        };
        std::vector<FloatLane> floatLanes;
        // Objects as the loop entered them, each to be of exactly a class (clsflag, the way where
        // it is not out of the region): the slot and the global holding the class; words 112 on.
        std::vector<std::pair<int64_t, int64_t>> classChecks;
    };
    // An index as base + coef * counter + k: base null or an operand the same each time round.
    struct Lin {
        const Expr* base = nullptr;
        int64_t coef = 0, k = 0;
    };
    template <typename Changed, typename Written>
    static bool linear(const Expr* e, Lin& out, int64_t scale, const Changed& changed, const Written& written) {
        if (e == nullptr || e->cls != CI || scale < -64 || scale > 64) return false;
        if (e->kind == Expr::Counter) return out.coef += scale, true;
        if (e->kind == Expr::KInt) {
            if (e->ki < -(int64_t{1} << 40) || e->ki > (int64_t{1} << 40)) return false;
            return out.k += e->ki * scale, true;
        }
        if (e->kind == Expr::Bin && e->op == '+') return linear(e->a, out, scale, changed, written) && linear(e->b, out, scale, changed, written);
        if (e->kind == Expr::Bin && e->op == '*') {
            if (e->a->kind == Expr::KInt && e->a->ki >= 1 && e->a->ki <= 64) return linear(e->b, out, scale * e->a->ki, changed, written);
            if (e->b->kind == Expr::KInt && e->b->ki >= 1 && e->b->ki <= 64) return linear(e->a, out, scale * e->b->ki, changed, written);
            return false;
        }
        if (scale == 1 && out.base == nullptr && fixedOperand(e) && !changed(e) && !written(e)) return out.base = e, true;
        return false;
    }
    static bool kf(const Expr* e, double v) { return e != nullptr && e->kind == Expr::KFloat && e->kf == v; }
    // `min(hi, max(lo, x))` as WinterWolf writes Math.min(hi, Math.max(lo, x)), either way round
    // each: x, or null.
    static const Expr* held(const Expr* e, double lo, double hi) {
        if (e == nullptr || e->kind != Expr::Min || e->cls != CF) return nullptr;
        const Expr* inner = kf(e->a, hi) ? e->b : kf(e->b, hi) ? e->a : nullptr;
        if (inner == nullptr || inner->kind != Expr::Max || inner->cls != CF) return nullptr;
        return kf(inner->a, lo) ? inner->b : kf(inner->b, lo) ? inner->a : nullptr;
    }
    // x + c (either way round): x, or null.
    static const Expr* plus(const Expr* e, double c) {
        if (e == nullptr || e->kind != Expr::Bin || e->cls != CF || e->op != '+') return nullptr;
        return kf(e->a, c) ? e->b : kf(e->b, c) ? e->a : nullptr;
    }
    // A colour's part as numeric.ww's __colorPart makes it from v: Math.min(255.0, Math.max(0.0,
    // v + 0.5)).toInt().toByte(): v, or null.
    static const Expr* colorPart(const Expr* e) {
        if (e == nullptr || e->kind != Expr::ToByte || e->a == nullptr || e->a->kind != Expr::ToInt) return nullptr;
        return plus(held(e->a->a, 0.0, 255.0), 0.5);
    }
    // Four floats a pixel each plus a gathered float times a weight (kLoopFGather4, 2026-11-15:
    // numeric.ww's __gatherMulAdd, scaling's across pass, a place of taps for every pixel):
    // `out[o + p] = out[o + p] + line[c + p] * w` for p from 0 to 3, o the out base plus four
    // times the counter, c four times the int at an index list's base plus the counter, w the
    // float at a weight list's same place. Words 50 to 52 the out base (50: 0 none, 1 an
    // operand), 63 to 65 the base of the index and the weight lists, 66 the constant added there,
    // 129 the weight list (looked at once).
    template <typename Changed, typename Written>
    bool gather4(const Followed& fw, Ahead& ah, const Changed& changed, const Written& written) {
        if (fw.stores.size() != 4) return false;
        int out = -1, line = -1, ilist = -1, wlist = -1;
        const Expr* c = nullptr;
        const Expr* w = nullptr;
        Lin outAt;
        for (int p = 0; p < 4; p++) {
            const Followed::Store& st = fw.stores[static_cast<size_t>(p)];
            if (st.stem != "hsetf" || st.index == nullptr || st.value == nullptr) return false;
            Lin o;
            if (!linear(st.index, o, 1, changed, written) || o.coef != 4 || o.k != p) return false;
            if (p == 0) out = st.list, outAt = o;
            else if (st.list != out || !same(o.base, outAt.base)) return false;
            const Expr* v = st.value;
            if (v->kind != Expr::Bin || v->op != '+' || v->cls != CF) return false;
            const Expr* old = v->a->kind == Expr::Load ? v->a : v->b;
            const Expr* mul = old == v->a ? v->b : v->a;
            if (old->kind != Expr::Load || old->list != out || old->cls != CF || !same(old->b, st.index)) return false;
            if (mul->kind != Expr::Bin || mul->op != '*' || mul->cls != CF) return false;
            const Expr* got = mul->a;
            const Expr* wt = mul->b;
            if (got->kind != Expr::Load || got->cls != CF || wt->kind != Expr::Load || wt->cls != CF) return false;
            if (p == 0) line = got->list, w = wt;
            else if (got->list != line || !same(wt, w)) return false;
            // The gathered index: c (four times the int read) for p = 0, c + p after.
            const Expr* at = got->b;
            if (p > 0) {
                if (at->kind != Expr::Bin || at->op != '+' || at->cls != CI) return false;
                const Expr* k = at->a->kind == Expr::KInt ? at->a : at->b;
                if (k->kind != Expr::KInt || k->ki != p) return false;
                at = k == at->a ? at->b : at->a;
            }
            if (p == 0) c = at;
            else if (!same(at, c)) return false;
        }
        // c = the int at the index list's place, times 4; w the float at the weight list's same place.
        if (c->kind != Expr::Bin || c->op != '*' || c->cls != CI) return false;
        const Expr* four = c->a->kind == Expr::KInt ? c->a : c->b;
        const Expr* idx = four == c->a ? c->b : c->a;
        if (four->kind != Expr::KInt || four->ki != 4 || idx->kind != Expr::Load || idx->cls != CI) return false;
        ilist = idx->list, wlist = w->list;
        Lin ii, wi;
        if (!linear(idx->b, ii, 1, changed, written) || !linear(w->b, wi, 1, changed, written) || ii.coef != 1 || wi.coef != 1 || ii.k != wi.k ||
            !same(ii.base, wi.base))
            return false;
        if (out < 0 || line < 0 || ilist < 0 || wlist < 0 || out == line || out == ilist || out == wlist) return false;
        ah.shape = 27;  // kLoopFGather4
        ah.out = out, ah.a = line, ah.b = ilist;
        ah.baseA = outAt.base, ah.fromBase = ii.base;
        ah.copyWhat[0] = ii.k, ah.copyWhat[1] = wlist;
        return true;
    }
    // Colours made four floats a pixel and back (2026-11-15: numeric.ww's __premultiplied and
    // __unpremultiplied, which blurred, scaled and a colour grid's convolve work through): each
    // matched to the expressions those loops are, every operation in the same order.
    //
    // kLoopWToFloats: items of four bytes (a colour) at a base plus the counter, into a list of
    // floats at a base plus four times the counter: the first three bytes each times the fourth
    // over 255.0, then the fourth. Words 50 to 53 the floats' base (50: 0 none, 1 an operand; 53 a
    // constant added), 63 to 66 the items'.
    //
    // kLoopWFromFloats: four floats a pixel into items of a value type of four bytes (a colour)
    // made from them: a = the fourth held from 0 to 255, m = min(1, (a + 0.5) cut) times 255.0
    // over max(a, 0.5), each of the first three bytes the part of a float times m, the fourth
    // the part of a (__colorPart). Words 50 to 53 the items' base, 63 to 66 the floats', 97 the
    // global holding the type and 98 the type (the class mksm checks).
    template <typename Changed, typename Written>
    bool colorFloats(const Followed& fw, Ahead& ah, const Changed& changed, const Written& written) {
        if (ah.iterSlot >= 0) return false;
        auto byteField = [&](const Expr* e, int64_t offset) {
            return e != nullptr && e->kind == Expr::Field && (e->ki & 0xFF) == 1 && ((e->ki >> 8) & 0xFFFFFF) == offset && (e->ki >> 32) == 4;
        };
        if (fw.stores.size() == 4) {
            const Followed::Store* by[4] = {nullptr, nullptr, nullptr, nullptr};
            Lin out[4];
            for (int q = 0; q < 4; q++) {
                const Followed::Store& st = fw.stores[static_cast<size_t>(q)];
                if (st.stem != "hsetf" || st.list != fw.stores[0].list || !linear(st.index, out[q], 1, changed, written) || out[q].coef != 4) return false;
            }
            int64_t k0 = out[0].k;
            for (int q = 1; q < 4; q++) k0 = std::min(k0, out[q].k);
            for (int q = 0; q < 4; q++) {
                const int64_t at = out[q].k - k0;
                if (at < 0 || at > 3 || by[at] != nullptr || !same(out[q].base, out[0].base)) return false;
                by[at] = &fw.stores[static_cast<size_t>(q)];
            }
            const Expr* alpha = by[3]->value;
            if (alpha == nullptr || alpha->kind != Expr::ToFloat || !byteField(alpha->a, 3)) return false;
            const Expr* f3 = alpha->a;
            for (int q = 0; q < 3; q++) {
                const Expr* v = by[q]->value;
                if (v == nullptr || v->kind != Expr::Bin || v->cls != CF || v->op != '*') return false;
                auto part = [&](const Expr* e) {
                    return e->kind == Expr::ToFloat && byteField(e->a, q) && e->a->list == f3->list && same(e->a->b, f3->b);
                };
                auto factor = [&](const Expr* e) {
                    return e->kind == Expr::Bin && e->cls == CF && e->op == '/' && kf(e->b, 255.0) && e->a->kind == Expr::ToFloat && same(e->a->a, f3);
                };
                if (!((part(v->a) && factor(v->b)) || (part(v->b) && factor(v->a)))) return false;
            }
            Lin from;
            if (!linear(f3->b, from, 1, changed, written) || from.coef != 1) return false;
            ah.shape = 24;  // kLoopWToFloats
            ah.out = fw.stores[0].list, ah.a = f3->list;
            ah.baseA = out[0].base, ah.fromBase = from.base;
            ah.copyWhat[0] = k0, ah.copyWhat[1] = from.k;
            return true;
        }
        if (fw.stores.size() != 1 || fw.stores[0].stem != "hsetsm" || fw.made.count(fw.stores[0].source) == 0) return false;
        const Followed::Store& st = fw.stores[0];
        const Followed::Made& made = fw.made.at(st.source);
        if ((static_cast<uint64_t>(st.what) >> 32) != static_cast<uint64_t>(made.id) || ((static_cast<uint64_t>(st.what) >> 24) & 0xFF) != 4 || st.width != 4)
            return false;
        Lin out;
        if (!linear(st.index, out, 1, changed, written) || out.coef != 1) return false;
        const Expr* lanes[4] = {nullptr, nullptr, nullptr, nullptr};
        const Expr* v = made.bytes;
        while (v->kind == Expr::Bin && v->op == 'p') {
            if (v->ki % 8 != 0 || v->ki < 0 || v->ki > 24 || lanes[v->ki / 8] != nullptr) return false;
            lanes[v->ki / 8] = v->b;
            v = v->a;
        }
        if (v->kind != Expr::KInt || v->ki != 0) return false;
        for (const Expr* lane : lanes)
            if (lane == nullptr) return false;
        // The fourth: the float at k + 3 held from 0 to 255, its part.
        const Expr* a = colorPart(lanes[3]);
        const Expr* load3 = held(a, 0.0, 255.0);
        if (load3 == nullptr || load3->kind != Expr::Load || load3->cls != CF) return false;
        Lin from;
        if (!linear(load3->b, from, 1, changed, written) || from.coef != 4) return false;
        const int64_t k0 = from.k - 3;
        // m = min(1, (a + 0.5) cut) as a float, times 255.0 over max(a, 0.5).
        const Expr* m = nullptr;
        auto isM = [&](const Expr* e) {
            if (e == nullptr || e->kind != Expr::Bin || e->cls != CF || e->op != '*') return false;
            auto gate = [&](const Expr* g) {
                if (g->kind != Expr::ToFloat || g->a == nullptr || g->a->kind != Expr::Min || g->a->cls != CI) return false;
                const Expr* x = g->a->a->kind == Expr::KInt && g->a->a->ki == 1 ? g->a->b : g->a->b->kind == Expr::KInt && g->a->b->ki == 1 ? g->a->a : nullptr;
                return x != nullptr && x->kind == Expr::ToInt && same(plus(x->a, 0.5), a);
            };
            auto over = [&](const Expr* d) {
                if (d->kind != Expr::Bin || d->cls != CF || d->op != '/' || !kf(d->a, 255.0) || d->b->kind != Expr::Max) return false;
                const Expr* x = kf(d->b->a, 0.5) ? d->b->b : kf(d->b->b, 0.5) ? d->b->a : nullptr;
                return same(x, a);
            };
            return (gate(e->a) && over(e->b)) || (gate(e->b) && over(e->a));
        };
        for (int q = 0; q < 3; q++) {
            const Expr* x = colorPart(lanes[q]);
            if (x == nullptr || x->kind != Expr::Bin || x->cls != CF || x->op != '*') return false;
            const Expr* load = x->a->kind == Expr::Load ? x->a : x->b->kind == Expr::Load ? x->b : nullptr;
            const Expr* other = load == x->a ? x->b : x->a;
            Lin at;
            if (load == nullptr || load->list != load3->list || !linear(load->b, at, 1, changed, written) || at.coef != 4 || at.k != k0 + q ||
                !same(at.base, from.base))
                return false;
            if (m == nullptr) {
                if (!isM(other)) return false;
                m = other;
            } else if (!same(other, m)) {
                return false;
            }
        }
        ah.shape = 25;  // kLoopWFromFloats
        ah.out = st.list, ah.a = load3->list;
        ah.baseA = out.base, ah.fromBase = from.base;
        ah.copyWhat[0] = out.k, ah.copyWhat[1] = k0;
        ah.mixWords = {0, made.global, made.cls};
        return true;
    }
    // The normal blend of a row (kLoopWBlendNormal, 2026-11-15: numeric.ww's __blendNormalRow):
    // each backdrop pixel d (items of four bytes at a base plus the counter) set from source pixel
    // s (another list's, the same way), skipped where sa = u[s.alpha] * opacity is at most 0;
    // with ba = u[d.alpha] and ao = sa + ba (1 - sa), each part c of the three
    // held((sa ((1 - ba) u[s.c] + ba u[s.c]) + ba (1 - sa) u[d.c]) / ao), alpha held(ao), held(v)
    // = (min(1, max(0, v)) 255 + 0.5) cut: u a list of floats (the bytes over 255). Matched by
    // its expressions written out with each leaf named by what it is, sums and products and min
    // and max with their two sides in one order (each the same either way round: neither side is
    // ever NaN, opacity checked as it runs), against the same written out for that formula.
    // Words: 50 to 54 where the source is read, 56 to 60 where the backdrop is, 97 and 98 the
    // class Color(...) calls, 129 the opacity, 130 the table.
    bool blendNormal(const Followed& fw, Ahead& ah) {
        if (fw.stores.size() != 1 || fw.stores[0].stem != "hsetsm" || ah.skips.size() != 1) return false;
        const Followed::Store& st = fw.stores[0];
        auto madeAt = fw.made.find(st.source);
        if (madeAt == fw.made.end()) return false;
        const Followed::Made& made = madeAt->second;
        if ((static_cast<uint64_t>(st.what) >> 32) != static_cast<uint64_t>(made.id) || ((static_cast<uint64_t>(st.what) >> 24) & 0xFF) != 4 || st.width != 4)
            return false;
        // The leaves by what they are: S0 to S3 the source's bytes, D0 to D3 the backdrop's, P the
        // opacity, u[...] the table's floats.
        int source = -1, table = -1;
        const Expr* sourceIndex = nullptr;
        const Expr* opacity = nullptr;
        bool bad = false;
        auto fmt = [](double v) {
            char text[40];
            std::snprintf(text, sizeof text, "%.17g", v);
            return std::string(text);
        };
        auto bin = [](char op, std::string a, std::string b) {
            if ((op == '+' || op == '*') && b < a) std::swap(a, b);
            return "(" + a + op + b + ")";
        };
        auto two = [](const char* name, std::string a, std::string b) {
            if (b < a) std::swap(a, b);
            return std::string(name) + "(" + a + "," + b + ")";
        };
        std::function<std::string(const Expr*)> show = [&](const Expr* e) -> std::string {
            if (e == nullptr || bad) return bad = true, "";
            switch (e->kind) {
                case Expr::KFloat: return fmt(e->kf);
                case Expr::KInt: return std::to_string(e->ki);
                case Expr::Field: {
                    if ((e->ki & 0xFF) != 1 || (e->ki >> 32) != 4) return bad = true, "";
                    const int64_t offset = (e->ki >> 8) & 0xFFFFFF;
                    if (e->list == st.list && same(e->b, st.index)) return "D" + std::to_string(offset);
                    if (source < 0 && e->list != st.list) source = e->list, sourceIndex = e->b;
                    if (e->list == source && same(e->b, sourceIndex)) return "S" + std::to_string(offset);
                    return bad = true, "";
                }
                case Expr::Load: {
                    if (e->cls != CF) return bad = true, "";
                    if (table < 0) table = e->list;
                    if (e->list != table) return bad = true, "";
                    return "u[" + show(e->b) + "]";
                }
                case Expr::Inv: {
                    if (e->cls != CF) return bad = true, "";
                    if (opacity == nullptr) opacity = e;
                    if (!same(e, opacity)) return bad = true, "";
                    return "P";
                }
                case Expr::Bin: return bin(e->op, show(e->a), show(e->b));
                case Expr::Min: return two("min", show(e->a), show(e->b));
                case Expr::Max: return two("max", show(e->a), show(e->b));
                case Expr::ToByte: return "byte(" + show(e->a) + ")";
                case Expr::ToInt: return "int(" + show(e->a) + ")";
                case Expr::ToFloat: return "float(" + show(e->a) + ")";
                default: return bad = true, "";
            }
        };
        // The formula, written out the same way.
        const std::string one = fmt(1.0), zero = fmt(0.0);
        auto u = [](const std::string& x) { return "u[" + x + "]"; };
        const std::string sa = bin('*', u("S3"), "P"), ba = u("D3");
        const std::string ao = bin('+', sa, bin('*', ba, bin('-', one, sa)));
        auto held = [&](const std::string& v) {
            return "byte(int(" + bin('+', bin('*', two("min", one, two("max", zero, v)), fmt(255.0)), fmt(0.5)) + "))";
        };
        auto part = [&](int c) {
            const std::string cs = u("S" + std::to_string(c)), cb = u("D" + std::to_string(c));
            const std::string cr = bin('+', bin('*', bin('-', one, ba), cs), bin('*', ba, cs));
            return held(bin('/', bin('+', bin('*', sa, cr), bin('*', bin('*', ba, bin('-', one, sa)), cb)), ao));
        };
        // The four bytes, down to nothing (a value made from them alone).
        const Expr* lanes[4] = {nullptr, nullptr, nullptr, nullptr};
        const Expr* v = made.bytes;
        while (v->kind == Expr::Bin && v->op == 'p') {
            if (v->ki % 8 != 0 || v->ki < 0 || v->ki > 24 || lanes[v->ki / 8] != nullptr) return false;
            lanes[v->ki / 8] = v->b;
            v = v->a;
        }
        if (v->kind != Expr::KInt || v->ki != 0) return false;
        for (int c = 0; c < 4; c++)
            if (lanes[c] == nullptr || show(lanes[c]) != (c < 3 ? part(c) : held(ao)) || bad) return false;
        // The skip: sa <= 0.
        const Followed::Cmp& g = ah.skips[0];
        if (g.op != 2 || show(g.a) != sa || g.b == nullptr || g.b->kind != Expr::KFloat || g.b->kf != 0.0 || bad) return false;
        if (source < 0 || table < 0 || opacity == nullptr || source == st.list) return false;
        // Each at a base plus the counter.
        Ahead out;
        if (!fieldIndex(sourceIndex, ah) || !fieldIndex(st.index, out)) return false;
        ah.outMode = out.baseMode, ah.outA = out.baseA, ah.outB = out.baseB;
        ah.shape = 28;  // kLoopWBlendNormal
        ah.a = source, ah.out = st.list, ah.p1 = opacity;
        ah.mixWords = {made.global, made.cls, table};
        return true;
    }
    // A formula to match an expression against (2026-11-15, bilinear sampling): a node of two
    // sides (+ - * / and m min, M max), or of one (f as a float, i cut to an int, b as a byte, F
    // floored, a an int's size); an int or a float constant; a byte field of an item four bytes wide of a list
    // named so, at an index matching its one side; or any expression named so, the same each
    // place it is named. Sums, products, min and max match either way round (the same either way
    // round where no side is NaN: a template using one checks that as it runs).
    struct Pat {
        enum Kind { Any, Node, Int, Float, Field, Load } kind = Node;
        char op = 0;
        std::string name;  // Any, Field: the expression's or the list's name
        int64_t ki = 0;    // Int; Field: the byte's offset
        double kf = 0;
        std::shared_ptr<const Pat> a, b;
    };
    using PatP = std::shared_ptr<const Pat>;
    static PatP patAny(const char* name) {
        Pat p;
        p.kind = Pat::Any, p.name = name;
        return std::make_shared<const Pat>(p);
    }
    static PatP patInt(int64_t v) {
        Pat p;
        p.kind = Pat::Int, p.ki = v;
        return std::make_shared<const Pat>(p);
    }
    static PatP patFloat(double v) {
        Pat p;
        p.kind = Pat::Float, p.kf = v;
        return std::make_shared<const Pat>(p);
    }
    static PatP patOp(char op, PatP a, PatP b = nullptr) {
        Pat p;
        p.op = op, p.a = std::move(a), p.b = std::move(b);
        return std::make_shared<const Pat>(p);
    }
    static PatP patField(const char* list, PatP index, int64_t offset) {
        Pat p;
        p.kind = Pat::Field, p.name = list, p.a = std::move(index), p.ki = offset;
        return std::make_shared<const Pat>(p);
    }
    // An item of a list of floats (float) or of ints (!float) named so, at an index matching `index`.
    static PatP patLoad(const char* list, PatP index, bool isFloat) {
        Pat p;
        p.kind = Pat::Load, p.name = list, p.a = std::move(index), p.ki = isFloat ? 1 : 0;
        return std::make_shared<const Pat>(p);
    }
    struct Bound {
        std::map<std::string, const Expr*> exprs;
        std::map<std::string, int> lists;
        int64_t budget = 1000000;  // steps it may take, trying sides both ways round
    };
    static bool matches(const Pat& p, const Expr* e, Bound& b) {
        if (e == nullptr || --b.budget < 0) return false;
        switch (p.kind) {
            case Pat::Any: {
                auto at = b.exprs.find(p.name);
                if (at != b.exprs.end()) return same(at->second, e);
                b.exprs[p.name] = e;
                return true;
            }
            case Pat::Int: return e->kind == Expr::KInt && e->ki == p.ki;
            case Pat::Float: return e->kind == Expr::KFloat && std::memcmp(&e->kf, &p.kf, 8) == 0;
            case Pat::Field: {
                if (e->kind != Expr::Field || (e->ki & 0xFF) != 1 || (e->ki >> 32) != 4 || ((e->ki >> 8) & 0xFFFFFF) != p.ki) return false;
                auto at = b.lists.find(p.name);
                if (at != b.lists.end() && at->second != e->list) return false;
                b.lists[p.name] = e->list;
                return matches(*p.a, e->b, b);
            }
            case Pat::Load: {
                if (e->kind != Expr::Load || e->cls != (p.ki == 1 ? CF : CI)) return false;
                auto at = b.lists.find(p.name);
                if (at != b.lists.end() && at->second != e->list) return false;
                b.lists[p.name] = e->list;
                return matches(*p.a, e->b, b);
            }
            case Pat::Node: break;
        }
        switch (p.op) {
            case 'f': return e->kind == Expr::ToFloat && matches(*p.a, e->a, b);
            case 'i': return e->kind == Expr::ToInt && matches(*p.a, e->a, b);
            case 'b': return e->kind == Expr::ToByte && matches(*p.a, e->a, b);
            case 'F': return e->kind == Expr::Floor && matches(*p.a, e->a, b);
            case 'a': return e->kind == Expr::Abs && matches(*p.a, e->a, b);
            default: break;
        }
        const bool minMax = p.op == 'm' || p.op == 'M';
        if (minMax ? e->kind != (p.op == 'm' ? Expr::Min : Expr::Max) : (e->kind != Expr::Bin || e->op != p.op)) return false;
        Bound before = b;
        if (matches(*p.a, e->a, b) && matches(*p.b, e->b, b)) return true;
        if (p.op == '-' || p.op == '/') return false;
        const int64_t budget = b.budget;
        b = std::move(before);
        b.budget = budget;
        return matches(*p.a, e->b, b) && matches(*p.b, e->a, b);
    }
    // Bilinear sampling of a row (kLoopWSampleBilinear, 2026-11-15: image.ww's __sampleBilinear,
    // a transformed picture's pixels): with x and y floats of a list at a base plus the counter
    // (the item skipped where x is less than a constant), the four pixels round (x, y) of a
    // picture w by h, each place held to it, mixed by x's and y's parts past their floors, each
    // part times alpha / 255, then as image.ww's __premultipliedColor makes a colour of them, set
    // at a base plus the counter of another list. Words: 50 to 54 where x is read, 56 to 60
    // where the colour is set, 63 to 67 where y is read, 97 and 98 the class Color(...) calls,
    // 7 the skip's constant, 130 and 131 w, 132 and 133 h.
    bool sampleBilinear(const Followed& fw, Ahead& ah) {
        if (fw.stores.size() != 1 || fw.stores[0].stem != "hsetsm" || ah.skips.size() != 1) return false;
        const Followed::Store& st = fw.stores[0];
        auto madeAt = fw.made.find(st.source);
        if (madeAt == fw.made.end()) return false;
        const Followed::Made& made = madeAt->second;
        if ((static_cast<uint64_t>(st.what) >> 32) != static_cast<uint64_t>(made.id) || ((static_cast<uint64_t>(st.what) >> 24) & 0xFF) != 4 || st.width != 4)
            return false;
        const Expr* lanes[4] = {nullptr, nullptr, nullptr, nullptr};
        const Expr* v = made.bytes;
        while (v->kind == Expr::Bin && v->op == 'p') {
            if (v->ki % 8 != 0 || v->ki < 0 || v->ki > 24 || lanes[v->ki / 8] != nullptr) return false;
            lanes[v->ki / 8] = v->b;
            v = v->a;
        }
        if (v->kind != Expr::KInt || v->ki != 0) return false;
        // The formula, as image.ww writes it.
        const PatP x = patAny("x"), y = patAny("y"), w = patAny("w"), h = patAny("h");
        const PatP fx = patOp('F', x), fy = patOp('F', y);
        const PatP u = patOp('-', x, patOp('f', fx)), t = patOp('-', y, patOp('f', fy));
        auto held = [&](PatP i, PatP top) { return patOp('m', patOp('M', std::move(i), patInt(0)), patOp('-', std::move(top), patInt(1))); };
        const PatP x0 = held(fx, w), x1 = held(patOp('+', fx, patInt(1)), w);
        const PatP y0 = patOp('*', held(fy, h), w), y1 = patOp('*', held(patOp('+', fy, patInt(1)), h), w);
        const PatP at[4] = {patOp('+', y0, x0), patOp('+', y0, x1), patOp('+', y1, x0), patOp('+', y1, x1)};
        PatP al[4], k[4];
        for (int q = 0; q < 4; q++) al[q] = patOp('f', patField("px", at[q], 3)), k[q] = patOp('/', al[q], patFloat(255.0));
        const PatP su = patOp('-', patFloat(1.0), u), st1 = patOp('-', patFloat(1.0), t);
        auto mixed = [&](PatP p0, PatP p1, PatP p2, PatP p3) {
            return patOp('+', patOp('*', patOp('+', patOp('*', p0, su), patOp('*', p1, u)), st1), patOp('*', patOp('+', patOp('*', p2, su), patOp('*', p3, u)), t));
        };
        auto part = [&](int c) {
            PatP p[4];
            for (int q = 0; q < 4; q++) p[q] = patOp('*', patOp('f', patField("px", at[q], c)), k[q]);
            return mixed(p[0], p[1], p[2], p[3]);
        };
        const PatP a = patOp('m', patFloat(255.0), patOp('M', patFloat(0.0), mixed(al[0], al[1], al[2], al[3])));
        const PatP ab = patOp('i', patOp('+', a, patFloat(0.5)));
        const PatP mul = patOp('*', patOp('f', patOp('m', patInt(1), ab)), patOp('/', patFloat(255.0), patOp('M', a, patFloat(1.0e-300))));
        auto byteOf = [&](PatP p) { return patOp('b', patOp('i', patOp('m', patFloat(255.0), patOp('M', patFloat(0.0), patOp('+', std::move(p), patFloat(0.5)))))); };
        Bound b;
        for (int c = 0; c < 4; c++)
            if (!matches(c < 3 ? *byteOf(patOp('*', part(c), mul)) : *patOp('b', ab), lanes[c], b)) return false;
        // The skip: x below a constant.
        const Followed::Cmp& g = ah.skips[0];
        if (g.op != 1 || !same(g.a, b.exprs["x"]) || g.b == nullptr || g.b->kind != Expr::KFloat) return false;
        // x and y floats of one list at a base plus the counter; w and h the same each time round;
        // the picture's list neither that nor the one set.
        const Expr* ex = b.exprs["x"];
        const Expr* ey = b.exprs["y"];
        const Expr* ew = b.exprs["w"];
        const Expr* eh = b.exprs["h"];
        const int px = b.lists.count("px") != 0 ? b.lists["px"] : -1;
        if (ex == nullptr || ey == nullptr || ex->kind != Expr::Load || ey->kind != Expr::Load || ex->cls != CF || ex->list != ey->list || !fixedOperand(ew) ||
            !fixedOperand(eh) || px < 0 || px == st.list || px == ex->list || ex->list == st.list)
            return false;
        Ahead out, ya;
        if (!fieldIndex(ex->b, ah) || !fieldIndex(ey->b, ya) || !fieldIndex(st.index, out)) return false;
        ah.outMode = out.baseMode, ah.outA = out.baseA, ah.outB = out.baseB;
        ah.yMode = ya.baseMode, ah.yA = ya.baseA, ah.yB = ya.baseB;
        ah.shape = 29;  // kLoopWSampleBilinear
        ah.a = ex->list, ah.b = px, ah.out = st.list, ah.sizeW = ew, ah.sizeH = eh;
        ah.mixWords = {made.global, made.cls};
        std::memcpy(&ah.words[0], &g.b->kf, 8);
        return true;
    }
    // Pixels gathered and made four floats as they are added (kLoopWGatherPixels, 2026-11-15:
    // numeric.ww's __gatherPixels, scaling's across pass reading the picture itself): for each
    // pixel c = px[from + idx[i + x]] (items four bytes wide), m = c[3] as a float over 255.0, w =
    // weight[i + x], `out[o + p] = out[o + p] + c[p] * m * w` for p from 0 to 2 and `out[o + 3] =
    // out[o + 3] + c[3] * w`, o the out base plus four times the counter. Words: 50 to 53 the out
    // base (50: 0 none, 1 an operand; 53 a constant), 63 to 66 the index and weight lists' base, 129
    // the weight list, 130 to 132 `from` (130: 0 none, 1 an operand); 7: 1 where each is 0.0 plus
    // the product, not the float there plus it (the first tap of a row's sums).
    template <typename Changed, typename Written>
    bool gatherPixels(const Followed& fw, Ahead& ah, const Changed& changed, const Written& written) {
        if (fw.stores.size() != 4 || ah.iterSlot >= 0 || !fw.made.empty()) return false;
        Bound b;
        Lin outAt;
        int first = -1;  // 0.0 plus each, not the float there (the first of a row's sums)
        for (int p = 0; p < 4; p++) {
            const Followed::Store& st = fw.stores[static_cast<size_t>(p)];
            if (st.stem != "hsetf" || st.index == nullptr || st.value == nullptr) return false;
            const std::string own = "o" + std::to_string(p);
            const PatP wt = patLoad("w", patAny("wi"), true);
            const PatP part = p < 3 ? patOp('*', patOp('*', patOp('f', patField("px", patAny("c"), p)),
                                                       patOp('/', patOp('f', patField("px", patAny("c"), 3)), patFloat(255.0))),
                                             wt)
                                    : patOp('*', patOp('f', patField("px", patAny("c"), 3)), wt);
            Bound tried = b;
            if (first != 1 && matches(*patOp('+', patLoad("out", patAny(own.c_str()), true), part), st.value, tried)) {
                first = 0;
                b = std::move(tried);
                if (b.lists["out"] != st.list || !same(b.exprs[own], st.index)) return false;
            } else if (first != 0 && matches(*patOp('+', patFloat(0.0), part), st.value, b)) {
                first = 1;
            } else {
                return false;
            }
            Lin o;
            if (!linear(st.index, o, 1, changed, written) || o.coef != 4) return false;
            if (p == 0) outAt = o;
            else if (o.k != outAt.k + p || !same(o.base, outAt.base)) return false;
        }
        // c = from + idx[i + x] (or idx[i + x] alone), the weight at the same place.
        const Expr* c = b.exprs["c"];
        const Expr* from = nullptr;
        const Expr* load = c;
        if (c != nullptr && c->kind == Expr::Bin && c->op == '+' && c->cls == CI) {
            load = c->a->kind == Expr::Load ? c->a : c->b;
            from = load == c->a ? c->b : c->a;
            if (!fixedOperand(from) || changed(from) || written(from)) return false;
        }
        if (load == nullptr || load->kind != Expr::Load || load->cls != CI) return false;
        Lin ii, wi;
        if (!linear(load->b, ii, 1, changed, written) || ii.coef != 1 || !linear(b.exprs["wi"], wi, 1, changed, written) || wi.coef != 1 || wi.k != ii.k ||
            !same(wi.base, ii.base))
            return false;
        const int out = fw.stores[0].list, px = b.lists["px"], wl = b.lists["w"];
        if (px == out || load->list == out || wl == out) return false;
        ah.shape = 31;  // kLoopWGatherPixels
        ah.out = out, ah.a = px, ah.b = load->list;
        ah.baseA = outAt.base, ah.fromBase = ii.base, ah.yA = from;
        ah.copyWhat[0] = outAt.k, ah.copyWhat[1] = ii.k, ah.copyWhat[2] = wl;
        ah.words[0] = first;
        return true;
    }
    // Four multiply-adds of a row in one pass (kLoopFMulAdd4, 2026-11-15: numeric.ww's
    // __mulAdd4, a blur's taps four at a time): out[o + j] = (((out[o + j] + a[b0 + j] * w0) + a[b1 +
    // j] * w1) + a[b2 + j] * w2) + a[b3 + j] * w3, or the first product in place of out's float to
    // start; each base the counter plus what is the same each time round, each weight a float the
    // same each time round, `out` not `a`. Words: 50 to 53 out's base (50: 0 none, 1 an operand; 53
    // a constant), from 130 four a read's (the same), 146 to 149 the weights, 7 1 to start.
    template <typename Changed, typename Written>
    bool mulAdd4(const Followed& fw, Ahead& ah, const Changed& changed, const Written& written) {
        if (fw.stores.size() != 1 || fw.stores[0].stem != "hsetf" || ah.iterSlot >= 0 || fw.stores[0].index == nullptr) return false;
        const Followed::Store& st = fw.stores[0];
        auto product = [](int q) {
            const std::string i = "i" + std::to_string(q), w = "w" + std::to_string(q);
            return patOp('*', patLoad("a", patAny(i.c_str()), true), patAny(w.c_str()));
        };
        auto sum = [&](PatP start) { return patOp('+', patOp('+', patOp('+', patOp('+', std::move(start), product(0)), product(1)), product(2)), product(3)); };
        const PatP firstForm = patOp('+', patOp('+', patOp('+', product(0), product(1)), product(2)), product(3));
        Bound b;
        int first = 0;
        if (matches(*sum(patLoad("out", patAny("o"), true)), st.value, b)) {
            if (b.lists["out"] != st.list || !same(b.exprs["o"], st.index)) return false;
        } else {
            b = Bound{};
            if (!matches(*firstForm, st.value, b)) return false;
            first = 1;
        }
        const int a = b.lists["a"];
        if (a == st.list) return false;
        Lin o;
        if (!linear(st.index, o, 1, changed, written) || o.coef != 1) return false;
        for (int q = 0; q < 4; q++) {
            const std::string i = "i" + std::to_string(q), w = "w" + std::to_string(q);
            Lin at;
            if (!linear(b.exprs[i], at, 1, changed, written) || at.coef != 1) return false;
            const Expr* wt = b.exprs[w];
            if (wt == nullptr || wt->cls != CF || !(wt->kind == Expr::KFloat || wt->kind == Expr::Inv) || changed(wt)) return false;
            ah.fourBase[q] = at.base, ah.fourK[q] = at.k, ah.fourWeight[q] = wt;
        }
        ah.shape = 32;  // kLoopFMulAdd4
        ah.out = st.list, ah.a = a;
        ah.baseA = o.base, ah.copyWhat[0] = o.k;
        ah.words[0] = first;
        return true;
    }
    // How far apart two pixels are (kLoopWApartItems, 2026-11-15: numeric.ww's __regionApart, a
    // row of regions' neighbours): out[o + j] = the most |p.c - q.c| of the four byte fields c, p
    // and q items four bytes wide at their bases plus the counter, `out` a list of ints. Words: 50
    // to 53 where it writes (50: 0 no operand, 1 one; 53 a constant added), 63 to 66 where p is,
    // 130 to 133 where q is.
    template <typename Changed, typename Written>
    bool apartItems(const Followed& fw, Ahead& ah, const Changed& changed, const Written& written) {
        if (fw.stores.size() != 1 || fw.stores[0].stem != "hsetwi" || ah.iterSlot >= 0 || !fw.made.empty() || fw.stores[0].index == nullptr) return false;
        const Followed::Store& st = fw.stores[0];
        auto apart = [](int c) {
            return patOp('a', patOp('-', patField("p", patAny("i"), c), patField("q", patAny("j"), c)));
        };
        Bound b;
        if (!matches(*patOp('M', patOp('M', apart(0), apart(1)), patOp('M', apart(2), apart(3))), st.value, b)) return false;
        Lin o, p, q;
        if (!linear(st.index, o, 1, changed, written) || o.coef != 1 || !linear(b.exprs["i"], p, 1, changed, written) || p.coef != 1 ||
            !linear(b.exprs["j"], q, 1, changed, written) || q.coef != 1)
            return false;
        const int lp = b.lists["p"], lq = b.lists["q"];
        if (lp == st.list || lq == st.list) return false;
        ah.shape = 30;  // kLoopWApartItems
        ah.out = st.list, ah.a = lp, ah.b = lq;
        ah.baseA = o.base, ah.fromBase = p.base, ah.yA = q.base;
        ah.copyWhat[0] = o.k, ah.copyWhat[1] = p.k, ah.copyWhat[2] = q.k;
        return true;
    }
    // Byte fields of each item counted into a list of ints (kLoopWCountItems, 2026-11-15: a
    // histogram, `levels[256 + px[p].green] += 1`; a palette's bins, `bins[32768 + key] += g` with
    // `key = (r >> 3) << 10 | (g >> 3) << 5 | b >> 3`, see-through pixels skipped): every store
    // `counts[K + key] = counts[K + key] + v`, K a constant, the key one byte field or the one
    // key the stores share (byte fields each shifted right and then left, or'd together), v a
    // constant or a byte field (`-=` a constant taken away); up to eight stores, all into one list
    // of ints, the fields of one item of a list of a value type's items at a base plus the
    // counter; an item skipped where a byte field compares so with a constant. The template does
    // them in order for each item, as the steps would. Words: 50 to 54 where the items are read
    // (fieldIndex); 128 how many stores, 129 the items' width; 130 how many terms the shared key
    // has, three each from 131 (the field's offset, the shifts right and left); 143 the skip's
    // comparison (0 none, else as a guard's), 144 its field's offset, 145 its constant; from 146
    // four a store: K, its own field's offset or -1 for the shared key, 0 for a constant v or 1
    // for a field, and v or the field's offset.
    bool countItems(const Followed& fw, Ahead& ah) {
        if (fw.stores.empty() || fw.stores.size() > 8 || !fw.made.empty() || !fw.places.empty() || ah.skips.size() > 1) return false;
        int out = -1, items = -1;
        const Expr* index = nullptr;
        int64_t width = 0;
        // A byte field of the one item every field is of: its offset, or -1.
        auto byteOf = [&](const Expr* f) -> int64_t {
            if (f == nullptr || f->kind != Expr::Field || (f->ki & 0xFF) != 1) return -1;
            if (items < 0) items = f->list, index = f->b, width = f->ki >> 32;
            else if (f->list != items || !same(f->b, index) || (f->ki >> 32) != width) return -1;
            const int64_t offset = (f->ki >> 8) & 0xFFFFFF;
            return offset < width ? offset : -1;
        };
        // The shared key: an or of terms, each a byte field shifted right, then left.
        const Expr* shared = nullptr;
        std::vector<int64_t> terms;
        std::function<bool(const Expr*)> keyTerms = [&](const Expr* e) -> bool {
            if (e->kind == Expr::Bin && e->op == '|' && e->cls == CI) return keyTerms(e->a) && keyTerms(e->b);
            int64_t shl = 0, shr = 0;
            if (e->kind == Expr::Bin && e->op == '<' && e->b->kind == Expr::KInt) shl = e->b->ki, e = e->a;
            if (e->kind == Expr::Bin && e->op == '>' && e->b->kind == Expr::KInt) shr = e->b->ki, e = e->a;
            const int64_t offset = byteOf(e);
            if (offset < 0 || shl < 0 || shl > 31 || shr < 0 || shr > 7 || terms.size() >= 12) return false;
            terms.push_back(offset), terms.push_back(shr), terms.push_back(shl);
            return true;
        };
        std::vector<int64_t> words;
        for (const Followed::Store& st : fw.stores) {
            if (st.stem != "hsetwi" || st.index == nullptr || st.value == nullptr) return false;
            if (out < 0) out = st.list;
            else if (st.list != out) return false;
            const Expr* f = st.index;
            int64_t K = 0;
            if (f->kind == Expr::Bin && f->op == '+' && f->cls == CI) {
                if (f->a->kind == Expr::KInt) K = f->a->ki, f = f->b;
                else if (f->b->kind == Expr::KInt) K = f->b->ki, f = f->a;
                else return false;
            }
            int64_t own = byteOf(f);
            if (own < 0) {
                if (shared == nullptr) {
                    if (!keyTerms(f)) return false;
                    shared = f;
                } else if (!same(shared, f)) {
                    return false;
                }
            }
            // The count plus a constant or a byte field, or (`-=`) less a constant.
            const Expr* v = st.value;
            if (v->kind != Expr::Bin || (v->op != '+' && v->op != '-') || v->cls != CI) return false;
            const Expr* load = v->op == '-' || v->a->kind == Expr::Load ? v->a : v->b;
            const Expr* c = load == v->a ? v->b : v->a;
            if (load->kind != Expr::Load || load->list != out || load->cls != CI || !same(load->b, st.index)) return false;
            int64_t kind, value;
            if (c->kind == Expr::KInt) {
                if (c->ki < -(1 << 20) || c->ki > (1 << 20)) return false;
                kind = 0, value = v->op == '-' ? -c->ki : c->ki;
            } else {
                if (v->op != '+' || (value = byteOf(c)) < 0) return false;
                kind = 1;
            }
            if (K < 0 || K > (int64_t{1} << 40)) return false;
            words.push_back(K), words.push_back(own), words.push_back(kind), words.push_back(value);
        }
        // The skip: a byte field of the item against a constant.
        int64_t skip[3] = {0, 0, 0};
        if (!ah.skips.empty()) {
            const Followed::Cmp& g = ah.skips[0];
            const int64_t offset = byteOf(g.a);
            if (offset < 0 || g.b == nullptr || g.b->kind != Expr::KInt || g.op < 1 || g.op > 6) return false;
            skip[0] = g.op, skip[1] = offset, skip[2] = g.b->ki;
        }
        if (items < 0 || items == out || width <= 0 || width > 0xFFFF || !fieldIndex(index, ah)) return false;
        ah.shape = 26;  // kLoopWCountItems
        ah.a = items, ah.out = out;
        ah.mixWords.assign(18, 0);
        ah.mixWords[0] = static_cast<int64_t>(fw.stores.size()), ah.mixWords[1] = width, ah.mixWords[2] = static_cast<int64_t>(terms.size() / 3);
        for (size_t k = 0; k < terms.size(); k++) ah.mixWords[3 + k] = terms[k];
        ah.mixWords[15] = skip[0], ah.mixWords[16] = skip[1], ah.mixWords[17] = skip[2];
        ah.mixWords.insert(ah.mixWords.end(), words.begin(), words.end());
        return true;
    }
    // How a mix ends (2026-11-13): `>> shift`, `/ divisor` (a constant of 1 or more, which truncates
    // toward zero as C++'s does), or both in either order; `v` the sum before them.
    static bool mixEnd(const Expr*& v, int64_t& shift, int64_t& divisor, bool& divideFirst) {
        shift = 0, divisor = 1, divideFirst = false;
        bool shifted = false, divided = false, shiftOutside = false;
        for (int k = 0; k < 2; k++) {
            if (!shifted && v->kind == Expr::Bin && v->op == '>' && v->b->kind == Expr::KInt) {
                shift = v->b->ki, shifted = true, shiftOutside = !divided;
                v = v->a;
            } else if (!divided && v->kind == Expr::Bin && v->op == '/' && v->cls == CI && v->b->kind == Expr::KInt) {
                divisor = v->b->ki, divided = true;
                v = v->a;
            } else {
                break;
            }
        }
        // Divided first where the division is inside the shift, or there is no shift.
        divideFirst = divided && (!shifted || shiftOutside);
        return shift >= 0 && shift <= 31 && divisor >= 1 && divisor <= (1 << 20);
    }
    // A guard, where it is not one already.
    void addGuard(Ahead& ah, const Followed::Cmp& g) {
        for (const Followed::Cmp& h : ah.guards)
            if (h.op == g.op && same(h.a, g.a) && same(h.b, g.b)) return;
        ah.guards.push_back(g);
    }
    // A value of a value type made from bytes, each one mix of byte fields of an item, a byte
    // field of that item, or a constant, stored into that item, or into another list's item at the
    // counter (kLoopWMixItems, 2026-11-13: a greyscale by image[x, y] = Color(v, v, v, c.alpha), in
    // editPixels, or by map). Words from 96: 96 how (bit 0: into another list), 97 the global the
    // value's class is and 98 that class, 99 the type's number, 100 the size stored (bytes) and the
    // items' widths read and written (bits 8 and 32 on), 101 the constant bits, 102 the bytes' kinds
    // (8 a byte: 0 the constant's, 1 the mix, 2 + the field's offset), 103 the mix's shift and
    // how many it reads, 104 its constant, 105 to 108 each read's weight (high half) and offset.
    bool mixItems(const Followed& fw, Ahead& ah) {
        const Followed::Store& st = fw.stores[0];
        const Followed::Made& made = fw.made.at(st.source);
        if ((static_cast<uint64_t>(st.what) >> 32) != static_cast<uint64_t>(made.id)) return false;
        const uint64_t size = (static_cast<uint64_t>(st.what) >> 24) & 0xFF;
        if (size == 0 || size > 8 || st.width <= 0 || st.width > 0xFFFF || static_cast<uint64_t>(st.width) < size) return false;
        // The bytes, down to the constant they start from.
        std::vector<std::pair<int64_t, const Expr*>> lanes;
        const Expr* v = made.bytes;
        while (v->kind == Expr::Bin && v->op == 'p') {
            lanes.push_back({v->ki, v->b});
            v = v->a;
        }
        if (v->kind != Expr::KInt || lanes.empty()) return false;
        const uint64_t constant = static_cast<uint64_t>(v->ki);
        // The item read: the one list and index every field is of.
        int list = -1;
        const Expr* index = nullptr;
        int64_t readWidth = 0;
        auto field = [&](const Expr* f) {
            if (f->kind != Expr::Field || (f->ki & 0xFF) != 1) return false;
            if (list < 0) list = f->list, index = f->b, readWidth = f->ki >> 32;
            return f->list == list && same(f->b, index) && (f->ki >> 32) == readWidth;
        };
        const Expr* theMix = nullptr;
        uint64_t kinds = 0;
        for (const auto& [shift, e] : lanes) {
            if (shift < 0 || shift % 8 != 0 || static_cast<uint64_t>(shift) / 8 >= size) return false;
            const uint64_t at = static_cast<uint64_t>(shift) / 8;
            if (((kinds >> (8 * at)) & 0xFF) != 0) return false;
            uint64_t kind;
            if (e->kind == Expr::Field) {
                if (!field(e)) return false;
                kind = 2 + ((static_cast<uint64_t>(e->ki) >> 8) & 0xFFFFFF);
                if (kind >= 0x60) return false;  // below the float mixes' kinds
            } else if (e->kind == Expr::ToByte && e->a->kind == Expr::ToInt) {
                // A float mix of byte fields of the item, held between two bounds and cut to a
                // whole number (2026-11-13: __mixColors, greyscale and saturation): 0x60 + its lane.
                Ahead::FloatLane lane;
                if (ah.floatLanes.size() >= 4 || !floatLane(e->a->a, lane, field)) return false;
                kind = 0x60 | ah.floatLanes.size();
                ah.floatLanes.push_back(lane);
            } else if (const Expr* t = e->kind == Expr::ToByte ? e->a : e;
                       t->kind == Expr::Load && t->cls == CI && t->b != nullptr && t->b->kind == Expr::Field && t->list >= 0 && t->list < 6) {
                // A table's byte at a byte of the item (2026-11-13: mapChannels): 0x80, the table's
                // list (as looked at once) times 8, plus the byte's offset. The template takes only
                // a table of bytes, every item a byte, so a check that it is one holds.
                if (!field(t->b)) return false;
                const uint64_t offset = (static_cast<uint64_t>(t->b->ki) >> 8) & 0xFFFFFF;
                if (offset > 7) return false;
                kind = 0x80 | static_cast<uint64_t>(t->list) << 3 | offset;
            } else if (e->kind == Expr::ToByte) {
                if (theMix != nullptr && !same(theMix, e)) return false;
                theMix = e;
                kind = 1;
            } else {
                return false;
            }
            if (kind < 0x80 && kind >= 0x7E) return false;  // a field's offset too far for its kind
            kinds |= kind << (8 * at);
        }
        // The mix: (a sum of byte fields of the item, each times a constant, plus a constant) >> a
        // constant, as a byte.
        int64_t shift = 0, c = 0, divisor = 1;
        bool divideFirst = false;
        std::vector<std::pair<int64_t, int64_t>> terms;  // weight, offset
        if (theMix != nullptr) {
            const Expr* m = theMix->a;
            if (!mixEnd(m, shift, divisor, divideFirst)) return false;
            // Terms added or taken away (2026-11-15: `a * 200 - b * 45 + k`), each with its sign.
            std::function<bool(const Expr*, int64_t)> sum = [&](const Expr* e, int64_t sign) -> bool {
                if (e->cls != CI) return false;
                if (e->kind == Expr::KInt) return e->ki >= -(1 << 28) && e->ki <= (1 << 28) && (c += sign * e->ki, true);
                if (e->kind == Expr::Bin && e->op == '+') return sum(e->a, sign) && sum(e->b, sign);
                if (e->kind == Expr::Bin && e->op == '-') return sum(e->a, sign) && sum(e->b, -sign);
                const Expr* f = e;
                int64_t w = 1;
                if (e->kind == Expr::Bin && e->op == '*') {
                    const Expr* k = e->a->kind == Expr::KInt ? e->a : e->b;
                    f = k == e->a ? e->b : e->a;
                    if (k->kind != Expr::KInt) return false;
                    w = k->ki;
                }
                if (!field(f) || w < -(1 << 20) || w > (1 << 20)) return false;
                terms.push_back({w * sign, (f->ki >> 8) & 0xFFFFFF});
                return true;
            };
            if (!sum(m, 1) || terms.empty() || terms.size() > 4 || c < -(1 << 28) || c > (1 << 28)) return false;
            for (const auto& t : terms)
                if (t.second >= readWidth) return false;
        }
        if (list < 0) return false;
        // Into the item read, or into another list's item at a base of its own plus the counter
        // (2026-11-15: `to[x, y] = ...` from `from[x, y]`, two grids, each y times its width
        // plus x), the item read at its base plus the counter.
        bool into = false;
        if (st.list == list && same(st.index, index)) {
            if (st.width != readWidth) return false;
        } else if (st.list != list) {
            Ahead out;
            if (!fieldIndex(st.index, out)) return false;
            ah.outMode = out.baseMode, ah.outA = out.baseA, ah.outB = out.baseB;
            into = true;
        } else {
            return false;
        }
        if (!fieldIndex(index, ah)) return false;
        ah.shape = 19;  // kLoopWMixItems
        ah.a = list;
        ah.out = st.list;
        ah.mixWords = {into ? 1 : 0, made.global, made.cls, made.id,
                       static_cast<int64_t>(size | static_cast<uint64_t>(readWidth) << 8 | static_cast<uint64_t>(st.width) << 32),
                       static_cast<int64_t>(constant), static_cast<int64_t>(kinds),
                       static_cast<int64_t>(static_cast<uint64_t>(shift) | static_cast<uint64_t>(terms.size()) << 8), c};
        for (const auto& t : terms) ah.mixWords.push_back(static_cast<int64_t>(static_cast<uint64_t>(t.first) << 32 | static_cast<uint64_t>(t.second)));
        while (ah.mixWords.size() < 13) ah.mixWords.push_back(0);
        ah.mixWords.push_back(divisor);              // 109
        ah.mixWords.push_back(divideFirst ? 1 : 0);  // 110
        return true;
    }

    // A float mix of byte fields, as kLoopWMixItems takes one (stencils.cpp, FloatMix): x held by
    // max and min with constants lo and hi (in either order: every sum is finite, and lo <= hi),
    // where x is a sum of terms added left to right, each a weight (a constant, or what is the same
    // each time round) times a byte field as a float, with a constant added last or not.
    template <typename F>
    bool floatLane(const Expr* x, Ahead::FloatLane& lane, F& field) {
        bool haveLo = false, haveHi = false;
        while ((x->kind == Expr::Min || x->kind == Expr::Max) && x->cls == CF) {
            const Expr* k = x->a->kind == Expr::KFloat ? x->a : x->b->kind == Expr::KFloat ? x->b : nullptr;
            if (k == nullptr) return false;
            bool& have = x->kind == Expr::Max ? haveLo : haveHi;
            if (have) return false;
            have = true;
            (x->kind == Expr::Max ? lane.lo : lane.hi) = k->kf;
            x = k == x->a ? x->b : x->a;
        }
        if (!haveLo || !haveHi || !(lane.lo > -1.0) || !(lane.hi < 256.0) || lane.lo > lane.hi) return false;
        auto isTerm = [&](const Expr* t) {
            if (t->kind != Expr::Bin || t->op != '*' || t->cls != CF) return false;
            const Expr* f = t->a->kind == Expr::ToFloat ? t->a : t->b->kind == Expr::ToFloat ? t->b : nullptr;
            if (f == nullptr) return false;
            const Expr* w = f == t->a ? t->b : t->a;
            return w->cls == CF && (w->kind == Expr::KFloat || w->kind == Expr::Inv) && f->a->kind == Expr::Field && field(f->a);
        };
        if (x->kind == Expr::Bin && x->op == '+' && x->cls == CF && (x->a->kind == Expr::KFloat || x->b->kind == Expr::KFloat)) {
            lane.c = x->a->kind == Expr::KFloat ? x->a : x->b;  // a + b is b + a, exactly
            x = lane.c == x->a ? x->b : x->a;
        }
        std::vector<const Expr*> terms;
        while (x->kind == Expr::Bin && x->op == '+' && x->cls == CF && isTerm(x->b)) {
            terms.push_back(x->b);
            x = x->a;
        }
        if (!isTerm(x)) return false;
        terms.push_back(x);
        if (terms.size() > 4) return false;
        for (auto it = terms.rbegin(); it != terms.rend(); ++it) {
            const Expr* t = *it;
            const Expr* f = t->a->kind == Expr::ToFloat ? t->a : t->b;
            const Expr* w = f == t->a ? t->b : t->a;
            const uint64_t offset = (static_cast<uint64_t>(f->a->ki) >> 8) & 0xFFFFFF;
            if (offset > 7) return false;
            lane.terms.emplace_back(static_cast<int64_t>(offset), w);
        }
        return true;
    }

    // An item's index of base + counter, base none, an operand, or one operand times another.
    static bool fieldIndex(const Expr* e, Ahead& ah) {
        if (e == nullptr || e->cls != CI) return false;
        if (e->kind == Expr::Counter) return ah.baseMode = 0, true;
        if (e->kind != Expr::Bin || e->op != '+') return false;
        const Expr* base = e->a->kind == Expr::Counter ? e->b : e->b->kind == Expr::Counter ? e->a : nullptr;
        if (base == nullptr) return false;
        if (fixedOperand(base)) return ah.baseMode = 1, ah.baseA = base, true;
        if (base->kind == Expr::Bin && base->op == '*' && base->cls == CI && fixedOperand(base->a) && fixedOperand(base->b))
            return ah.baseMode = 2, ah.baseA = base->a, ah.baseB = base->b, true;
        return false;
    }

    // Bytes of an item of K bytes, each set to one mix of bytes of it (kLoopWMix, 2026-11-13: a
    // greyscale of a picture's bytes in place): every store a byte's value into bytes, at counter
    // * K + an offset below K, of one value: (a sum of items of one list at counter * K + an
    // offset below K, each times a constant, plus a constant) >> a constant, as a byte. The
    // weights and the constant small, so that nothing it works out can overflow.
    bool mix(const Followed& fw, Ahead& ah) {
        if (fw.stores.empty() || fw.stores.size() > 4) return false;
        const Followed::Store& first = fw.stores[0];
        int64_t K = 0;
        uint64_t writes = 0;
        for (size_t m = 0; m < fw.stores.size(); m++) {
            const Followed::Store& st = fw.stores[m];
            int64_t stride = 0, offset = 0;
            // One value, worked out once before any of them is stored: every read before every write.
            if (st.stem != "hsetwb" || st.list != first.list || st.value != first.value || !strided(st.index, stride, offset)) return false;
            if (K == 0) K = stride;
            if (stride != K || K > 255 || offset >= K) return false;
            writes |= static_cast<uint64_t>(offset) << (8 * m);
        }
        const Expr* v = first.value;
        if (v->kind != Expr::ToByte) return false;
        v = v->a;
        int64_t shift = 0, divisor = 1;
        bool divideFirst = false;
        if (!mixEnd(v, shift, divisor, divideFirst)) return false;
        ah.mixDivisor = divisor, ah.mixDivideFirst = divideFirst;
        struct Term {
            int64_t weight, offset;
        };
        std::vector<Term> terms;
        int64_t c = 0;
        int list = -1;
        // Each term with its sign: what is taken away, its weight turned round.
        std::function<bool(const Expr*, int64_t)> sum = [&](const Expr* e, int64_t sign) -> bool {
            if (e->cls != CI) return false;
            if (e->kind == Expr::KInt) {
                if (e->ki < -(int64_t{1} << 40) || e->ki > (int64_t{1} << 40)) return false;
                return c += sign * e->ki, true;
            }
            if (e->kind == Expr::Bin && e->op == '+') return sum(e->a, sign) && sum(e->b, sign);
            if (e->kind == Expr::Bin && e->op == '-') return sum(e->a, sign) && sum(e->b, -sign);
            const Expr* load = e;
            int64_t w = sign;
            if (e->kind == Expr::Bin && e->op == '*') {
                const Expr* k = e->a->kind == Expr::KInt ? e->a : e->b;
                load = e->a->kind == Expr::KInt ? e->b : e->a;
                if (k->kind != Expr::KInt || k->ki < -(int64_t{1} << 40) || k->ki > (int64_t{1} << 40)) return false;
                w = sign * k->ki;
            }
            int64_t stride = 0, offset = 0;
            if (load->kind != Expr::Load || load->cls != CI || !strided(load->b, stride, offset) || stride != K || offset >= K) return false;
            if (list >= 0 && load->list != list) return false;
            list = load->list;
            terms.push_back(Term{w, offset});
            return true;
        };
        if (!sum(v, 1) || terms.empty() || terms.size() > 4 || list < 0) return false;
        for (const Term& t : terms)
            if (t.weight < -(1 << 20) || t.weight > (1 << 20)) return false;
        if (c < -(1 << 28) || c > (1 << 28)) return false;
        ah.shape = 14;  // kLoopWMix
        ah.out = first.list;
        ah.a = list;
        ah.bytesOut = true;
        ah.words[0] = static_cast<int64_t>(static_cast<uint64_t>(K) | static_cast<uint64_t>(shift) << 8 | static_cast<uint64_t>(terms.size()) << 16 |
                                           static_cast<uint64_t>(fw.stores.size()) << 24 | writes << 32);
        ah.words[1] = c;
        for (size_t k = 0; k < terms.size(); k++)
            ah.words[2 + k] = static_cast<int64_t>(static_cast<uint64_t>(terms[k].weight) << 32 | static_cast<uint64_t>(terms[k].offset));
        return true;
    }

    // The loop from head h (its RangeNext, the step 1) to its Loop e: a template where it fits.
    int templateHead = -1, templateEnd = -1;  // the loop recognise last looked at (templateSteps)
    bool recognise(int h, int e, Ahead& ah, Reg& next, Reg& left) {
        templateHead = h, templateEnd = e;
        const Result& head = results[static_cast<size_t>(h)];
        Followed fw;
        loopTop = in[static_cast<size_t>(h)].top;
        Reg counter{-101, CI};
        if (head.ops.size() == 1 && head.ops[0].stem == "iterref") {
            // A for over a list of items, each a Ref (2026-11-13): the counter the item's index,
            // which the template reads from the iterator and leaves it at.
            ah.iterSlot = head.ops[0].slot;
            next = left = Reg{-100, CI};
            Expr c{Expr::Counter};
            fw.refs[head.ops[0].slot2] = Followed::Ref{-2, mk(c), 0};
        } else {
            if (head.ops.size() != 4 || head.ops[0].stem != "jngtik" || head.ops[2].stem != "movi" || head.ops[3].stem != "addwik" || head.ops[3].ki != 1)
                return false;
            left = head.ops[0].named[0];
            counter = head.ops[2].named[0];
            next = head.ops[2].named[1];
            Expr c{Expr::Counter};
            fw.val[counter] = mk(c);
        }
        std::set<Reg> defined;
        std::vector<Followed::Cmp> pending;
        // A kind check of a variable the loop never writes: checked once, by the template.
        auto kindCheck = [&](const LOp& l) {
            const int slot = static_cast<int>(l.slot);
            for (int u = h; u <= e; u++)
                if (in[static_cast<size_t>(u)].kind(slot).bits != kEntry) return false;
            const int kind = l.stem == "chkf" ? 1 : l.stem == "chkint" ? 2 : 3;
            if (ah.checkSlot < 0 || (ah.checkSlot == slot && ah.checkKind == kind)) return ah.checkSlot = slot, ah.checkKind = kind, true;
            // A second (2026-11-15: a gather's `from` beside its out base), in words 47 and 48.
            if (ah.checkSlot2 < 0 || (ah.checkSlot2 == slot && ah.checkKind2 == kind)) return ah.checkSlot2 = slot, ah.checkKind2 = kind, true;
            return false;
        };
        for (int i = h + 1; i < e;) {
            const Result& r = results[static_cast<size_t>(i)];
            if (!in[static_cast<size_t>(i)].known || r.island || r.fallExit >= 0) return false;
            // An int chosen by a comparison with a constant: x > K ? K : x and the like.
            const LOp* last = r.ops.empty() ? nullptr : &r.ops.back();
            // A held int field set to a value where the value is below it (or above it): its min
            // (max) with that value, `if (x < m) { m = x }` (2026-11-13: an image's changed rectangle).
            if (last != nullptr && last->stem.size() == 6 && last->stem.rfind("jn", 0) == 0 && last->stem.substr(4) == "ii" && last->jumpExit < 0 &&
                last->jumpTo > i + 1 && last->jumpTo < e) {
                Followed tried = fw;
                bool fits = true;
                for (size_t k = 0; k + 1 < r.ops.size() && fits; k++) fits = r.ops[k].jumpTo < 0 && r.ops[k].jumpExit < 0 && follow(tried, r.ops[k]);
                const std::string cmp = last->stem.substr(2, 2);
                const Expr* a = fits ? valueOf(tried, last->named[0]) : nullptr;
                const Expr* b = fits ? valueOf(tried, last->named[1]) : nullptr;
                const bool heldB = b != nullptr && b->kind == Expr::Held && b->op != 'b';
                const bool heldA = a != nullptr && a->kind == Expr::Held && a->op != 'b';
                if (fits && (heldA != heldB) && (cmp == "lt" || cmp == "le" || cmp == "gt" || cmp == "ge")) {
                    const Expr* held = heldB ? b : a;
                    const Expr* other = heldB ? a : b;
                    // Stored where `a cmp b` holds: other below held (a min) or above it (a max).
                    const bool below = heldB ? (cmp == "lt" || cmp == "le") : (cmp == "gt" || cmp == "ge");
                    std::vector<Reg> set;
                    int stores = 0;
                    for (int u = i + 1; u < last->jumpTo && fits; u++) {
                        if (!in[static_cast<size_t>(u)].known || results[static_cast<size_t>(u)].island) fits = false;
                        for (const LOp& l : results[static_cast<size_t>(u)].ops) {
                            if (!fits) break;
                            if (l.jumpTo >= 0 || l.jumpExit >= 0) fits = false;
                            else if (l.stem == "chkf" || l.stem == "chkint" || l.stem == "chki") fits = kindCheck(l);
                            else if (l.stem == "hsetpi" && l.slot == held->ki && same(valueOf(tried, l.named[0]), other)) stores++;
                            else if (l.stem == "movi") set.push_back(l.named[0]);
                            else fits = false;
                        }
                    }
                    if (fits && stores == 1 && tried.heldWrites.count(held->ki) == 0) {
                        fw = tried;
                        for (const LOp& l : r.ops)
                            for (const Reg& d : l.defs) defined.insert(d);
                        Expr m{below ? Expr::Min : Expr::Max};
                        m.a = held;
                        m.b = other;
                        fw.heldWrites[held->ki] = mk(m);
                        Expr unknown{Expr::Unknown};
                        for (const Reg& d : set) {
                            fw.val[d] = mk(unknown);
                            defined.insert(d);
                        }
                        i = last->jumpTo;
                        continue;
                    }
                }
            }
            // A skip (2026-11-15): a jump on where a comparison does not hold, which falls into the
            // loop's way back to its head where it does (`if (c.alpha < 128) { continue }`): the
            // item left as it is, the rest of the body not done. kLoopWCountItems and
            // kLoopWBlendNormal take it.
            if (last != nullptr && last->stem.size() == 6 && last->stem.rfind("jn", 0) == 0 && (last->stem.substr(4) == "ii" || last->stem.substr(4) == "ik" || last->stem.substr(4) == "fk") &&
                last->jumpExit < 0 && last->jumpTo >= i + 2 && last->jumpTo < e) {
                // The way there: instructions with no steps (the body's variables let go), then
                // the way back alone.
                const int at = last->jumpTo - 1;
                const Result& back = results[static_cast<size_t>(at)];
                const Result& end = results[static_cast<size_t>(e)];
                bool loopsBack = in[static_cast<size_t>(at)].known && !back.island && back.ops.size() == 1 && back.ops[0].stem == "loopback" &&
                                 !end.ops.empty() && end.ops.back().stem == "loopback" && back.ops[0].jumpTo == end.ops.back().jumpTo;
                for (int u = i + 1; u < at && loopsBack; u++)
                    loopsBack = in[static_cast<size_t>(u)].known && !results[static_cast<size_t>(u)].island && results[static_cast<size_t>(u)].ops.empty() &&
                                results[static_cast<size_t>(u)].fallsThrough;
                static const char* const ops[] = {"lt", "le", "gt", "ge", "eq", "ne"};
                int op = 0;
                for (int k = 0; k < 6; k++)
                    if (last->stem.compare(2, 2, ops[k]) == 0) op = k + 1;
                if (loopsBack && op != 0 && ah.skips.empty() && fw.stores.empty()) {
                    for (size_t k = 0; k + 1 < r.ops.size(); k++) {
                        const LOp& l = r.ops[k];
                        if (l.jumpTo >= 0 || l.jumpExit >= 0 || !follow(fw, l)) return false;
                        for (const Reg& d : l.defs) defined.insert(d);
                    }
                    Followed::Cmp g;
                    g.op = op;
                    g.a = valueOf(fw, last->named[0]);
                    g.b = last->stem.substr(4) == "ik" ? kintOf(last->ki) : last->stem.substr(4) == "fk" ? kfloatOf(last->kf) : valueOf(fw, last->named[1]);  // fk: a float against a constant (2026-11-15: the blend's sa <= 0.0)
                    ah.skips.push_back(g);
                    i = last->jumpTo;
                    continue;
                }
            }
            // A guard as one step (2026-11-13): a jump on where a comparison does not hold, which
            // falls into a way to a throw or a return where it does (`if (i >= n) { throw ... }`).
            if (last != nullptr && last->stem.size() == 6 && last->stem.rfind("jn", 0) == 0 && (last->stem.substr(4) == "ii" || last->stem.substr(4) == "ik") &&
                last->jumpExit < 0 && last->jumpTo > i + 1 && last->jumpTo < e) {
                bool ends = true;
                for (int u = i + 1; u < last->jumpTo && ends; u++) ends = endingAt(u);
                static const char* const ops[] = {"lt", "le", "gt", "ge", "eq", "ne"};
                int op = 0;
                for (int k = 0; k < 6; k++)
                    if (last->stem.compare(2, 2, ops[k]) == 0) op = k + 1;
                if (ends && op != 0) {
                    for (size_t k = 0; k + 1 < r.ops.size(); k++) {
                        const LOp& l = r.ops[k];
                        if (l.jumpTo >= 0 || l.jumpExit >= 0 || !follow(fw, l)) return false;
                        for (const Reg& d : l.defs) defined.insert(d);
                    }
                    Followed::Cmp g;
                    g.op = op;
                    g.a = valueOf(fw, last->named[0]);
                    g.b = last->stem.substr(4) == "ik" ? kintOf(last->ki) : valueOf(fw, last->named[1]);
                    addGuard(ah, g);
                    i = last->jumpTo;
                    continue;
                }
            }
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
            int skipTo = -1;
            const LOp* classCheck = nullptr;
            for (const LOp& l : r.ops) {
                // A class check of an object as the loop entered it, the way where it fails out of
                // the region: checked once, by the template (2026-11-13).
                if (l.stem == "clsflag") {
                    const int slot = static_cast<int>(l.slot);
                    for (int u = h; u <= e; u++)
                        if (in[static_cast<size_t>(u)].kind(slot).bits != kEntry) return false;
                    classCheck = &l;
                    continue;
                }
                if (classCheck != nullptr) {
                    if (l.stem != "jfalse" || l.jumpExit < 0) return false;
                    const std::pair<int64_t, int64_t> check{classCheck->slot, classCheck->ki};
                    if (std::find(ah.classChecks.begin(), ah.classChecks.end(), check) == ah.classChecks.end()) ah.classChecks.push_back(check);
                    if (ah.classChecks.size() > 2) return false;
                    classCheck = nullptr;
                    continue;
                }
                // A guard (2026-11-13): comparisons joined by `or`, each true one jumping on to the
                // next `or` (or to the last jump), the last jump going on where none holds and
                // falling into a way to a throw or a return otherwise. None may hold anywhere in
                // the range, which the template checks first.
                if (l.stem == "jtrue" || l.stem == "jfalse") {
                    if (fw.last.op == 0 || l.jumpExit >= 0 || l.jumpTo <= i || l.jumpTo >= e) return false;
                    pending.push_back(fw.last);
                    fw.last = Followed::Cmp{};
                    if (l.stem == "jtrue") {
                        const Result& to = results[static_cast<size_t>(l.jumpTo)];
                        if (to.ops.size() != 1 || (to.ops[0].stem != "jtrue" && to.ops[0].stem != "jfalse")) return false;
                        continue;
                    }
                    if (&l != &r.ops.back()) return false;
                    // What it falls into where a comparison holds (a way to a throw or a return, or
                    // getOrNull's null, joining later) is never run where the template runs: it
                    // runs only where none of them holds anywhere in the loop.
                    if (l.jumpTo == i + 1) return false;
                    for (const Followed::Cmp& g : pending) {
                        if (g.op == 7) return false;
                        if (g.op == 8) {
                            // Two items not the same falling into the way out: the template stops
                            // before the first such (2026-11-14, kLoopWSameItems).
                            if (ah.stopAt.listA >= 0) return false;
                            ah.stopAt = fw.itemCmp;
                            continue;
                        }
                        // An item's four bytes, one word (a pixel's fields put together), not a
                        // word the same each time round: the template stops before the first such
                        // (2026-11-15, kLoopWFindWord: inkBox's scans).
                        auto wholeWord = [](const Expr* f) {
                            return f != nullptr && f->kind == Expr::Field && (f->ki & 0xFF) == 4 && ((f->ki >> 8) & 0xFFFFFF) == 0 && (f->ki >> 32) == 4;
                        };
                        if (g.op == 6 && (wholeWord(g.a) || wholeWord(g.b))) {
                            const Expr* f = wholeWord(g.a) ? g.a : g.b;
                            const Expr* w = f == g.a ? g.b : g.a;
                            if (ah.stopList >= 0 || !fixedOperand(w)) return false;
                            ah.stopList = f->list, ah.stopIndex = f->b, ah.stopWord = w;
                            continue;
                        }
                        addGuard(ah, g);
                    }
                    pending.clear();
                    skipTo = l.jumpTo;
                    continue;
                }
                // A jump on over steps the region never reaches (the VM's way past a class check
                // that leaves the region where it fails).
                if (l.stem == "jump" && &l == &r.ops.back() && l.jumpExit < 0 && l.jumpTo > i + 1 && l.jumpTo < e) {
                    for (int u = i + 1; u < l.jumpTo; u++)
                        if (in[static_cast<size_t>(u)].known) return false;
                    skipTo = l.jumpTo;
                    continue;
                }
                if (l.jumpTo >= 0 || l.jumpExit >= 0) return false;
                if ((l.stem.rfind("hsetp", 0) == 0 && l.stem.rfind("hsetpi", 0) != 0) || l.stem == "putp") return false;
                if (l.stem == "chkf" || l.stem == "chkint" || l.stem == "chki") {
                    if (!kindCheck(l)) return false;
                    continue;
                }
                if (!follow(fw, l)) return false;
                for (const Reg& d : l.defs) defined.insert(d);
            }
            i = skipTo >= 0 ? skipTo : i + 1;
        }
        if (!pending.empty() || ah.guards.size() > 12) return false;
        // A guard's sides: the counter plus a constant, or what is the same each time round.
        auto changed = [&](const Expr* e) { return e != nullptr && e->kind == Expr::Inv && defined.count(e->reg) != 0; };
        for (const Followed::Cmp& g : ah.guards)
            if (!guardSide(g.a) || !guardSide(g.b) || changed(g.a) || changed(g.b)) return false;
        // A place made at an item: of an item a store sets, its index checked there.
        for (const auto& [list, index] : fw.places) {
            bool stored = false;
            for (const Followed::Store& st : fw.stores) stored = stored || (st.list == list && same(st.index, index));
            if (!stored) return false;
        }
        // The slots the body writes beside its registers: temporaries above the loop's own.
        for (int64_t slot : fw.slots)
            if (slot < in[static_cast<size_t>(h)].top) return false;
        // A value read from a slot as the loop entered it: one the body never writes.
        for (const auto& [slot, v] : fw.values)
            if (v.kind == 1 && std::find(fw.slots.begin(), fw.slots.end(), v.at) != fw.slots.end()) return false;
        // Held int fields the body writes: its min or max with what is the same each time round or
        // the counter plus a constant, or itself plus a constant; none of them read by a guard or an
        // index.
        auto written = [&](const Expr* x) { return x != nullptr && x->kind == Expr::Held && fw.heldWrites.count(x->ki) != 0; };
        auto plusOf = [&](const Expr* x) { return x != nullptr && x->kind == Expr::Bin && x->op == '+' ? (x->a->kind == Expr::KInt ? x->b : x->a) : x; };
        for (const Followed::Cmp& g : ah.guards)
            if (written(plusOf(g.a)) || written(plusOf(g.b))) return false;
        for (const auto& [k, v] : fw.heldWrites) {
            Ahead::Update u;
            u.field = k;
            const bool minmax = v->kind == Expr::Min || v->kind == Expr::Max;
            if (minmax) {
                const Expr* self = v->a->kind == Expr::Held && v->a->ki == k ? v->a : v->b->kind == Expr::Held && v->b->ki == k ? v->b : nullptr;
                const Expr* other = self == v->a ? v->b : v->a;
                if (self == nullptr || !guardSide(other) || changed(other) || written(plusOf(other))) return false;
                u.how = v->kind == Expr::Min ? 1 : 2;
                u.with = other;
            } else if (v->kind == Expr::Bin && v->op == '+' && v->cls == CI) {
                const Expr* self = v->a->kind == Expr::Held && v->a->ki == k ? v->a : v->b->kind == Expr::Held && v->b->ki == k ? v->b : nullptr;
                const Expr* c = self == v->a ? v->b : v->a;
                if (self == nullptr || c->kind != Expr::KInt) return false;
                u.how = 3;
                u.constant = c->ki;
            } else {
                return false;
            }
            ah.updates.push_back(u);
        }
        if (ah.updates.size() > 6) return false;
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
        // An item skipped: counting, the normal blend or bilinear sampling alone.
        if (!ah.skips.empty())
            return !ah.hasAcc && fw.heldWrites.empty() && ah.iterSlot < 0 && ah.guards.empty() && (countItems(fw, ah) || blendNormal(fw, ah) || sampleBilinear(fw, ah)) &&
                   !changed(ah.baseA) && !changed(ah.baseB) && !written(plusOf(ah.baseA)) && !written(plusOf(ah.baseB)) && !changed(ah.outA) &&
                   !changed(ah.outB) && !written(plusOf(ah.outA)) && !written(plusOf(ah.outB)) && !changed(ah.p1) && !changed(ah.yA) && !changed(ah.yB) &&
                   !written(plusOf(ah.yA)) && !written(plusOf(ah.yB)) && !changed(ah.sizeW) && !changed(ah.sizeH) && !written(ah.sizeW) && !written(ah.sizeH);
        // Items compared with one word until one is not it (2026-11-15: inkBox's scans), forward or
        // back, nothing else done: the template moves on past those that are.
        if (ah.stopList >= 0) {
            if (ah.hasAcc || !fw.stores.empty() || ah.iterSlot >= 0 || !fw.heldWrites.empty() || ah.stopAt.listA >= 0) return false;
            // A base the same each time round, plus or less a constant: the base and the constant.
            auto basePlus = [&](const Expr* x, const Expr*& base, int64_t& k) {
                base = nullptr, k = 0;
                if (x->kind == Expr::KInt) return k = x->ki, true;
                if (x->kind == Expr::Bin && (x->op == '+' || x->op == '-') && x->cls == CI && x->b->kind == Expr::KInt) {
                    k = x->op == '+' ? x->b->ki : -x->b->ki;
                    x = x->a;
                } else if (x->kind == Expr::Bin && x->op == '+' && x->cls == CI && x->a->kind == Expr::KInt) {
                    k = x->a->ki;
                    x = x->b;
                }
                if (k < -(int64_t{1} << 40) || k > (int64_t{1} << 40) || !fixedOperand(x) || changed(x) || written(x)) return false;
                return base = x, true;
            };
            const Expr* x = ah.stopIndex;
            const Expr* base = nullptr;
            int64_t k = 0, dir = 0;
            Lin at;
            if (linear(x, at, 1, changed, written) && at.coef == 1) {
                base = at.base, k = at.k, dir = 1;
            } else if (x->kind == Expr::Bin && x->op == '-' && x->cls == CI && x->b->kind == Expr::Counter && basePlus(x->a, base, k)) {
                dir = -1;
            } else {
                return false;
            }
            if (changed(ah.stopWord) || written(ah.stopWord)) return false;
            ah.shape = 33;  // kLoopWFindWord
            ah.a = ah.stopList;
            ah.baseA = base, ah.copyWhat[0] = k, ah.copyWhat[1] = dir;
            ah.yA = ah.stopWord;
            return true;
        }
        // Items compared until two differ (2026-11-14: an Image's ==), nothing else done: the
        // template moves on past those the same.
        if (ah.stopAt.listA >= 0) {
            if (ah.hasAcc || !fw.stores.empty() || ah.iterSlot >= 0 || !fw.heldWrites.empty()) return false;
            auto counterPlus = [&](const Expr* x, const Expr*& b) {
                if (x == nullptr || x->cls != CI) return false;
                if (x->kind == Expr::Counter) return b = nullptr, true;
                if (x->kind != Expr::Bin || x->op != '+') return false;
                const Expr* y = x->a->kind == Expr::Counter ? x->b : x->b->kind == Expr::Counter ? x->a : nullptr;
                if (y == nullptr) return false;
                const Expr* core = y;
                if (y->kind == Expr::Bin && y->op == '+' && y->cls == CI) {
                    const Expr* k = y->a->kind == Expr::KInt ? y->a : y->b->kind == Expr::KInt ? y->b : nullptr;
                    if (k == nullptr || k->ki < -(int64_t{1} << 40) || k->ki > (int64_t{1} << 40)) return false;
                    core = k == y->a ? y->b : y->a;
                }
                if (!fixedOperand(core) || changed(core) || written(core)) return false;
                return b = y, true;
            };
            const Expr* baseA = nullptr;
            const Expr* baseB = nullptr;
            if (!counterPlus(ah.stopAt.indexA, baseA) || !counterPlus(ah.stopAt.indexB, baseB) || ah.stopAt.size <= 0) return false;
            ah.shape = 21;  // kLoopWSameItems
            ah.a = ah.stopAt.listA, ah.b = ah.stopAt.listB;
            ah.baseA = baseA, ah.fromBase = baseB;
            ah.copyWhat[0] = ah.stopAt.size;
            return true;
        }
        if (ah.hasAcc) {
            if (!fw.stores.empty()) return false;
            const Expr* v = fw.val[ah.acc];
            if (v->kind != Expr::Bin || v->op != '+' || v->a->kind != Expr::Inv || !(v->a->reg == ah.acc)) return false;
            if (itemAtCounter(v->b, ah.acc.cls)) {
                ah.shape = ah.acc.cls == CF ? 8 /* kLoopFSum */ : 11 /* kLoopWSum */;
                ah.a = v->b->list;
                return true;
            }
            // Fields of items at base + counter (2026-11-13: image[x, y].red): one, or up to four
            // bytes of one item added together (a pixel's four parts).
            std::vector<const Expr*> parts;
            std::function<bool(const Expr*)> fields = [&](const Expr* f) -> bool {
                if (f->kind == Expr::Field) return parts.push_back(f), true;
                return f->kind == Expr::Bin && f->op == '+' && f->cls == CI && fields(f->a) && fields(f->b);
            };
            if (ah.acc.cls == CI && fields(v->b) && !parts.empty() && parts.size() <= 4) {
                bool fit = fieldIndex(parts[0]->b, ah) && !changed(ah.baseA) && !changed(ah.baseB);
                for (const Expr* p : parts)
                    fit = fit && p->list == parts[0]->list && same(p->b, parts[0]->b) && ((p->ki >> 32) == (parts[0]->ki >> 32)) &&
                          (parts.size() == 1 || (p->ki & 0xFF) == 1);
                if (fit && ah.iterSlot >= 0 && ah.baseMode != 0) fit = false;
                if (fit) {
                    ah.shape = ah.iterSlot >= 0 ? 16 /* kLoopWIterSumField */ : 15 /* kLoopWSumField */;
                    ah.a = parts[0]->list;
                    for (const Expr* p : parts) ah.fields.push_back(p->ki);
                    return true;
                }
            }
            // Whole numbers at counter * K + C (2026-11-13): one part of each pixel's four, say.
            int64_t stride = 0, offset = 0;
            if (ah.acc.cls != CI || v->b->kind != Expr::Load || v->b->cls != CI || !strided(v->b->b, stride, offset)) return false;
            ah.shape = 11 /* kLoopWSum */;
            ah.a = v->b->list;
            ah.words[0] = stride, ah.words[1] = offset;
            return true;
        }
        if (mix(fw, ah)) return true;
        if (countItems(fw, ah) && !changed(ah.baseA) && !changed(ah.baseB) && !written(plusOf(ah.baseA)) && !written(plusOf(ah.baseB))) return true;
        if (fw.stores.size() == 1 && fw.stores[0].stem == "hsetsm" && fw.made.count(fw.stores[0].source) != 0 && ah.iterSlot < 0 &&
            mixItems(fw, ah) && !changed(ah.baseA) && !changed(ah.baseB) && !written(plusOf(ah.baseA)) && !written(plusOf(ah.baseB)) &&
            !changed(ah.outA) && !changed(ah.outB) && !written(plusOf(ah.outA)) && !written(plusOf(ah.outB)))
            return true;
        // Items of a value type copied from one list to another (or along the same one), each at a
        // base plus the counter (2026-11-14: copy, crop and a grid's part, kLoopWCopyItems): moved at
        // once, the bytes the steps would move.
        // The counter, or the counter plus what is the same each time round (an operand, or one
        // plus a constant): the base, or null for none.
        auto counterPlus = [&](const Expr* e, const Expr*& b) {
            if (e == nullptr || e->cls != CI) return false;
            if (e->kind == Expr::Counter) return b = nullptr, true;
            if (e->kind != Expr::Bin || e->op != '+') return false;
            const Expr* x = e->a->kind == Expr::Counter ? e->b : e->b->kind == Expr::Counter ? e->a : nullptr;
            if (x == nullptr) return false;
            const Expr* core = x;
            if (x->kind == Expr::Bin && x->op == '+' && x->cls == CI) {
                const Expr* k = x->a->kind == Expr::KInt ? x->a : x->b->kind == Expr::KInt ? x->b : nullptr;
                if (k == nullptr || k->ki < -(int64_t{1} << 40) || k->ki > (int64_t{1} << 40)) return false;
                core = k == x->a ? x->b : x->a;
            }
            if (!fixedOperand(core) || changed(core) || written(core)) return false;
            return b = x, true;
        };
        if (fw.stores.size() == 1 && fw.stores[0].stem == "hsetsm" && ah.iterSlot < 0 && fw.refs.count(fw.stores[0].source) != 0) {
            const Followed::Store& st = fw.stores[0];
            const Followed::Ref from = fw.refs[st.source];
            const Expr* toBase = nullptr;
            const Expr* fromBase = nullptr;
            if (from.list >= 0 && from.width > 0 && from.width == st.width && counterPlus(st.index, toBase) && counterPlus(from.index, fromBase)) {
                ah.shape = 20;  // kLoopWCopyItems
                ah.out = st.list, ah.a = from.list;
                ah.baseA = toBase, ah.fromBase = fromBase;
                ah.copyWhat[0] = st.width, ah.copyWhat[1] = from.what, ah.copyWhat[2] = st.what;
                return true;
            }
        }
        // Items of a value type added one by one at the end of a list, from another list looked
        // at once, at a base plus the counter (2026-11-15: to.add(from[i]); a grid's part and copy,
        // its room made first): as many as the list has room for moved at once (kLoopWAddItems).
        // The list added to is in a slot the region never writes (pushsm's own check, that it is
        // a list of the class's items with room, made by the template for them all).
        if (fw.stores.size() == 1 && fw.stores[0].stem == "pushsm" && ah.iterSlot < 0 && fw.refs.count(fw.stores[0].source) != 0) {
            const Followed::Store& st = fw.stores[0];
            const Followed::Ref from = fw.refs[st.source];
            const Expr* fromBase = nullptr;
            if (from.list >= 0 && st.list >= 0 && untouched(st.list) && counterPlus(from.index, fromBase)) {
                ah.shape = 22;  // kLoopWAddItems
                ah.a = from.list;
                ah.fromBase = fromBase;
                ah.copyWhat[0] = st.list, ah.copyWhat[1] = st.cls, ah.copyWhat[2] = (st.what >> 24) & 0xFFFF;
                return true;
            }
        }
        // One value into items at base + counter (2026-11-13: image[x, y] = c, a grid's in editPixels).
        if (fw.stores.size() == 1 && fw.stores[0].stem == "hsetsm" && ah.iterSlot < 0 && fw.values.count(fw.stores[0].source) != 0 && fieldIndex(fw.stores[0].index, ah) &&
            !changed(ah.baseA) && !changed(ah.baseB) && !written(plusOf(ah.baseA)) && !written(plusOf(ah.baseB))) {
            const Followed::Store& st = fw.stores[0];
            const Followed::Value v = fw.values[st.source];
            ah.shape = 18;  // kLoopWFillItems
            ah.out = st.list;
            ah.fill = {st.width, v.kind, v.at, st.what, v.cls};
            return true;
        }
        // A tap of a convolution (2026-11-15: __mulAdd, which blurred, scaled and a colour grid's
        // convolve take a row at a time): out[ob + j] = a[ab + j] * c, or out[ob + j] plus that, c the
        // same each time round, each base the counter plus what is the same each time round
        // (kLoopFMulAdd).
        if (fw.stores.size() == 1 && fw.stores[0].stem == "hsetf" && ah.iterSlot < 0 && fw.stores[0].index != nullptr) {
            const Followed::Store& st = fw.stores[0];
            const Expr* v = st.value;
            auto mulOf = [&](const Expr* e, const Expr*& load, const Expr*& c) {
                if (e == nullptr || e->kind != Expr::Bin || e->cls != CF || e->op != '*') return false;
                if (e->a->kind == Expr::Load && e->a->cls == CF && invariant(e->b)) return load = e->a, c = e->b, true;
                if (e->b->kind == Expr::Load && e->b->cls == CF && invariant(e->a)) return load = e->b, c = e->a, true;
                return false;
            };
            const Expr* load = nullptr;
            const Expr* c = nullptr;
            bool add = false;
            if (!mulOf(v, load, c) && v->kind == Expr::Bin && v->cls == CF && v->op == '+') {
                auto self = [&](const Expr* e) { return e->kind == Expr::Load && e->cls == CF && e->list == st.list && same(e->b, st.index); };
                const Expr* m = self(v->a) ? v->b : self(v->b) ? v->a : nullptr;
                add = m != nullptr && mulOf(m, load, c);
                if (!add) load = nullptr;
            }
            const Expr* outBase = nullptr;
            const Expr* fromBase = nullptr;
            if (load != nullptr && counterPlus(st.index, outBase) && counterPlus(load->b, fromBase)) {
                ah.shape = 23;  // kLoopFMulAdd
                ah.out = st.list, ah.a = load->list;
                ah.baseA = outBase, ah.fromBase = fromBase;
                ah.p1 = c;
                ah.copyWhat[0] = add ? 1 : 0;
                return true;
            }
        }
        if (apartItems(fw, ah, changed, written)) return true;
        if (gatherPixels(fw, ah, changed, written)) return true;
        if (mulAdd4(fw, ah, changed, written)) return true;
        if (colorFloats(fw, ah, changed, written)) return true;
        if (gather4(fw, ah, changed, written)) return true;
        // Below, a store of a number (hsetf, hsetwi, hsetwb); a value of a value type's (hsetsm,
        // pushsm) has none.
        if (fw.stores.size() != 1 || fw.stores[0].value == nullptr) return false;
        const Followed::Store& st = fw.stores[0];
        if (st.index == nullptr || st.index->kind != Expr::Counter || ah.iterSlot >= 0) return false;  // (a pushsm has none)
        ah.out = st.list;
        const Expr* v = st.value;
        // A field of each item, a byte, into a list of bytes or ints at the same index (2026-11-13:
        // pixels.map { it.red }).
        if ((st.stem == "hsetwb" || st.stem == "hsetwi") && v->kind == Expr::Field && (v->ki & 0xFF) == 1 && fieldIndex(v->b, ah) && ah.baseMode == 0) {
            ah.shape = 17;  // kLoopWField
            ah.a = v->list;
            ah.fields = {v->ki};
            ah.bytesOut = st.stem == "hsetwb";
            return true;
        }
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
        // An operand in words at and at + 1 (stencils.cpp, operand).
        std::function<void(int, const Expr*, int64_t)> operandWords = [&](int at, const Expr* e, int64_t plus) {
            if (e->kind == Expr::KInt) return word(at, 0), word(at + 1, e->ki + plus);
            if (e->kind == Expr::Counter) return word(at, 1), word(at + 1, plus);
            if (e->kind == Expr::Bin) {  // something plus a constant
                const Expr* k = e->a->kind == Expr::KInt ? e->a : e->b;
                return operandWords(at, k == e->a ? e->b : e->a, plus + k->ki);
            }
            if (e->kind == Expr::Held) return word(at, (e->op == 'b' ? 4 : 3) | static_cast<int64_t>(static_cast<uint64_t>(plus) << 16)), word(at + 1, e->ki);
            word(at, 2 | static_cast<int64_t>(static_cast<uint64_t>(plus) << 16));
            // A register of a slot the loop never writes: read from there (2026-11-15), so that
            // the template's words keep no register from being kept in its slot (the loop's own
            // steps, which seldom run, may then hold more values than there are registers).
            bool entered = e->reg.slot >= 0 && templateHead >= 0;
            for (int u = templateHead; entered && u <= templateEnd; u++) entered = in[static_cast<size_t>(u)].kind(e->reg.slot).bits == kEntry;
            if (!entered) return store(at + 1, e->reg);
            LOp l;
            l.stem = "ctxsloti", l.slot = at + 1, l.slot2 = e->reg.slot;
            ops.push_back(l);
        };
        word(16, static_cast<int64_t>(ah.guards.size()));
        for (size_t g = 0; g < ah.guards.size(); g++) {
            const int at = g < 6 ? 17 + 5 * static_cast<int>(g) : 192 + 5 * static_cast<int>(g - 6);  // as stencils.cpp, guardsHold
            word(at, ah.guards[g].op);
            operandWords(at + 1, ah.guards[g].a, 0);
            operandWords(at + 3, ah.guards[g].b, 0);
        }
        word(112, static_cast<int64_t>(ah.classChecks.size()));
        for (size_t k = 0; k < ah.classChecks.size(); k++) word(113 + 2 * static_cast<int>(k), ah.classChecks[k].first), word(114 + 2 * static_cast<int>(k), ah.classChecks[k].second);
        word(70, static_cast<int64_t>(ah.updates.size()));
        for (size_t u = 0; u < ah.updates.size(); u++) {
            const int at = 71 + 4 * static_cast<int>(u);
            word(at, ah.updates[u].field);
            word(at + 1, ah.updates[u].how);
            if (ah.updates[u].how == 3) word(at + 3, ah.updates[u].constant);
            else operandWords(at + 2, ah.updates[u].with, 0);
        }
        if (ah.shape == 14) word(61, ah.mixDivisor), word(62, ah.mixDivideFirst ? 1 : 0);
        if (ah.shape == 19) {
            word(50, ah.baseMode);
            if (ah.baseMode >= 1) operandWords(51, ah.baseA, 0);
            if (ah.baseMode == 2) operandWords(53, ah.baseB, 0);
            word(56, ah.outMode);  // into another list: where it writes (always set: words outlive a template)
            if (ah.outMode >= 1) operandWords(57, ah.outA, 0);
            if (ah.outMode == 2) operandWords(59, ah.outB, 0);
            for (size_t k = 0; k < ah.mixWords.size(); k++) word(96 + static_cast<int>(k), ah.mixWords[k]);
            word(111, static_cast<int64_t>(ah.floatLanes.size()));
            auto bitsOf = [](double d) {
                int64_t b;
                std::memcpy(&b, &d, 8);
                return b;
            };
            for (size_t j = 0; j < ah.floatLanes.size(); j++) {
                const Ahead::FloatLane& lane = ah.floatLanes[j];
                const int at = 128 + 16 * static_cast<int>(j);
                word(at, static_cast<int64_t>(lane.terms.size()) | (lane.c != nullptr ? 0x100 : 0));
                for (size_t k = 0; k < lane.terms.size(); k++) {
                    word(at + 1 + static_cast<int>(k), lane.terms[k].first);
                    // A weight in a slot the loop never writes, read from there (no register kept
                    // for it while the template runs: nine weights are more than there are).
                    const Expr* w = lane.terms[k].second;
                    bool entered = w->kind == Expr::Inv && w->reg.slot >= 0;
                    for (int u = templateHead; entered && u <= templateEnd; u++) entered = in[static_cast<size_t>(u)].kind(w->reg.slot).bits == kEntry;
                    if (entered) {
                        LOp l;
                        l.stem = "ctxslot", l.slot = at + 5 + static_cast<int>(k), l.slot2 = w->reg.slot;
                        ops.push_back(l);
                    } else if (!param(at + 5 + static_cast<int>(k), w)) {
                        return false;
                    }
                }
                word(at + 9, bitsOf(lane.c != nullptr ? lane.c->kf : 0.0));
                word(at + 10, bitsOf(lane.lo));
                word(at + 11, bitsOf(lane.hi));
            }
        }
        if (ah.shape == 26) {
            word(50, ah.baseMode);
            if (ah.baseMode >= 1) operandWords(51, ah.baseA, 0);
            if (ah.baseMode == 2) operandWords(53, ah.baseB, 0);
            for (size_t k = 0; k < ah.mixWords.size(); k++) word(128 + static_cast<int>(k), ah.mixWords[k]);
        }
        if (ah.shape == 21) {
            word(50, ah.baseA != nullptr ? 1 : 0);
            if (ah.baseA != nullptr) operandWords(51, ah.baseA, 0);
            word(63, ah.fromBase != nullptr ? 1 : 0);
            if (ah.fromBase != nullptr) operandWords(64, ah.fromBase, 0);
            word(55, ah.copyWhat[0]);
        }
        if (ah.shape == 33) {
            word(50, ah.baseA != nullptr ? 1 : 0);
            if (ah.baseA != nullptr) operandWords(51, ah.baseA, 0);
            word(53, ah.copyWhat[0]), word(54, ah.copyWhat[1]);
            operandWords(130, ah.yA, 0);
        }
        if (ah.shape == 32) {
            word(50, ah.baseA != nullptr ? 1 : 0);
            if (ah.baseA != nullptr) operandWords(51, ah.baseA, 0);
            word(53, ah.copyWhat[0]);
            for (int q = 0; q < 4; q++) {
                word(130 + 4 * q, ah.fourBase[q] != nullptr ? 1 : 0);
                if (ah.fourBase[q] != nullptr) operandWords(131 + 4 * q, ah.fourBase[q], 0);
                word(133 + 4 * q, ah.fourK[q]);
                if (!param(146 + q, ah.fourWeight[q])) return false;
            }
        }
        if (ah.shape == 31) {
            word(50, ah.baseA != nullptr ? 1 : 0);
            if (ah.baseA != nullptr) operandWords(51, ah.baseA, 0);
            word(53, ah.copyWhat[0]);
            word(63, ah.fromBase != nullptr ? 1 : 0);
            if (ah.fromBase != nullptr) operandWords(64, ah.fromBase, 0);
            word(66, ah.copyWhat[1]);
            word(129, ah.copyWhat[2]);
            word(130, ah.yA != nullptr ? 1 : 0);
            if (ah.yA != nullptr) operandWords(131, ah.yA, 0);
        }
        if (ah.shape == 30) {
            word(50, ah.baseA != nullptr ? 1 : 0);
            if (ah.baseA != nullptr) operandWords(51, ah.baseA, 0);
            word(53, ah.copyWhat[0]);
            word(63, ah.fromBase != nullptr ? 1 : 0);
            if (ah.fromBase != nullptr) operandWords(64, ah.fromBase, 0);
            word(66, ah.copyWhat[1]);
            word(130, ah.yA != nullptr ? 1 : 0);
            if (ah.yA != nullptr) operandWords(131, ah.yA, 0);
            word(133, ah.copyWhat[2]);
        }
        if (ah.shape == 29) {
            word(50, ah.baseMode);
            if (ah.baseMode >= 1) operandWords(51, ah.baseA, 0);
            if (ah.baseMode == 2) operandWords(53, ah.baseB, 0);
            word(56, ah.outMode);
            if (ah.outMode >= 1) operandWords(57, ah.outA, 0);
            if (ah.outMode == 2) operandWords(59, ah.outB, 0);
            word(63, ah.yMode);
            if (ah.yMode >= 1) operandWords(64, ah.yA, 0);
            if (ah.yMode == 2) operandWords(66, ah.yB, 0);
            word(97, ah.mixWords[0]), word(98, ah.mixWords[1]);
            operandWords(130, ah.sizeW, 0);
            operandWords(132, ah.sizeH, 0);
        }
        if (ah.shape == 28) {
            word(50, ah.baseMode);
            if (ah.baseMode >= 1) operandWords(51, ah.baseA, 0);
            if (ah.baseMode == 2) operandWords(53, ah.baseB, 0);
            word(56, ah.outMode);
            if (ah.outMode >= 1) operandWords(57, ah.outA, 0);
            if (ah.outMode == 2) operandWords(59, ah.outB, 0);
            word(97, ah.mixWords[0]), word(98, ah.mixWords[1]), word(130, ah.mixWords[2]);
            if (!param(129, ah.p1)) return false;
        }
        if (ah.shape == 27) {
            word(50, ah.baseA != nullptr ? 1 : 0);
            if (ah.baseA != nullptr) operandWords(51, ah.baseA, 0);
            word(63, ah.fromBase != nullptr ? 1 : 0);
            if (ah.fromBase != nullptr) operandWords(64, ah.fromBase, 0);
            word(66, ah.copyWhat[0]);
            word(129, ah.copyWhat[1]);
        }
        if (ah.shape == 24 || ah.shape == 25) {
            word(50, ah.baseA != nullptr ? 1 : 0);
            if (ah.baseA != nullptr) operandWords(51, ah.baseA, 0);
            word(53, ah.copyWhat[0]);
            word(63, ah.fromBase != nullptr ? 1 : 0);
            if (ah.fromBase != nullptr) operandWords(64, ah.fromBase, 0);
            word(66, ah.copyWhat[1]);
            if (ah.shape == 25) word(97, ah.mixWords[1]), word(98, ah.mixWords[2]);
        }
        if (ah.shape == 23) {
            word(50, ah.baseA != nullptr ? 1 : 0);
            if (ah.baseA != nullptr) operandWords(51, ah.baseA, 0);
            word(63, ah.fromBase != nullptr ? 1 : 0);
            if (ah.fromBase != nullptr) operandWords(64, ah.fromBase, 0);
            word(55, ah.copyWhat[0]);
        }
        if (ah.shape == 22) {
            word(63, ah.fromBase != nullptr ? 1 : 0);
            if (ah.fromBase != nullptr) operandWords(64, ah.fromBase, 0);
            word(55, ah.copyWhat[0]), word(56, ah.copyWhat[1]), word(57, ah.copyWhat[2]);
        }
        if (ah.shape == 20) {
            word(50, ah.baseA != nullptr ? 1 : 0);
            if (ah.baseA != nullptr) operandWords(51, ah.baseA, 0);
            word(63, ah.fromBase != nullptr ? 1 : 0);
            if (ah.fromBase != nullptr) operandWords(64, ah.fromBase, 0);
            word(55, ah.copyWhat[0]), word(56, ah.copyWhat[1]), word(57, ah.copyWhat[2]);
        }
        if (ah.shape == 18) {
            word(50, ah.baseMode);
            if (ah.baseMode >= 1) operandWords(51, ah.baseA, 0);
            if (ah.baseMode == 2) operandWords(53, ah.baseB, 0);
            word(55, ah.fill[0]), word(56, ah.fill[1]), word(57, ah.fill[2]), word(58, ah.fill[3]), word(59, ah.fill[4]);
        }
        if (ah.shape >= 15 && ah.shape <= 17) {
            word(50, ah.baseMode);
            if (ah.baseMode >= 1) operandWords(51, ah.baseA, 0);
            if (ah.baseMode == 2) operandWords(53, ah.baseB, 0);
            for (size_t k = 0; k < ah.fields.size(); k++) word(55 + static_cast<int>(k), ah.fields[k]);
            word(59, static_cast<int64_t>(ah.fields.size()));
        }
        word(0, static_cast<int64_t>(ah.shape));
        word(4, ah.out), word(5, ah.a), word(6, ah.b);
        for (int k = 0; k < 6; k++) word(7 + k, ah.words[k]);
        word(13, ah.bytesOut ? 1 : 0);
        word(14, ah.checkSlot), word(15, ah.checkKind);
        word(47, ah.checkSlot2), word(48, ah.checkKind2);
        if (ah.p1 != nullptr && !param(7, ah.p1)) return false;
        if (ah.p2 != nullptr && !param(8, ah.p2)) return false;
        if (ah.addend != nullptr) store(8, ah.addend->reg);
        if (ah.iterSlot >= 0) {
            word(60, ah.iterSlot);
        } else {
            store(1, next), store(2, left);
        }
        if (ah.hasAcc) store(3, ah.acc);
        LOp run;
        run.stem = "tloop";
        ops.push_back(run);
        if (ah.iterSlot < 0) load(1, next), load(2, left);
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
        // Lists looked at once: none, no loop made ahead. Where the VM's only work is the way to a
        // return or a throw (lookedBeforeEnds), a loop is made ahead as anywhere (2026-11-13): its
        // body may hold no way out but a guard's, which the template checks never fires first.
        if (prologue.empty()) return;
        for (size_t e = 0; e < ins.size(); e++) {
            if (!in[e].known || ins[e].op != Op::Loop) continue;
            const LOp& back = results[e].ops.back();
            const int h = back.jumpTo;
            if (h < 0 || static_cast<size_t>(h) >= e || (ins[static_cast<size_t>(h)].op != Op::RangeNext && ins[static_cast<size_t>(h)].op != Op::IterNextRef)) continue;
            // Entered by falling in from the instruction before alone (or the region's start); a
            // `continue` inside the body goes back to it too (2026-11-15: recognise takes one as a
            // skip, and refuses any other).
            bool alone = true;
            for (size_t i = 0; i < ins.size() && alone; i++) {
                if (!in[i].known || i == e) continue;
                for (const LOp& l : results[i].ops)
                    if (l.jumpTo == h && !(l.stem == "loopback" && i > static_cast<size_t>(h) && i < e)) alone = false;
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
        // Backwards, as a way out may load a value kept in a cell before writing it back
        // (spillHome): what it loads is no read of the way in to it.
        std::set<Reg> here;
        for (auto l = e.stores.rbegin(); l != e.stores.rend(); ++l) {
            for (const Reg& d : l->defs) here.erase(d);
            here.insert(l->uses.begin(), l->uses.end());
        }
        live.insert(here.begin(), here.end());
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
            // A way out's own steps, backwards too (2026-11-15): a value loaded from its cell
            // (spillHome) meets what the stores after it write back, or its register could be
            // one of theirs, overwritten before they store it (a NaN's floori leaving with the
            // stack's copy of x in the register y was loaded into: the VM floored y).
            for (const ExitPoint& e : r.exits) {
                std::set<Reg> wanted;
                for (auto l = e.stores.rbegin(); l != e.stores.rend(); ++l) {
                    for (const Reg& d : l->defs) {
                        note(d);
                        for (const Reg& x : wanted) meet(d, x);
                    }
                    for (const Reg& d : l->defs) wanted.erase(d);
                    wanted.insert(l->uses.begin(), l->uses.end());
                    for (const Reg& u : l->named) note(u);
                }
            }
        }
        // The order (Chaitin's, as Briggs has it): one clashing with fewer than there are
        // registers set aside, those it meets then clashing with one fewer, and again; where
        // none is, the one clashing most set aside all the same, in hope. Registers given the
        // other way round, each the lowest none of those it meets has: one set aside with fewer
        // clashes than registers always finds one, and the hoped-for often do.
        // How often each value is read or written: a float's high half (f8 to f15) costs a
        // shuffle to read and to write, and makes its low half's writes wait (markWhole), so the
        // floats used least are the ones given high halves.
        std::map<Reg, int> traffic;
        for (size_t i = 0; i < ins.size(); i++)
            if (in[i].known)
                for (const LOp& l : results[i].ops) {
                    for (const Reg& u : l.uses) traffic[u]++;
                    for (const Reg& d : l.defs) traffic[d]++;
                }
        std::vector<Reg> order;
        {
            std::map<Reg, int> degree;
            std::set<Reg> left;
            for (const auto& [r, others] : clash) {
                degree[r] = static_cast<int>(others.size());
                left.insert(r);
            }
            while (!left.empty()) {
                // Of those clashing with fewer than there are registers, the one used least: set
                // aside first, it is given its register last, a high half where the low ones
                // are gone.
                auto pick = left.end();
                for (auto it = left.begin(); it != left.end(); ++it)
                    if (degree[*it] < (it->cls == CI ? 9 : kFloatRegisters) && (pick == left.end() || traffic[*it] < traffic[*pick])) pick = it;
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
            const int limit = r.cls == CI ? 9 : kFloatRegisters;
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
            if (r.cls == CF && got < 0) {
                // A low half whose high half nothing it meets has (its writes whole); then any low
                // half; then the high half whose low half's values are used least.
                for (int k = 0; k < 8 && got < 0; k++)
                    if (!taken[static_cast<size_t>(k)] && !taken[static_cast<size_t>(k + 8)]) got = k;
                for (int k = 0; k < 8 && got < 0; k++)
                    if (!taken[static_cast<size_t>(k)]) got = k;
                if (got < 0) {
                    std::vector<int> low(8, 0);
                    for (const Reg& o : clash[r]) {
                        auto g = regs.find(o);
                        if (g != regs.end() && g->second < 8) low[static_cast<size_t>(g->second)] += traffic[o];
                    }
                    for (int k = 8; k < limit; k++)
                        if (!taken[static_cast<size_t>(k)] && (got < 0 || low[static_cast<size_t>(k - 8)] < low[static_cast<size_t>(got - 8)])) got = k;
                }
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
        // And a value written back as one kind at one place and another at another (2026-11-14: an
        // int made a byte in place by toByte(), which checks it and writes nothing): kept in its
        // slot, it would stay the first kind there, as each write leaves it, where a way out after
        // the change wants the second.
        std::map<Reg, std::string> writtenAs;
        for (size_t i = 0; i < ins.size(); i++) {
            if (!in[i].known) continue;
            const Result& res = results[i];
            for (const LOp& l : res.ops) {
                for (const Reg& u : l.uses) used[u]++;
                for (const Reg& d : l.defs) {
                    used[d]++;
                    if (ins[i].op == Op::Numeric || storeOf(res.after, d).stem.empty()) barred.insert(d);
                }
                for (const auto* list : {&l.uses, &l.defs})
                    for (const Reg& r : *list) {
                        if (r.slot < 0) continue;
                        const std::string as = storeOf(res.after, r).stem;
                        if (auto [seen, added] = writtenAs.emplace(r, as); !added && seen->second != as) barred.insert(r);
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
        if (best == nullptr) return spillHome();
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
                    madeBySpill.insert(t);
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
    // Where no value can be kept in its slot: one kept in a cell of the region's own instead
    // (2026-11-13, at the user's word: any value may be moved out to memory), loaded where it is
    // read and stored where it is written, a way out that writes it back loading it first. Of
    // those not wanted where the VM comes in (which loads only from slots) nor used by the steps
    // before the region's first instruction, the one clashing most for how often it is used.
    // False where there is none, or every cell is taken.
    int homes = 0;
    bool spillHome() {
        if (homes >= kRegionHomes) return false;
        std::set<Reg> barred;
        for (int e : entries)
            if (e >= 0 && static_cast<size_t>(e) < liveIn.size()) barred.insert(liveIn[static_cast<size_t>(e)].begin(), liveIn[static_cast<size_t>(e)].end());
        for (size_t i = 0; i < ins.size(); i++) {
            if (!in[i].known) continue;
            for (const ExitPoint& e : results[i].exits)
                if (e.toEntry >= 0 && static_cast<size_t>(e.toEntry) < liveIn.size())
                    barred.insert(liveIn[static_cast<size_t>(e.toEntry)].begin(), liveIn[static_cast<size_t>(e.toEntry)].end());
        }
        for (const LOp& l : headOps)
            for (const auto* list : {&l.named, &l.uses, &l.defs})
                for (const Reg& r : *list) barred.insert(r);
        std::map<Reg, int> used;
        for (size_t i = 0; i < ins.size(); i++) {
            if (!in[i].known) continue;
            for (const LOp& l : results[i].ops) {
                for (const Reg& u : l.uses) used[u]++;
                for (const Reg& d : l.defs) used[d]++;
            }
        }
        const Reg* best = nullptr;
        double bestScore = -1;
        for (const auto& [r, others] : clash) {
            if (madeBySpill.count(r) || housed.count(r) || barred.count(r) || used[r] == 0) continue;  // (not a load of what is kept already)
            const double score = static_cast<double>(others.size()) / (1.0 + used[r]);
            if (score > bestScore) {
                bestScore = score;
                best = &r;
            }
        }
        if (best == nullptr) return false;
        const Reg r = *best;
        housed.insert(r);
        const int64_t cell = homes++;
        const char* load = r.cls == CI ? "homeli" : "homelf";
        const char* store = r.cls == CI ? "homesi" : "homesf";
        auto loadOf = [&](const Reg& t) {
            LOp l;
            l.stem = load, l.named = {t}, l.defs = {t}, l.slot = cell;
            return l;
        };
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
                    madeBySpill.insert(t);
                    if (reads) out.push_back(loadOf(t));
                    if (writes) {
                        LOp keep;
                        keep.stem = store, keep.named = {t}, keep.uses = {t}, keep.slot = cell;
                        after.push_back(keep);
                    }
                    for (auto* list : {&l.named, &l.uses, &l.defs})
                        for (Reg& x : *list)
                            if (x == r) x = t;
                }
                out.push_back(l);
                out.insert(out.end(), after.begin(), after.end());
            }
            res.ops = std::move(out);
            // A way out writing it back: loaded from its cell first.
            for (ExitPoint& e : res.exits) {
                std::vector<LOp> stores;
                for (LOp l : e.stores) {
                    if (std::find(l.uses.begin(), l.uses.end(), r) != l.uses.end()) {
                        const Reg t{-100 - temps++, r.cls};
                        madeBySpill.insert(t);
                        stores.push_back(loadOf(t));
                        for (auto* list : {&l.named, &l.uses, &l.defs})
                            for (Reg& x : *list)
                                if (x == r) x = t;
                    }
                    stores.push_back(l);
                }
                e.stores = std::move(stores);
            }
        }
        return true;
    }
    std::set<Reg> housed, madeBySpill;  // kept in cells; the registers spilling made to load and store
    std::string name(const LOp& l) const {
        std::string s = l.stem;
        for (size_t k = 0; k < l.named.size(); k++) s += "_" + std::to_string(regs.at(l.named[k]) + (k == 0 && l.whole ? 16 : 0));
        return s;
    }

    // Two float registers share each of the processor's XMM registers (stencils.cpp): f0 to f7
    // the low halves, f8 to f15 the high ones. A write to a half keeps the other, and so waits
    // for whatever last wrote the register, though the two values have nothing to do with each
    // other. A step writing a low half whose high half holds nothing wanted after it writes the
    // register whole instead (its stencil's f16 to f23): as quick as with a register to itself.
    void markWhole() {
        static const std::set<std::string> writers = {
            "addf", "subf", "mulf", "divf", "addfk", "subfk", "mulfk", "divfk", "addfkr", "subfkr", "mulfkr", "divfkr",
            "movf", "negf", "absf", "sqrtf", "minf", "maxf", "setfk", "cvtif", "idxf", "idxfk", "idxpf", "valpf",
            "loadf", "numf", "getpf", "iterf", "hidxf", "hidxfk", "hgetpf", "hidxpf", "hvalpf", "ctxlf", "homelf"};
        findLive();
        auto highWanted = [&](const std::set<Reg>& live, int low) {
            for (const Reg& x : live) {
                if (x.cls != CF) continue;
                auto g = regs.find(x);
                if (g != regs.end() && g->second == low + 8) return true;
            }
            return false;
        };
        for (size_t i = 0; i < ins.size(); i++) {
            if (!in[i].known) continue;
            Result& r = results[i];
            // Backwards through its steps, as findLive goes: what is wanted after each.
            std::set<Reg> live;
            if (r.fallExit >= 0) addUses(live, r.exits[static_cast<size_t>(r.fallExit)]);
            else if (r.fallsThrough && i + 1 < ins.size()) live = liveIn[i + 1];
            for (auto l = r.ops.rbegin(); l != r.ops.rend(); ++l) {
                if (l->jumpTo >= 0) live.insert(liveIn[static_cast<size_t>(l->jumpTo)].begin(), liveIn[static_cast<size_t>(l->jumpTo)].end());
                if (l->jumpExit >= 0) addUses(live, r.exits[static_cast<size_t>(l->jumpExit)]);
                if (!l->named.empty() && l->named[0].cls == CF && writers.count(l->stem) &&
                    std::find(l->defs.begin(), l->defs.end(), l->named[0]) != l->defs.end()) {
                    const int k = regs.at(l->named[0]);
                    l->whole = k < 8 && !highWanted(live, k);
                }
                for (const Reg& d : l->defs) live.erase(d);
                if (l->exit >= 0) addUses(live, r.exits[static_cast<size_t>(l->exit)]);
                live.insert(l->uses.begin(), l->uses.end());
            }
        }
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
            const char* given = std::getenv("WW_NATIVE_RATIO");
            return given != nullptr ? std::atoi(given) : 6;
        }();
        // WW_WORTH: what each loop does itself and leaves to the VM, as weighed here.
        static const bool show = std::getenv("WW_WORTH") != nullptr;
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
    // The low halves of the float registers loaded whole (Region::markWhole), before the high
    // halves are loaded into them.
    for (int high = 0; high < 2; high++)
        for (const Reg& reg : r.liveAt(entry)) {
            const int k = r.regs.at(reg);
            if ((reg.cls == CF && k >= 8) != (high == 1)) continue;
            const std::string n = reg.cls == CI ? "loadi_" + std::to_string(k) : "loadf_" + std::to_string(k < 8 ? k + 16 : k);
            if (names != nullptr) names->push_back(n);
            a.add(n).slot = reg.slot;
        }
}

// ---- A profile of the steps native code runs (WW_PROFILE_NATIVE, 2026-11-15) ----

// With WW_PROFILE_NATIVE set (to a function's name, or to anything else for every function),
// each step of a region is counted as it runs (a `count` step before it), and as the program
// ends the regions are told on stderr by how many steps each ran: what each spent most steps on,
// how many of those were values kept in memory loaded and stored (what registers too few cost),
// and its busiest steps in the order they come. What native code's choices are measured by.
struct StepProfile {
    std::string where;                // the function, and where its region starts
    std::vector<std::string> stems;   // each step's name
    std::vector<int> instruction;     // the instruction it does (-1: a way in or out)
    std::deque<uint64_t> counts;      // how often each ran (a deque: each count stays where it is)
};
std::mutex profileLock;
std::deque<StepProfile>* stepProfiles = nullptr;
std::string millions(uint64_t n) {
    char text[32];
    if (n >= 1000000) std::snprintf(text, sizeof text, "%.1f M", static_cast<double>(n) / 1e6);
    else if (n >= 1000) std::snprintf(text, sizeof text, "%.1f k", static_cast<double>(n) / 1e3);
    else std::snprintf(text, sizeof text, "%llu", static_cast<unsigned long long>(n));
    return text;
}
void reportStepProfiles() {
    std::lock_guard<std::mutex> hold(profileLock);
    if (stepProfiles == nullptr) return;
    std::vector<std::pair<uint64_t, const StepProfile*>> made;
    uint64_t all = 0;
    for (const StepProfile& p : *stepProfiles) {
        uint64_t total = 0;
        for (uint64_t c : p.counts) total += c;
        if (total > 0) made.emplace_back(total, &p), all += total;
    }
    std::sort(made.begin(), made.end(), [](const auto& x, const auto& y) { return x.first > y.first; });
    std::fprintf(stderr, "native profile: %s steps run in %zu regions\n", millions(all).c_str(), made.size());
    // A step's kind: its name without the registers it was made for (hidxpb_2_0: hidxpb).
    auto kindOf = [](const std::string& stem) {
        size_t cut = stem.size();
        for (size_t k = 1; k + 1 < stem.size(); k++)
            if (stem[k] == '_' && stem[k + 1] >= '0' && stem[k + 1] <= '9') {
                cut = k;
                break;
            }
        return stem.substr(0, cut);
    };
    auto memory = [](const std::string& kind) {
        return kind == "loadi" || kind == "loadf" || kind == "storei" || kind == "storef" || kind == "storeby" || kind == "homeli" || kind == "homelf" ||
               kind == "homesi" || kind == "homesf";
    };
    for (size_t m = 0; m < made.size() && m < 12; m++) {
        const auto& [total, p] = made[m];
        if (m > 0 && total * 100 < all) break;  // a region of under 1% of all
        std::map<std::string, uint64_t> kinds;
        uint64_t inMemory = 0;
        for (size_t s = 0; s < p->stems.size(); s++) {
            const std::string kind = kindOf(p->stems[s]);
            kinds[kind] += p->counts[s];
            if (memory(kind)) inMemory += p->counts[s];
        }
        std::fprintf(stderr, "== %s: %s steps (%.1f%%), %s of them values kept in memory loaded or stored\n", p->where.c_str(), millions(total).c_str(),
                     100.0 * static_cast<double>(total) / static_cast<double>(all), millions(inMemory).c_str());
        std::vector<std::pair<uint64_t, std::string>> byKind;
        for (const auto& [kind, n] : kinds) byKind.emplace_back(n, kind);
        std::sort(byKind.rbegin(), byKind.rend());
        std::string line = "   by kind:";
        for (size_t k = 0; k < byKind.size() && k < 14 && byKind[k].first > 0; k++) line += " " + byKind[k].second + " " + millions(byKind[k].first) + ",";
        line.pop_back();
        std::fprintf(stderr, "%s\n", line.c_str());
        // The busiest steps (each run at least a tenth as often as the busiest), in their order.
        uint64_t top = 0;
        for (uint64_t c : p->counts) top = std::max(top, c);
        line = "   in order:";
        for (size_t s = 0; s < p->stems.size(); s++)
            if (p->counts[s] > 0 && p->counts[s] * 10 >= top)
                line += " " + p->stems[s] + (p->instruction[s] >= 0 ? "@" + std::to_string(p->instruction[s]) : "") + " " + millions(p->counts[s]) + ",";
        line.pop_back();
        std::fprintf(stderr, "%s\n", line.c_str());
    }
}
StepProfile* newStepProfile(const Region& r) {
    static const char* const asked = std::getenv("WW_PROFILE_NATIVE");
    if (asked == nullptr || (std::strcmp(asked, r.f.name.c_str()) != 0 && std::strcmp(asked, "1") != 0 && std::strcmp(asked, "*") != 0)) return nullptr;
    std::lock_guard<std::mutex> hold(profileLock);
    if (stepProfiles == nullptr) {
        stepProfiles = new std::deque<StepProfile>();  // never freed: read as the program ends
        std::atexit(reportStepProfiles);
    }
    stepProfiles->emplace_back();
    stepProfiles->back().where = r.f.name + ", its loop at " + std::to_string(r.ins[0].at);
    return &stepProfiles->back();
}

// The region's code, and each way in: (the instruction, its address in the code).
std::vector<std::pair<uint32_t, void*>> make(Region& r, std::vector<std::string>* names) {
    r.markWhole();
    Assembler a;
    std::vector<int> labels(r.ins.size());
    for (int& l : labels) l = a.label();
    // With a profile, each step counted as it runs (a `count` step before it).
    StepProfile* const profile = newStepProfile(r);
    int doing = -1;  // the instruction the steps added are for (-1: a way in or out)
    auto add = [&](const std::string& n) -> Step& {
        if (profile != nullptr) {
            profile->counts.push_back(0);
            profile->stems.push_back(n);
            profile->instruction.push_back(doing);
            a.add("count").ki = static_cast<int64_t>(reinterpret_cast<intptr_t>(&profile->counts.back()));
            if (names != nullptr) names->push_back("count");
        }
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
        st.slot2 = l.slot2;  // (ctxslot's slot: a loop template's word read from a slot)
    }
    for (size_t i = 0; i < r.ins.size(); i++) {
        a.at(labels[i]);
        if (!r.in[i].known) continue;
        doing = static_cast<int>(i);
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
            st.ret = l.ret;
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
    doing = -1;
    // The ways out: what the region wrote that the VM may read written back, then where the VM
    // goes on and the stack's top there; or, to an instruction the VM comes back in at, what it
    // loads, and on to it.
    for (const Out& o : outs) {
        a.at(o.label);
        for (const LOp& l : o.exit->stores) {
            Step& st = add(r.name(l));
            // (A dump shows each store's slot, and where the VM goes on: the instruction and the top.)
            if (names != nullptr) names->back() += "@" + std::to_string(l.slot) + (l.slot2 ? ":" + std::to_string(l.slot2) : std::string());
            st.slot = l.slot;
            st.slot2 = l.slot2;
        }
        if (o.exit->toEntry >= 0) {
            loads(a, r, o.exit->toEntry, names, leaveAt(o.exit->toEntry));
            add("jump").jump = labels[static_cast<size_t>(o.exit->toEntry)];
        } else {
            if (names != nullptr) names->push_back("[to " + std::to_string(o.exit->at) + " top " + std::to_string(o.exit->state.top) + "]");
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
        case Op::WithPlace:
        case Op::SetPlace:
        case Op::InvokeImplicit:
        case Op::GetImplicit:
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
        case Op::RBitXorSS:
        case Op::RBitXorSK:
        case Op::RUshrSK:
        case Op::RUshrSS:
        case Op::RAddWrapSS:
        case Op::RAddWrapSK:
        case Op::RSubWrapSS:
        case Op::RSubWrapSK:
        case Op::RMulWrapSS:
        case Op::RMulWrapSK:
        case Op::IterNext:
        case Op::IterNextRef:
        case Op::RGetProp:
        case Op::RSetPropSS:
        case Op::RSetPropSK: return true;
        default: return false;
    }
}
// The same of an instruction: a call the region makes itself (`list.add(x)`, 2026-11-15) too.
bool mayDo(const FunctionProto& f, const Ins& x) {
    if (x.op == Op::RInvoke && u8(x, 4) == 1 && u16(x, 10) < f.constants.size()) {
        const Value& name = f.constants[u16(x, 10)];
        if (name.isObjType(ObjType::String) && asString(name)->utf8() == "add") return true;
    }
    return mayDo(x.op);
}

// The loops of a function's run code: each Loop jumping back, with where it jumps.
struct Candidate {
    int first, last;  // instructions, the loop's start and its Loop
};

// Where a whole function's region starts (2026-11-15): the first instruction past its
// parameters' prologue (defaults given, parameters checked: the VM's) that every call comes to
// once, nothing before it jumping past it, from which a run of instructions a region does
// follows (four, or fewer on the way to a return); -1 where there is none, or where it is a
// loop's start. `mayDoAt(i)`, `endsAt(i)` and `targetOf(i)` (an instruction's jump target as an
// index, or -1) say what the instructions are.
template <typename MayDo, typename Ends, typename TargetOf>
int functionHead(int count, MayDo mayDoAt, Ends endsAt, TargetOf targetOf, const std::vector<int>& loopHeads) {
    int reach = 0;  // the furthest instruction any before jumps to
    for (int i = 0; i < count && i < 64; i++) {
        if (i >= reach && mayDoAt(i)) {
            int run = 0;
            int k = i;
            for (; k < count && run < 4 && mayDoAt(k); k++) run++;
            if (run >= 4 || k == count || endsAt(k)) {
                if (std::find(loopHeads.begin(), loopHeads.end(), i) != loopHeads.end()) return -1;
                return i;
            }
        }
        if (const int t = targetOf(i); t > reach) reach = t;
    }
    return -1;
}

void makeRegionsTimed(FunctionProto& f, const Value* frame, int frameSlots, uint32_t head, std::vector<FunctionProto::NativeRegion>& made);

// A function's run code read as instructions, with where each starts -> its place.
std::vector<Ins> readRun(const FunctionProto& f, std::unordered_map<uint32_t, int>* index) {
    std::vector<Ins> all;
    for (size_t at = 0; at < f.run.size();) {
        const char* name = nullptr;
        const size_t size = runSize(f.run, at + kThreaded, name);
        Ins x;
        x.at = static_cast<uint32_t>(at);
        x.op = opAt(f.run.data() + at + kThreaded);
        x.o = f.run.data() + at + kThreaded + kOpBytes;
        x.next = static_cast<uint32_t>(at + kThreaded + size);
        if (index != nullptr) (*index)[x.at] = static_cast<int>(all.size());
        all.push_back(x);
        at += kThreaded + size;
    }
    return all;
}
// The last instruction of the function's own code, before the R instructions' slow ways: the
// last the translator gave a top (FunctionProto::runTops); -1 where none has one.
int lastOwn(const FunctionProto& f, const std::unordered_map<uint32_t, int>& index) {
    int last = -1;
    for (const auto& [at, top] : f.runTops)
        if (auto i = index.find(at); i != index.end()) last = std::max(last, i->second);
    return last;
}
// Its whole region's start (functionHead), as an index into `all`, or -1.
int functionHeadOf(const FunctionProto& f, const std::vector<Ins>& all, const std::unordered_map<uint32_t, int>& index) {
    std::vector<int> loopHeads;
    for (size_t i = 0; i < all.size(); i++)
        if (all[i].op == Op::Loop)
            if (auto h = index.find(static_cast<uint32_t>(i32(all[i], 0))); h != index.end()) loopHeads.push_back(h->second);
    return functionHead(
        static_cast<int>(all.size()), [&](int i) { return mayDo(f, all[static_cast<size_t>(i)]); },
        [&](int i) { return endsFunction(all[static_cast<size_t>(i)].op); },
        [&](int i) -> int {
            // Where it may jump (not its slow way, which comes back): a stack instruction's
            // target; the run code's jumps', in their operands (registers.cpp, codeOffsets);
            // any other that may jump, past everything.
            const Ins& x = all[static_cast<size_t>(i)];
            int at = targetOperand(x.op) - 1;
            if (x.op == Op::RJumpIfFalse) at = 2;
            else if ((x.op >= Op::RJumpLtSS && x.op <= Op::RJumpEqSK) || (x.op >= Op::RJumpLtISS && x.op <= Op::RJumpEqFSK)) at = 3;
            if (at < 0) return mayJump(x.op) ? INT32_MAX : -1;
            auto to = index.find(static_cast<uint32_t>(i32(x, at)));
            return to == index.end() ? INT32_MAX : to->second;
        },
        loopHeads);
}

// How long making regions took, said as the program ends where WW_TIME_NATIVE is set.
double phaseTimes[4] = {};  // each phase's time, for WW_TIME_NATIVE
long refusals = 0;
double setupTime = 0;
struct NativeTimes {
    double seconds = 0;
    long functions = 0;
    ~NativeTimes() {
        if (std::getenv("WW_TIME_NATIVE") != nullptr)
            std::fprintf(stderr, "  hints %.1f, analysis %.1f, liveness %.1f, registers %.1f ms; %ld refused; setup %.1f ms\n", phaseTimes[0] * 1000, phaseTimes[1] * 1000, phaseTimes[2] * 1000, phaseTimes[3] * 1000, refusals, setupTime * 1000);
        if (std::getenv("WW_TIME_NATIVE") != nullptr) std::fprintf(stderr, "native code: %ld functions, %.1f ms making regions\n", functions, seconds * 1000);
    }
} nativeTimes;

void makeRegions(FunctionProto& f, const Value* frame, int frameSlots, uint32_t head, std::vector<FunctionProto::NativeRegion>& out) {
    const auto start = std::chrono::steady_clock::now();
    makeRegionsTimed(f, frame, frameSlots, head, out);
    const double took = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    nativeTimes.seconds += took;
    static const bool each = std::getenv("WW_TIME_NATIVE") != nullptr && std::string(std::getenv("WW_TIME_NATIVE")) == "each";
    if (each && took > 0.0003) std::fprintf(stderr, "  %.1f ms %s (%zu bytes of run code)\n", took * 1000, f.name.c_str(), f.run.size());
    nativeTimes.functions++;
}

void makeRegionsTimed(FunctionProto& f, const Value* frame, int frameSlots, uint32_t head, std::vector<FunctionProto::NativeRegion>& out) {
    if (!enabled()) return;
    const auto setupStart = std::chrono::steady_clock::now();
    std::unordered_map<uint32_t, int> index;
    const std::vector<Ins> all = readRun(f, &index);
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
    // A whole function's region (2026-11-15): from its head to the end of its own code, its
    // loops (the outermost: their starts and their Loops) left out.
    std::set<std::pair<int, int>> leftOut;
    if (head == f.nativeCallHead) {
        auto h = index.find(head);
        const int last = lastOwn(f, index);
        if (h == index.end() || last <= h->second) return;
        for (const Candidate& l : loops) {
            bool inner = false;
            for (const auto& [from, to] : leftOut) inner = inner || (l.first >= from && l.last <= to);
            if (!inner && l.first > h->second && l.last <= last) leftOut.emplace(l.first, l.last);
        }
        loops.assign(1, Candidate{h->second, last});
    }
    std::vector<Candidate> made;
    setupTime += std::chrono::duration<double>(std::chrono::steady_clock::now() - setupStart).count();
    static const char* const dump = std::getenv("WW_DUMP_NATIVE");
    const bool dumping = dump != nullptr && (f.name == dump || std::strcmp(dump, "*") == 0);
    for (const Candidate& c : loops) {
        bool inside = false;
        for (const Candidate& m : made) inside = inside || (c.first <= m.last && c.last >= m.first);
        if (inside) continue;
        if (all[static_cast<size_t>(c.first)].at != head) continue;  // another loop's: made as it starts
        auto headTop = tops.find(all[static_cast<size_t>(c.first)].at);
        if (headTop == tops.end() || headTop->second < 0) {
            if (dumping) std::fprintf(stderr, "== %s: no region for the loop at %u: the stack's top there not known\n", f.name.c_str(), head);
            continue;
        }
        // At a glance, before the analysis: a loop that starts with an instruction no region does,
        // or has too many of them for what it would do itself (Region::worthIt), stays the VM's.
        {
            int can = 0, cannot = 0;
            const std::vector<bool> ending = endingRuns(c.last - c.first + 1, [&](int i) { return all[static_cast<size_t>(c.first + i)].op; });
            // (A whole function's: its instructions but its loops', those on the way to a return
            // counted where a region does them: a function's work is mostly on its way to one.)
            const bool function = head == f.nativeCallHead;
            for (int i = c.first; i <= c.last; i++) {
                bool inLoop = false;
                for (const auto& [from, to] : leftOut) inLoop = inLoop || (i >= from && i <= to);
                const bool may = mayDo(f, all[static_cast<size_t>(i)]);
                if (inLoop || (ending[static_cast<size_t>(i - c.first)] && !(function && may))) continue;
                (may ? can : cannot)++;
            }
            if (!mayDo(f, all[static_cast<size_t>(c.first)]) || can < (function ? 8 : 3) || cannot * 6 > can) {
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
        for (const auto& [from, to] : leftOut) {
            for (int k = from; k <= to; k++) r.outside.insert(k - c.first);
            r.seeds.insert(to + 1 - c.first);
        }
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
                const char* asked = std::getenv("WW_NATIVE_TEMPLATES");
                return asked == nullptr || std::string(asked) != "0";
            }();
            if (ahead) {
                r.makeAhead();
                if (r.templates > 0) r.findLive();
            }
            if (r.twoItemsAtOnce()) r.findLive();
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
            for (size_t w = 0; w < ways.size(); w++) out.push_back(FunctionProto::NativeRegion{ways[w].first, ways[w].second, misses, w == 0, !r.prologue.empty()});
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
    {
        static const bool wholeFunctions = [] {
            const char* asked = std::getenv("WW_NATIVE_FUNCTIONS");
            return asked == nullptr || std::string(asked) != "0";
        }();
        std::unordered_map<uint32_t, int> index;
        const std::vector<Ins> all = readRun(f, &index);
        const int fh = wholeFunctions ? functionHeadOf(f, all, index) : -1;
        const int last = fh >= 0 ? lastOwn(f, index) : -1;
        if (fh >= 0 && last > fh) {
            f.nativeCallHead = all[static_cast<size_t>(fh)].at;
            for (int i = fh; i <= last; i++)
                if (all[static_cast<size_t>(i)].op == Op::Return || all[static_cast<size_t>(i)].op == Op::ReturnInit)
                    f.nativeReturns.push_back(all[static_cast<size_t>(i)].at);
            if (f.nativeReturns.empty()) f.nativeCallHead = UINT32_MAX;
        }
    }
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
    return !f.nativeHeads.empty() || f.nativeCallHead != UINT32_MAX;
}

#if WW_NATIVE
const bool hooked = (findNativeLoops = &findLoops, makeNativeRegions = &makeRegions, true);
#endif

}  // namespace

}  // namespace ww::native
