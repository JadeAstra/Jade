// Native code (M9.5, docs/native-plan.html): machine code made as a program runs, from the
// register stencils that tools/stencils read out of Clang's object file of src/native/stencils.cpp
// when ww.exe was built. This part knows the stencils and puts them together: a region's steps
// copied end to end into code memory, their gaps filled in. What a region is made of, and when,
// is the VM's (registers.cpp and vm.cpp, from step 3 on).
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace ww::native {

// What fills a stencil's gap (src/native/stencils.cpp names them ww_next and so on).
enum HoleKind : uint8_t { HoleNext, HoleJump, HoleExit, HoleFloat, HoleInt, HoleSlot, HoleRet, HoleAttention, HoleSlot2, HoleData };
// A gap: where its 32 bits are in the stencil, and how many bytes of the instruction follow them
// (what its offset is counted from: REL32_1 to REL32_5).
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
// runs on (0 outside one) and the object lent to that core, which only the VM reads.
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
    // float mixes of kLoopWMixItems.
    uint64_t t[192];
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
};

// A region's machine code, run: the frame's slots, its context, then the registers (all 0 as it
// starts). It gives back where the VM goes on: the instruction, and the stack's top above it.
#if defined(__clang__)
#define WW_REGION_CALL __attribute__((preserve_none))
#else
#define WW_REGION_CALL
#endif
using RegionFn = int64_t(WW_REGION_CALL *)(void* slots, RegionContext* context, int64_t, int64_t, int64_t, int64_t, int64_t,
                                                          int64_t, int64_t, int64_t, int64_t, int64_t, double, double, double, double, double,
                                                          double, double, double);
inline int64_t runRegion(void* code, void* slots, RegionContext* context) {
    return reinterpret_cast<RegionFn>(code)(slots, context, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0);
}

// Whether regions are made (ww --interpret, or WW_NATIVE=0, says not).
bool enabled();
void setEnabled(bool on);

// The build's check (ww --native-check): every stencil shaped as the copier shapes it, and
// regions put together and run, their answers beside C++'s; what is wrong, or "".
std::string check();

}  // namespace ww::native
