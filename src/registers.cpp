// The code the VM runs (M6.7), made from a function's stack code once that is final.
//
// The compiler writes stack code, which the verifier follows knowing the stack's depth before
// each instruction (bytecode.cpp). From it, this makes the code the VM runs, with the source line
// of each of its bytes and the ranges of the function's rights in its offsets
// (docs/M6.7-plan.md, "The register machine"). For now it is the same instructions.
#include "bytecode.h"

namespace astra {

void translate(FunctionProto& f, const std::vector<int>& depths) {
    (void)depths;
    f.run = f.code;
    f.runLines = f.lines;
    f.runRights.clear();
    for (const FunctionProto::Rights& r : f.rights) f.runRights.emplace_back(r.start, r.end);
}

}  // namespace astra
