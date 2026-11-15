// Native code (M9.5): the stencils ww.exe carries, and putting them together into machine code.
// See native.h, and docs/native-plan.html.

#include "native.h"

#include <cmath>
#include <cstdlib>
#include <cstring>
#include <stdexcept>
#include <unordered_map>

#include <algorithm>
#include <cstddef>

#if WW_NATIVE
#include "../bytecode.h"
#include "../value.h"
#include "layout.h"
#include "stencils.h"  // kStencils, made by tools/stencils when ww.exe is built, in name order
#endif

#if WW_NATIVE && defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#elif WW_NATIVE
#include <sys/mman.h>
#endif

namespace ww::native {

#if WW_NATIVE
// A slot's kind as the stencils write it: the VM's.
static_assert(static_cast<int>(ValueKind::Int) == 2 && static_cast<int>(ValueKind::Float) == 3, "src/native/stencils.cpp writes these kinds");
static_assert(sizeof(Value) == 16, "a stencil's slot is 16 bytes");

// What the stencils read of objects (layout.h), as the VM's structures have it.
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Winvalid-offsetof"
namespace L = layout;
static_assert(sizeof(Value) == L::kValueSize && offsetof(Value, as) == L::kValuePayload, "layout.h: a value");
static_assert(static_cast<int>(ValueKind::Bool) == L::kKindBool && static_cast<int>(ValueKind::Int) == L::kKindInt &&
                  static_cast<int>(ValueKind::Float) == L::kKindFloat && static_cast<int>(ValueKind::Obj) == L::kKindObj &&
                  static_cast<int>(ValueKind::Byte) == L::kKindByte,
              "layout.h: the kinds of value");
static_assert(offsetof(Obj, type) == L::kObjType && offsetof(Obj, core) == L::kObjCore && offsetof(Obj, frozen) == L::kObjFrozen,
              "layout.h: an object");
static_assert(sizeof(ObjType) == 1 && static_cast<int>(ObjType::List) == L::kTypeList && static_cast<int>(ObjType::Bytes) == L::kTypeBytes,
              "layout.h: the kinds of object");
static_assert(offsetof(ObjList, pack) == L::kListPack && sizeof(std::unique_ptr<PackedItems>) == 8, "layout.h: a list");
static_assert(offsetof(PackedItems, store) == L::kPackStore && offsetof(PackedItems, nullable) == L::kPackNullable &&
                  offsetof(PackedItems, count) == L::kPackCount && offsetof(PackedItems, block) == L::kPackBlock && sizeof(ListStore) == 1 &&
                  static_cast<int>(ListStore::Ints) == L::kStoreInts && static_cast<int>(ListStore::Floats) == L::kStoreFloats,
              "layout.h: a list's packed items");
static_assert(offsetof(PackedItems, width) == L::kPackWidth && sizeof(PackedItems::width) == 4 && offsetof(PackedItems, itemClass) == L::kPackItemClass &&
                  static_cast<int>(ListStore::Items) == L::kStoreItems,
              "layout.h: a list of a value type's items");
static_assert(static_cast<int>(ValueKind::Small) == L::kKindSmall && offsetof(Value, refDepth) == L::kRefDepth &&
                  offsetof(Value, refOffset) == L::kRefOffset && offsetof(Value, refIndex) == L::kRefIndex &&
                  offsetof(PackedItems, itemType) == L::kPackItemType && offsetof(PackedItems, typeSlot) == L::kPackTypeSlot &&
                  sizeof(PackedItems::typeSlot) == 4 && static_cast<int>(ObjType::Places) == L::kTypePlaces,
              "layout.h: values of value types");
static_assert(offsetof(ObjBytes, data) == L::kBytesData, "layout.h: bytes");
static_assert(static_cast<int>(ValueKind::Null) == L::kKindNull, "layout.h: null");
static_assert(offsetof(ObjInstance, cls) == L::kInstClass, "layout.h: an object's class");
static_assert(offsetof(ObjUpvalue, location) == L::kUpvalueLocation && sizeof(Value*) == 8, "layout.h: a captured variable's cell");
static_assert(offsetof(ObjPlace, cls) == L::kPlaceClass && offsetof(ObjPlace, type) == L::kPlaceType && sizeof(ObjPlace) == L::kPlaceData,
              "layout.h: a frame's place");
static_assert(static_cast<int>(ListStore::Narrow) == L::kStoreNarrow && static_cast<int>(Narrow::Byte) == L::kNarrowByte &&
                  offsetof(PackedItems, narrow) == L::kPackNarrow && sizeof(Narrow) == 1,
              "layout.h: a list of narrow items");
static_assert(offsetof(ObjIterator, source) == L::kIterSource && offsetof(ObjIterator, index) == L::kIterIndex &&
                  sizeof(ObjIterator::index) == 8 && static_cast<int>(ObjType::Iterator) == L::kTypeIterator,
              "layout.h: an iterator");
static_assert(offsetof(ObjInstance, shape) == L::kInstShape && sizeof(ObjInstance) == L::kInstData &&
                  static_cast<int>(ObjType::Instance) == L::kTypeInstance,
              "layout.h: an object of a class");
static_assert(static_cast<int>(ValueKind::Undefined) == L::kKindUndefined && static_cast<int>(ValueKind::Ref) == L::kKindRef &&
                  kTypeInt == L::kTypeIdInt && kTypeFloat == L::kTypeIdFloat,
              "layout.h: kinds and types");
#pragma clang diagnostic pop

bool available() { return true; }
size_t stencilCount() { return sizeof(kStencils) / sizeof(kStencils[0]); }

const StencilCode* stencil(std::string_view name) {
    const StencilCode* const end = kStencils + stencilCount();
    const StencilCode* found = std::lower_bound(kStencils, end, name, [](const StencilCode& s, std::string_view n) { return std::string_view(s.name) < n; });
    return found != end && std::string_view(found->name) == name ? found : nullptr;
}

#else

bool available() { return false; }
size_t stencilCount() { return 0; }
const StencilCode* stencil(std::string_view) { return nullptr; }

#endif

namespace {
bool regionsOn = [] {
    const char* asked = std::getenv("WW_NATIVE");
    return asked == nullptr || std::string(asked) != "0";
}();
}  // namespace

bool enabled() { return regionsOn && available(); }
void setEnabled(bool on) { regionsOn = on; }

namespace {

// ---- Code memory ----

// Memory for machine code: taken from the system as the program's data is, written while it may
// not run, and made runnable, and no longer writable, before it runs. A region has pages of its
// own, so that none is written while another region in it may be running on another core.
uint8_t* writableCode(size_t bytes) {
#if WW_NATIVE && defined(_WIN32)
    return static_cast<uint8_t*>(VirtualAlloc(nullptr, bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
#elif WW_NATIVE
    void* at = mmap(nullptr, bytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    return at == MAP_FAILED ? nullptr : static_cast<uint8_t*>(at);
#else
    (void)bytes;
    return nullptr;
#endif
}
bool makeRunnable(uint8_t* code, size_t bytes) {
#if WW_NATIVE && defined(_WIN32)
    DWORD old;
    if (!VirtualProtect(code, bytes, PAGE_EXECUTE_READ, &old)) return false;
    FlushInstructionCache(GetCurrentProcess(), code, bytes);
    return true;
#elif WW_NATIVE
    if (mprotect(code, bytes, PROT_READ | PROT_EXEC) != 0) return false;
    __builtin___clear_cache(reinterpret_cast<char*>(code), reinterpret_cast<char*>(code + bytes));
    return true;
#else
    (void)code, (void)bytes;
    return false;
#endif
}

// ---- Shaping a stencil where it is copied ----

struct Piece {
    std::vector<uint8_t> code;
    std::vector<Hole> holes;
};
bool holeAt(const Piece& p, uint32_t offset, HoleKind kind) {
    for (const Hole& h : p.holes)
        if (h.offset == offset && h.kind == kind) return true;
    return false;
}

// A stencil as it goes into a region. A conditional jump over a jump to a gap (`jno +5; jmp
// exit`, or the same with a 32-bit jump) is made one jump on the other condition, straight to
// the gap, the bytes left over no-ops: one branch where Clang wrote two; no offset moves, so the
// stencil's own jumps still land. Where the next stencil follows: its last jump to the next is
// left out, and a conditional jump to the next followed by a jump to the target is turned round
// (the condition reversed, to the target), so that going on falls through.
Piece shaped(const StencilCode& s, bool nextFollows) {
    Piece p{std::vector<uint8_t>(s.code, s.code + s.size), std::vector<Hole>(s.holes, s.holes + s.holeCount)};
    for (Hole& h : p.holes) {
        if (h.kind > HoleExit || h.after != 0 || h.offset < 3) continue;
        const uint32_t j = h.offset - 1;  // the `jmp`'s opcode
        if (p.code[j] != 0xE9) continue;
        if ((p.code[j - 2] & 0xF0) == 0x70 && p.code[j - 1] == 0x05) {
            const uint8_t cc = static_cast<uint8_t>((p.code[j - 2] & 0x0F) ^ 1);
            p.code[j - 2] = 0x0F, p.code[j - 1] = static_cast<uint8_t>(0x80 | cc);
            std::memmove(&p.code[j], &p.code[j + 1], 4);
            p.code[j + 4] = 0x90;
            h.offset = j;
        } else if (j >= 6 && p.code[j - 6] == 0x0F && (p.code[j - 5] & 0xF0) == 0x80 && p.code[j - 4] == 0x05 && p.code[j - 3] == 0 &&
                   p.code[j - 2] == 0 && p.code[j - 1] == 0) {
            p.code[j - 5] ^= 1;
            std::memmove(&p.code[j - 4], &p.code[j + 1], 4);
            std::memset(&p.code[j], 0x90, 5);
            h.offset = j - 4;
        }
    }
    if (!nextFollows) return p;
    const uint32_t n = s.size;
    if (n >= 5 && p.code[n - 5] == 0xE9 && holeAt(p, n - 4, HoleNext)) {
        p.code.resize(n - 5);
        std::erase_if(p.holes, [&](const Hole& h) { return h.offset == n - 4; });
    } else if (n >= 11 && p.code[n - 11] == 0x0F && (p.code[n - 10] & 0xF0) == 0x80 && holeAt(p, n - 9, HoleNext) && p.code[n - 5] == 0xE9 &&
               holeAt(p, n - 4, HoleJump)) {
        p.code[n - 10] ^= 1;  // jl becomes jge, and so on
        for (Hole& h : p.holes)
            if (h.offset == n - 9) h.kind = HoleJump;
        p.code.resize(n - 5);
        std::erase_if(p.holes, [&](const Hole& h) { return h.offset == n - 4; });
    }
    return p;
}

}  // namespace

// ---- Putting a region together ----

Step& Assembler::add(std::string_view stencilName) {
    const StencilCode* code = stencil(stencilName);
    if (code == nullptr) throw std::runtime_error("native code: no stencil " + std::string(stencilName));
    steps_.push_back(Step{});
    steps_.back().code = code;
    return steps_.back();
}

void Assembler::at(int label) { labels_.emplace_back(label, steps_.size()); }

void* Assembler::finish(const std::vector<Step>& exitSteps, size_t* bytes) {
    std::vector<Step> all = steps_;
    const size_t exitAt = all.size();
    all.insert(all.end(), exitSteps.begin(), exitSteps.end());
    auto labelled = [&](int label) {
        for (const auto& [l, step] : labels_)
            if (l == label) return step;
        throw std::runtime_error("native code: a jump to a label never placed");
    };
    // Where a loop goes back to starts on 32 bytes, as compilers place a loop: the time a loop
    // takes then does not change with the length of the code before it (the processor fetches
    // and caches its instructions by aligned blocks). The bytes before are no-ops, run once
    // each time the loop is come to from before.
    std::vector<bool> loopHead(all.size() + 1, false);
    if (const StencilCode* const back = stencil("loopback"))
        for (const Step& st : all)
            if (st.code == back && st.jump >= 0) loopHead[labelled(st.jump)] = true;
    std::vector<Piece> pieces;
    std::vector<size_t> start(all.size() + 1);
    std::vector<std::pair<size_t, size_t>> padding;  // where no-ops go, and how many bytes
    size_t at = 0, words = 0, constants = 0;
    for (size_t i = 0; i < all.size(); i++) {
        // The last step of the code and of the way out goes on to nothing after it.
        const bool nextFollows = i + 1 < all.size() && i + 1 != exitAt;
        pieces.push_back(shaped(*all[i].code, nextFollows));
        if (loopHead[i] && (at & 31) != 0) {
            const size_t aligned = (at + 31) & ~size_t{31};
            padding.emplace_back(at, aligned - at);
            at = aligned;
        }
        start[i] = at;
        at += pieces.back().code.size();
        for (const Hole& h : pieces.back().holes) {
            if (h.kind == HoleData) constants++;
            else if (h.kind >= HoleFloat && h.kind != HoleSlotAt && h.kind != HoleSlot2At) words++;
        }
    }
    start[all.size()] = at;
    // After the code, the stencils' constants, 16 bytes each and on 16 (as SSE reads a mask),
    // then the gaps' words.
    const size_t codeSize = (at + 15) & ~size_t{15};
    const size_t total = codeSize + constants * 16 + words * 8;
    uint8_t* mem = writableCode(total);
    if (mem == nullptr) return nullptr;
    std::memset(mem, 0xCC, total);
    for (const auto& [from, n] : padding) {
        // The fewest no-ops: 8-byte ones (nopl 0(%rax,%rax,1)), then 1-byte ones.
        static const uint8_t long8[8] = {0x0F, 0x1F, 0x84, 0x00, 0x00, 0x00, 0x00, 0x00};
        size_t k = 0;
        for (; k + 8 <= n; k += 8) std::memcpy(mem + from + k, long8, 8);
        for (; k < n; k++) mem[from + k] = 0x90;
    }
    size_t constant = codeSize;
    size_t word = codeSize + constants * 16;
    auto rel32 = [&](size_t place, uint32_t after, size_t target) {
        const int64_t d = static_cast<int64_t>(target) - static_cast<int64_t>(place + 4 + after);
        const int32_t v = static_cast<int32_t>(d);
        std::memcpy(mem + place, &v, 4);
    };
    for (size_t i = 0; i < all.size(); i++) {
        const Step& st = all[i];
        const Piece& p = pieces[i];
        std::memcpy(mem + start[i], p.code.data(), p.code.size());
        for (const Hole& hole : p.holes) {
            const size_t place = start[i] + hole.offset;
            switch (hole.kind) {
                case HoleNext: rel32(place, hole.after, start[i + 1]); break;
                case HoleJump: rel32(place, hole.after, start[labelled(st.jump)]); break;
                case HoleExit: rel32(place, hole.after, st.exit >= 0 ? start[labelled(st.exit)] : start[exitAt]); break;
                case HoleSlotAt:
                case HoleSlot2At: {
                    // The slot's own place, built into the instruction: its offset in the frame.
                    const int64_t slot = hole.kind == HoleSlotAt ? st.slot : st.slot2;
                    const int64_t v = slot * static_cast<int64_t>(sizeof(Value)) + static_cast<int64_t>(hole.after);
                    if (slot < 0 || v > INT32_MAX) throw std::runtime_error("native code: a slot too far for an instruction to reach");
                    const int32_t v32 = static_cast<int32_t>(v);
                    std::memcpy(mem + place, &v32, 4);
                    break;
                }
                case HoleData:
                    std::memset(mem + constant, 0, 16);
                    std::memcpy(mem + constant, hole.data, hole.size);
                    rel32(place, hole.after, constant);
                    constant += 16;
                    break;
                default: {
                    uint64_t v = 0;
                    if (hole.kind == HoleFloat) std::memcpy(&v, &st.kf, 8);
                    else if (hole.kind == HoleInt) v = static_cast<uint64_t>(st.ki);
                    else if (hole.kind == HoleSlot) v = static_cast<uint64_t>(st.slot);
                    else if (hole.kind == HoleSlot2) v = static_cast<uint64_t>(st.slot2);
                    else if (hole.kind == HoleRet) v = static_cast<uint64_t>(st.ret);
                    else throw std::runtime_error("native code: a gap of a kind no step fills");
                    std::memcpy(mem + word, &v, 8);
                    rel32(place, hole.after, word);
                    word += 8;
                }
            }
        }
    }
    if (!makeRunnable(mem, total)) return nullptr;
    if (bytes != nullptr) *bytes = total;
    code_ = mem;
    starts_.clear();
    for (const auto& [label, step] : labels_) {
        if (starts_.size() <= static_cast<size_t>(label)) starts_.resize(static_cast<size_t>(label) + 1, SIZE_MAX);
        starts_[static_cast<size_t>(label)] = start[step];
    }
    return mem;
}

void* Assembler::address(int label) const {
    if (code_ == nullptr || label < 0 || static_cast<size_t>(label) >= starts_.size() || starts_[static_cast<size_t>(label)] == SIZE_MAX)
        throw std::runtime_error("native code: the address of a label not placed");
    return code_ + starts_[static_cast<size_t>(label)];
}

// ---- The build's check ----

#if WW_NATIVE
namespace {

volatile uint8_t checkFlag = 0;

int64_t run(void* code, Value* slots) {
    RegionContext context{&checkFlag, 0, nullptr, {}, {}, nullptr, {}, {}, {}, nullptr};
    return runRegion(code, slots, &context);
}

// std::vector as the stencils read it (layout.h): its first item's address, then the end's.
bool vectorAsRead() {
    ObjBytes bytes;
    bytes.data = {1, 2, 3};
    const uint8_t* words[2];
    std::memcpy(words, reinterpret_cast<const uint8_t*>(&bytes) + layout::kBytesData, sizeof words);
    if (words[0] != bytes.data.data() || words[1] != bytes.data.data() + 3) return false;
    // A packed list's block: its first item, its end, and the end of its room (a list's add in
    // native code writes there and moves the end on, 2026-11-15).
    PackedItems pack;
    pack.block.reserve(10);
    pack.block.resize(3);
    const uint8_t* block[3];
    std::memcpy(block, reinterpret_cast<const uint8_t*>(&pack) + layout::kPackBlock, sizeof block);
    if (block[0] != pack.block.data() || block[1] != pack.block.data() + 3 || block[2] != pack.block.data() + pack.block.capacity()) return false;
    // A list's own items, where the list keeps them (a private member, so asked of the list).
    ObjList list;
    return reinterpret_cast<const uint8_t*>(&list.values()) - reinterpret_cast<const uint8_t*>(&list) == layout::kListItems;
}

// The way out the check's regions take: give back -1.
std::vector<Step> exitGiving(int64_t value) {
    Step ret;
    ret.code = stencil("ret");
    ret.ret = value;
    return {ret};
}

// numeric's mandelbrot(size, limit), as a region would be made of its run code: slots 1 and 2
// size and limit, slot 3 inside. See bench/spike/stencil/proof.cpp.
void* mandelbrot(Assembler& a) {
    const int outer = a.label(), done = a.label(), inner = a.label(), innerDone = a.label();
    const int loop = a.label(), loopDone = a.label(), notInside = a.label();
    a.add("loadi_0").slot = 1;
    a.add("loadi_1").slot = 2;
    a.add("setik_2").ki = 0;
    a.add("setik_3").ki = 0;
    a.add("cvtif_7_0");
    a.at(outer);
    a.add("jgeii_3_0").jump = done;
    a.add("cvtif_6_3");
    a.add("mulfk_6_6").kf = 2.0;
    a.add("divf_3_6_7");
    a.add("subfk_3_3").kf = 1.0;
    a.add("setik_4").ki = 0;
    a.at(inner);
    a.add("jgeii_4_0").jump = innerDone;
    a.add("cvtif_6_4");
    a.add("mulfk_6_6").kf = 2.0;
    a.add("divf_2_6_7");
    a.add("subfk_2_2").kf = 1.5;
    a.add("setfk_0").kf = 0.0;
    a.add("setfk_1").kf = 0.0;
    a.add("setik_5").ki = 0;
    a.at(loop);
    a.add("jgeii_5_1").jump = loopDone;
    a.add("mulf_4_0_0");
    a.add("mulf_5_1_1");
    a.add("addf_4_4_5");
    Step& escape = a.add("jgtfk_4");
    escape.jump = loopDone, escape.kf = 4.0;
    a.add("mulf_4_0_0");
    a.add("mulf_5_1_1");
    a.add("subf_4_4_5");
    a.add("addf_4_4_2");
    a.add("mulfkr_5_0").kf = 2.0;
    a.add("mulf_5_5_1");
    a.add("addf_1_5_3");
    a.add("movf_0_4");
    a.add("inci_5");
    a.add("loopback").jump = loop;
    a.at(loopDone);
    a.add("jneii_5_1").jump = notInside;
    a.add("inci_2");
    a.at(notInside);
    a.add("inci_4");
    a.add("loopback").jump = inner;
    a.at(innerDone);
    a.add("inci_3");
    a.add("loopback").jump = outer;
    a.at(done);
    a.add("storei_2").slot = 3;
    a.add("ret").ret = 0;
    return a.finish(exitGiving(-1));
}

int64_t mandelbrotInC(int64_t size, int64_t limit) {
    int64_t inside = 0;
    for (int64_t py = 0; py < size; py++) {
        const double ci = (py * 2.0) / static_cast<double>(size) - 1.0;
        for (int64_t px = 0; px < size; px++) {
            const double cr = (px * 2.0) / static_cast<double>(size) - 1.5;
            double zr = 0, zi = 0;
            int64_t n = 0;
            while (n < limit && zr * zr + zi * zi <= 4.0) {
                const double t = zr * zr - zi * zi + cr;
                zi = 2.0 * zr * zi + ci;
                zr = t;
                n++;
            }
            if (n == limit) inside++;
        }
    }
    return inside;
}

}  // namespace

std::string check() {
    if (stencilCount() == 0) return "no stencils";
    for (size_t i = 1; i < stencilCount(); i++)
        if (!(std::string_view(kStencils[i - 1].name) < std::string_view(kStencils[i].name))) return "the stencils are not in name order";
    if (!vectorAsRead()) return "std::vector is not laid out as layout.h reads it";
    // Every stencil shaped both ways, its gaps inside it.
    for (const StencilCode& s : kStencils)
        for (bool follows : {false, true}) {
            const Piece p = shaped(s, follows);
            for (const Hole& h : p.holes)
                if (h.offset + 4 > p.code.size()) return std::string("stencil ") + s.name + ": a gap past its end";
        }
    // A region's answer beside C++'s.
    std::vector<Value> slots(8);
    {
        Assembler a;
        void* code = mandelbrot(a);
        if (code == nullptr) return "no code memory";
        slots[1] = Value::integer(60), slots[2] = Value::integer(80);
        const int64_t gave = run(code, slots.data());
        if (gave != 0 || slots[3].kind != ValueKind::Int || slots[3].as.i != mandelbrotInC(60, 80))
            return "mandelbrot(60, 80) from stencils gave " + std::to_string(slots[3].as.i) + ", C++ " + std::to_string(mandelbrotInC(60, 80));
        // The VM's flag set: the region leaves at the next loop's end.
        checkFlag = 1;
        const int64_t left = run(code, slots.data());
        checkFlag = 0;
        if (left != -1) return "the flag did not take the region out";
    }
    {
        // An int's overflow leaves for the VM before the int is changed.
        Assembler a;
        a.add("loadi_0").slot = 1;
        a.add("inci_0");
        a.add("inci_0");
        a.add("storei_0").slot = 2;
        a.add("ret").ret = 0;
        std::vector<Step> out = exitGiving(-1);
        Step store;
        store.code = stencil("storei_0");
        store.slot = 3;
        out.insert(out.begin(), store);
        void* code = a.finish(out);
        if (code == nullptr) return "no code memory";
        slots[1] = Value::integer(INT64_MAX - 1);
        slots[2] = Value::integer(0);
        const int64_t gave = run(code, slots.data());
        if (gave != -1 || slots[3].as.i != INT64_MAX || slots[2].as.i != 0) return "an overflow did not leave as it should";
    }
    return "";
}
#else
std::string check() { return available() ? "" : ""; }
#endif

}  // namespace ww::native
