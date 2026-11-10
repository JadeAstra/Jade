// M9.5 step 1: the register stencils proven on their own. bench/numeric.jade's mandelbrot, as a
// region would make it, put together from the stencils tools/stencils read out of Clang's object
// file (stencils.h, made by build.bat): copied into code memory end to end, its gaps filled in,
// run, and timed beside the spike's numbers (bench/spike: the VM 93 ms, registers 14.4, C++ 13.4).
//
// Its registers, as a region would give them out: ints i0 size, i1 limit, i2 inside, i3 py, i4 px,
// i5 n; floats f0 zr, f1 zi, f2 cr, f3 ci, f4 and f5 the two temporaries, f6 a conversion, f7 size
// as a float. Slots 1 and 2 hold size and limit as it begins; slot 3 gets inside as it ends.

#include <windows.h>

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <vector>

namespace jade::native {
enum HoleKind : uint8_t { HoleNext, HoleJump, HoleExit, HoleFloat, HoleInt, HoleSlot, HoleRet, HoleAttention };
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
}  // namespace jade::native

#include "stencils.h"

using namespace jade::native;

namespace {

struct Slot {
    uint8_t kind;
    uint8_t pad[7];
    union {
        int64_t i;
        double d;
    };
};

volatile uint8_t attention = 0;

// One stencil of the region, with what fills its gaps.
struct Step {
    std::string name;
    double kf = 0;
    int64_t ki = 0;
    int64_t slot = 0;
    int64_t ret = 0;
    int jump = -1;  // a label
};

struct Region {
    std::vector<Step> steps;
    std::map<int, size_t> labels;  // label -> the step it is at
    int nextLabel = 0;
    int label() { return nextLabel++; }
    void at(int l) { labels[l] = steps.size(); }
    Step& add(const std::string& name) {
        steps.push_back(Step{name});
        return steps.back();
    }
};

const StencilCode& find(const std::string& name) {
    for (const StencilCode& s : kStencils)
        if (name == s.name) return s;
    std::fprintf(stderr, "no stencil %s\n", name.c_str());
    std::exit(1);
}

// A stencil as it is copied: its code and its gaps, which the copier may change.
struct Piece {
    std::vector<uint8_t> code;
    std::vector<Hole> holes;
};
bool holeAt(const Piece& p, uint32_t offset, HoleKind kind) {
    for (const Hole& h : p.holes)
        if (h.offset == offset && h.kind == kind) return true;
    return false;
}
// A stencil made ready to go where the next one follows it: a last jump to the next left out; a
// conditional jump to the next followed by a jump to the target turned round (the condition
// reversed, to the target), so that going on falls through and only the jump is taken.
Piece shaped(const StencilCode& s, bool nextFollows) {
    Piece p{std::vector<uint8_t>(s.code, s.code + s.size), std::vector<Hole>(s.holes, s.holes + s.holeCount)};
    // A conditional jump over a jump to a gap (`jno +5; jmp exit`, or the same with a 32-bit jump)
    // made one jump on the other condition, straight to the gap, the bytes left over no-ops: one
    // branch where Clang wrote two. Offsets do not move, so the stencil's own jumps still land.
    for (Hole& h : p.holes) {
        const uint32_t j = h.offset - 1;  // the `jmp`'s opcode
        if (h.after != 0 || j < 2 || p.code[j] != 0xE9) continue;
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

using RegionFn = int64_t(__attribute__((preserve_none)) *)(Slot*, int64_t, int64_t, int64_t, int64_t, int64_t, int64_t, double, double, double,
                                                          double, double, double, double, double);

// The region's machine code: the steps copied end to end, then the way out (a `ret` giving -1),
// then the words the gaps reach (constants, slots, the flag's address).
RegionFn assemble(Region& r, size_t& bytes) {
    r.add("ret").ret = -1;  // the way out, which the overflow and the flag go to
    const size_t exitStep = r.steps.size() - 1;
    std::vector<Piece> pieces;
    std::vector<size_t> start(r.steps.size() + 1);
    size_t at = 0, words = 0;
    for (size_t i = 0; i < r.steps.size(); i++) {
        pieces.push_back(shaped(find(r.steps[i].name), i + 1 < r.steps.size()));
        start[i] = at;
        at += pieces.back().code.size();
        for (const Hole& h : pieces.back().holes)
            if (h.kind >= HoleFloat) words++;
    }
    start[r.steps.size()] = at;
    const size_t codeSize = (at + 15) & ~size_t{15};
    bytes = codeSize + words * 8;
    auto* mem = static_cast<uint8_t*>(VirtualAlloc(nullptr, bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    std::memset(mem, 0xCC, bytes);
    size_t word = codeSize;
    auto rel32 = [&](size_t place, uint32_t after, size_t target) {
        const int64_t d = static_cast<int64_t>(target) - static_cast<int64_t>(place + 4 + after);
        const int32_t v = static_cast<int32_t>(d);
        std::memcpy(mem + place, &v, 4);
    };
    for (size_t i = 0; i < r.steps.size(); i++) {
        const Step& st = r.steps[i];
        const Piece& p = pieces[i];
        std::memcpy(mem + start[i], p.code.data(), p.code.size());
        for (const Hole& hole : p.holes) {
            const size_t place = start[i] + hole.offset;
            switch (hole.kind) {
                case HoleNext: rel32(place, hole.after, start[i + 1]); break;
                case HoleJump: rel32(place, hole.after, start[r.labels.at(st.jump)]); break;
                case HoleExit: rel32(place, hole.after, start[exitStep]); break;
                default: {
                    uint64_t v = 0;
                    if (hole.kind == HoleFloat) std::memcpy(&v, &st.kf, 8);
                    else if (hole.kind == HoleInt) v = static_cast<uint64_t>(st.ki);
                    else if (hole.kind == HoleSlot) v = static_cast<uint64_t>(st.slot);
                    else if (hole.kind == HoleRet) v = static_cast<uint64_t>(st.ret);
                    else v = reinterpret_cast<uint64_t>(&attention);
                    std::memcpy(mem + word, &v, 8);
                    rel32(place, hole.after, word);
                    word += 8;
                }
            }
        }
    }
    if (const char* dump = std::getenv("PROOF_DUMP")) {
        if (FILE* f = std::fopen(dump, "wb")) {
            std::fwrite(mem, 1, at, f);
            std::fclose(f);
        }
    }
    DWORD old;
    VirtualProtect(mem, bytes, PAGE_EXECUTE_READ, &old);
    FlushInstructionCache(GetCurrentProcess(), mem, bytes);
    return reinterpret_cast<RegionFn>(mem);
}

// mandelbrot(size, limit), as its run code goes.
Region mandelbrot() {
    Region r;
    const int outer = r.label(), done = r.label();
    r.add("loadi_0").slot = 1;  // size
    r.add("loadi_1").slot = 2;  // limit
    r.add("setik_2").ki = 0;    // inside
    r.add("setik_3").ki = 0;    // py
    r.add("cvtif_7_0");         // size as a float
    r.at(outer);
    r.add("jgeii_3_0").jump = done;
    // ci = 2.0 * py / size - 1.0
    r.add("cvtif_6_3");
    r.add("mulfk_6_6").kf = 2.0;
    r.add("divf_3_6_7");
    r.add("subfk_3_3").kf = 1.0;
    r.add("setik_4").ki = 0;  // px
    const int inner = r.label(), innerDone = r.label();
    r.at(inner);
    r.add("jgeii_4_0").jump = innerDone;
    r.add("cvtif_6_4");
    r.add("mulfk_6_6").kf = 2.0;
    r.add("divf_2_6_7");
    r.add("subfk_2_2").kf = 1.5;
    r.add("setfk_0").kf = 0.0;
    r.add("setfk_1").kf = 0.0;
    r.add("setik_5").ki = 0;
    const int loop = r.label(), loopDone = r.label();
    r.at(loop);
    r.add("jgeii_5_1").jump = loopDone;  // while (n < limit and ...)
    r.add("mulf_4_0_0");
    r.add("mulf_5_1_1");
    r.add("addf_4_4_5");
    r.add("jgtfk_4").jump = loopDone;  // ... zr * zr + zi * zi <= 4.0)
    r.steps.back().kf = 4.0;
    r.add("mulf_4_0_0");  // t = zr * zr - zi * zi + cr
    r.add("mulf_5_1_1");
    r.add("subf_4_4_5");
    r.add("addf_4_4_2");
    r.add("mulfkr_5_0").kf = 2.0;  // zi = 2.0 * zr * zi + ci
    r.add("mulf_5_5_1");
    r.add("addf_1_5_3");
    r.add("movf_0_4");     // zr = t
    r.add("inci_5");  // n += 1
    r.add("loopback").jump = loop;
    r.at(loopDone);
    const int notInside = r.label();
    r.add("jneii_5_1").jump = notInside;
    r.add("inci_2");
    r.at(notInside);
    r.add("inci_4");
    r.add("loopback").jump = inner;
    r.at(innerDone);
    r.add("inci_3");
    r.add("loopback").jump = outer;
    r.at(done);
    r.add("storei_2").slot = 3;
    r.add("ret").ret = 0;
    return r;
}

}  // namespace

int main() {
    Region r = mandelbrot();
    size_t bytes = 0;
    const RegionFn fn = assemble(r, bytes);
    std::vector<Slot> frame(16);
    double best = 1e30;
    int64_t got = 0, inside = 0;
    for (int run = 0; run < 5; run++) {
        std::memset(frame.data(), 0, frame.size() * sizeof(Slot));
        frame[1].kind = 2, frame[1].i = 400;
        frame[2].kind = 2, frame[2].i = 100;
        const auto s = std::chrono::steady_clock::now();
        got = fn(frame.data(), 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0);
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - s).count();
        if (ms < best) best = ms;
        inside = frame[3].i;
    }
    std::printf("mandelbrot from stencils: %.1f ms, inside %lld (61854 expected), gave %lld; %zu steps, %zu bytes of machine code; %zu stencils known\n", best,
                static_cast<long long>(inside), static_cast<long long>(got), r.steps.size(), bytes, sizeof(kStencils) / sizeof(kStencils[0]));
    return inside == 61854 && got == 0 ? 0 : 1;
}
