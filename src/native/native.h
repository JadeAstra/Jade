// Native code (M9.5, docs/native-plan.html): machine code made as a program runs, from the
// register stencils that tools/stencils read out of Clang's object file of src/native/stencils.cpp
// when ww.exe was built. This part knows the stencils and puts them together: a region's steps
// copied end to end into code memory, their gaps filled in. What a region is made of, and when,
// is the VM's (registers.cpp and vm.cpp, from step 3 on).
#pragma once

#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

namespace ww::native {

// What fills a stencil's gap (src/native/stencils.cpp names them ww_next and so on).
// HoleSlotAt and HoleSlot2At (ww_slotat, ww_slot2at, 2026-11-14) are the byte offset of the
// frame's slot ww_slot (ww_slot2) itself, built into the instruction that reads or writes it;
// HoleContextAt (ww_ctxat, 2026-11-19) so too: the offset of the region's context (from the
// Assembler's contextSlot), which the VM lays in the frame as the region runs; and HoleListAt
// (ww_listat) that of the context's words for the list ww_slot looks at once (lookAt). Every
// other gap is the distance to what fills it (a word placed after the code, or code).
enum HoleKind : uint8_t {
    HoleNext, HoleJump, HoleExit, HoleFloat, HoleInt, HoleSlot, HoleRet, HoleAttention, HoleSlot2, HoleData, HoleSlotAt, HoleSlot2At,
    HoleContextAt, HoleListAt
};
// A gap: where its 32 bits are in the stencil, and how many bytes of the instruction follow them
// (what its offset is counted from: REL32_1 to REL32_5); for HoleSlotAt and HoleSlot2At, what is
// added to the slot's offset instead (8: its payload).
struct Hole {
    uint32_t offset;
    HoleKind kind;
    uint32_t after;
    uint8_t size;  // HoleData: a constant of the stencil's own, placed beside the code
    uint8_t data[16];
};
struct StencilCode {
    const char* name;
    const uint8_t* code;
    uint32_t size;
    const Hole* holes;
    uint32_t holeCount;
};

// Whether this ww.exe has stencils: built with clang-cl on Windows (elsewhere, until the tool
// reads ELF, ww runs without native code).
bool available();
size_t stencilCount();
const StencilCode* stencil(std::string_view name);  // null where there is none of that name (the table is in name order)

// A region being put together: steps, each a stencil with what fills its gaps, and labels
// jumps go to. Finished, it is machine code in code memory, run as `RegionFn`.
struct Step {
    const StencilCode* code = nullptr;
    double kf = 0;
    int64_t ki = 0;
    int64_t slot = 0;
    int64_t slot2 = 0;
    int64_t ret = 0;
    int jump = -1;  // a label
    int exit = -1;  // the label of this step's own way out, or -1 for the region's
};

class Assembler {
public:
    // The step of the stencil named; an InternalError (std::runtime_error) where there is none.
    Step& add(std::string_view stencilName);
    int label() { return nextLabel_++; }
    void at(int label);
    // The region's machine code, the way out being steps given by `exitSteps` (written after the
    // rest; the overflow and the flag go there). Null where the code memory could not be had.
    void* finish(const std::vector<Step>& exitSteps, size_t* bytes = nullptr);
    // Where a label is in the finished code (a region's other ways in).
    void* address(int label) const;
    // The frame's slot the region's context is kept in as it runs (FunctionProto::
    // nativeContextSlot), which every ww_ctxat gap is filled with the offset of.
    int64_t contextSlot = -1;

private:
    std::vector<Step> steps_;
    std::vector<std::pair<int, size_t>> labels_;
    int nextLabel_ = 0;
    uint8_t* code_ = nullptr;
    std::vector<size_t> starts_;
};

constexpr int kRegionLists = 6;
constexpr int kRegionFields = 16;

// What a region is given besides the frame's slots: the VM's flag (a collection or another
// thread waiting), and for its reads and writes of objects, the core of a parallel section it
// runs on (0 outside one) and the object lent to that core, which only the VM reads. Laid in
// the frame itself, past the function's slots (FunctionProto::nativeContextSlot, 2026-11-19: a
// register held its address before), where the stencils find its words at offsets built in.
constexpr int kRegionHomes = 32;
struct RegionContext {
    const volatile uint8_t* attention;
    uint64_t core;
    const void* band;
    // The lists a region looks at once as it starts (src/native/stencils.cpp, lookAt): for each,
    // its items' address, how many may be read, how many changed, and what they are.
    uint64_t lists[kRegionLists * 4];
    // A loop template's words (stencils.cpp, runLoop): its shape, counter, lists and parameters;
    // from 16, the guards it checks first and what it reads items at (2026-11-13); from 128, the
    // float mixes of kLoopWMixItems; from 192, guards past the sixth (2026-11-15).
    uint64_t t[256];
    // The VM's globals, as a region reads one (B3: the class called to make a value).
    const void* globals;
    // The fields a region looks at once as it starts (stencils.cpp, lookp): each a value, two words;
    // and of those it writes (lookpw), where each is written back as it leaves (0: nowhere).
    uint64_t held[kRegionFields * 2];
    uint64_t heldAt[kRegionFields];
    // Cells a region keeps values in where it has more at once than registers and a value has no
    // slot of its own to be kept in (regions.cpp, spillOne): never read before written.
    uint64_t homes[kRegionHomes];
    // The cells of the variables the function the region runs in captures (its closure's
    // upvalues), as a region reads one (R_GET_UPVALUE, 2026-11-13).
    const void* upvalues;
    // Each byte's value over 255.0, a / 255.0 as a double (unitTable), for loop templates that
    // divide a byte by 255 (2026-11-15): the same number, read, not worked out each time.
    const double* units;
    // The VM the region runs on, and its way of making a value of a value type in a box for a
    // region (2026-11-15, stencils.cpp's mkbox): the box, its bytes zero; null where a collection
    // is due, where the region leaves the making to the VM.
    void* vm;
    void* (*makeBox)(void* vm, const void* cls);
    // The running frame's first place (VM::placeOf(frame, 0)), where a value of a value type is
    // made for a variable (stencils.cpp's mkplace; place k is k strides down); null where the
    // frame has none.
    uint8_t* places;
    // How many times a key has gone into or out of any Table (value.h's tableKeysChanged): a
    // bare name found to be no member as the region was made is none while it is unchanged.
    const uint64_t* keys;
    // The VM's way of making room in a packed list a region adds to (2026-11-16, stencils.cpp's
    // push steps): false where a collection is due or the list is not packed, where the region
    // leaves the add to the VM.
    bool (*makeRoom)(void* vm, const void* list);
    // The VM's way of looking a key up in a map or a set for a region (2026-11-16, sets-maps-plan
    // part 1, stencils.cpp's mapget and has steps): 1 found (a map's value copied to `out`), 0 not,
    // -1 where the VM looks it up instead.
    int (*findKey)(const void* table, const void* key, void* out);
    // And of setting a map's key or adding to a set (part 2, mapset and setadd): 1 done (a set:
    // added), 0 a set that had it, -1 where the VM does it instead.
    int (*setKey)(void* vm, const void* table, const void* key, const void* value);
};
// The 256 numbers a region's `units` points at, made once.
const double* unitTable();

// A region's machine code, run: the frame's slots, its context, then the registers (all 0 as it
// starts). It gives back where the VM goes on: the instruction, and the stack's top above it.
#if defined(__clang__)
#define WW_REGION_CALL __attribute__((preserve_none))
#else
#define WW_REGION_CALL
#endif
using Lanes = double __attribute__((vector_size(16)));  // two float registers (stencils.cpp)
using RegionFn = int64_t(WW_REGION_CALL *)(void* slots, int64_t, int64_t, int64_t, int64_t, int64_t, int64_t, int64_t, int64_t,
                                                          int64_t, int64_t, int64_t, Lanes, Lanes, Lanes, Lanes, Lanes, Lanes, Lanes, Lanes);
// Its context is the frame's own, past the function's slots (FunctionProto::nativeContextSlot),
// laid there by the caller (contextAt): the stencils find it at an offset from the frame built
// into them (2026-11-19: a register held its address before).
inline RegionContext& contextAt(void* slots, int64_t contextSlot) {
    return *reinterpret_cast<RegionContext*>(static_cast<uint8_t*>(slots) + contextSlot * 16);
}
inline int64_t runRegion(void* code, void* slots) {
    const Lanes z{0, 0};
    return reinterpret_cast<RegionFn>(code)(slots, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, z, z, z, z, z, z, z, z);
}

// Whether regions are made (ww --interpret, or WW_NATIVE=0, says not).
bool enabled();
void setEnabled(bool on);

// The build's check (ww --native-check): every stencil shaped as the copier shapes it, and
// regions put together and run, their answers beside C++'s; what is wrong, or "".
std::string check();

}  // namespace ww::native
