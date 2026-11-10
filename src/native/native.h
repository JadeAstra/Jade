// Native code (M9.5, docs/native-plan.html): machine code made as a program runs, from the
// register stencils that tools/stencils read out of Clang's object file of src/native/stencils.cpp
// when jade.exe was built. This part knows the stencils and puts them together: a region's steps
// copied end to end into code memory, their gaps filled in. What a region is made of, and when,
// is the VM's (registers.cpp and vm.cpp, from step 3 on).
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace jade::native {

// What fills a stencil's gap (src/native/stencils.cpp names them jade_next and so on).
enum HoleKind : uint8_t { HoleNext, HoleJump, HoleExit, HoleFloat, HoleInt, HoleSlot, HoleRet, HoleAttention };
// A gap: where its 32 bits are in the stencil, and how many bytes of the instruction follow them
// (what its offset is counted from: REL32_1 to REL32_5).
struct Hole {
    uint32_t offset;
    HoleKind kind;
    uint32_t after;
};
struct StencilCode {
    const char* name;
    const uint8_t* code;
    uint32_t size;
    const Hole* holes;
    uint32_t holeCount;
};

// Whether this jade.exe has stencils: built with clang-cl on Windows (elsewhere, until the tool
// reads ELF, jade runs without native code).
bool available();
size_t stencilCount();
const StencilCode* stencil(std::string_view name);  // null where there is none of that name

// A region being put together: steps, each a stencil with what fills its gaps, and labels
// jumps go to. Finished, it is machine code in code memory, run as `RegionFn`.
struct Step {
    const StencilCode* code = nullptr;
    double kf = 0;
    int64_t ki = 0;
    int64_t slot = 0;
    int64_t ret = 0;
    int jump = -1;  // a label
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

private:
    std::vector<Step> steps_;
    std::vector<std::pair<int, size_t>> labels_;
    int nextLabel_ = 0;
};

// The build's check (jade --native-check): every stencil shaped as the copier shapes it, and
// regions put together and run, their answers beside C++'s; what is wrong, or "".
std::string check();

}  // namespace jade::native
