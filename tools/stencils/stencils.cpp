// tools/stencils (M9.5, docs/native-plan.html): reads the object file Clang made of
// src/native/stencils.cpp and writes the stencils as a C++ table: each one's machine code and its
// gaps, where in the code each is and what fills it. Run when jade.exe is built; no third-party
// code. It refuses anything it does not know (a reference to a name that is no gap, a kind of
// relocation it does not fill), so that a change in what Clang makes shows when building.
//
//   stencils <object file> <header written>
//
// The object file is COFF (Windows, x64; clang-cl /Gy) or ELF (Linux, x86-64; clang
// -ffunction-sections). A stencil is a function whose name starts with "st_", each in a section
// of its own.

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <map>
#include <sstream>
#include <string>
#include <vector>

namespace {

[[noreturn]] void fail(const std::string& why) {
    std::fprintf(stderr, "stencils: %s\n", why.c_str());
    std::exit(1);
}

uint16_t u16(const std::vector<uint8_t>& b, size_t at) { return static_cast<uint16_t>(b[at] | (b[at + 1] << 8)); }
uint32_t u32(const std::vector<uint8_t>& b, size_t at) { return u16(b, at) | (static_cast<uint32_t>(u16(b, at + 2)) << 16); }

// The gaps a stencil may have, by the name it refers to (see src/native/stencils.cpp).
const std::map<std::string, std::string> kHoles = {
    {"jade_next", "HoleNext"},   {"jade_jump", "HoleJump"},   {"jade_exit", "HoleExit"},
    {"jade_kf", "HoleFloat"},    {"jade_ki", "HoleInt"},      {"jade_slot", "HoleSlot"},
    {"jade_ret", "HoleRet"},     {"jade_attention", "HoleAttention"}, {"jade_slot2", "HoleSlot2"},
};

struct Hole {
    uint32_t offset;   // of the 32 bits filled in, in the stencil
    std::string kind;
    uint32_t after;    // bytes of the instruction after the 32 bits (REL32_1 to _5), which the offset is counted from
    std::vector<uint8_t> data;  // HoleData: the constant Clang put in read-only data (a float's bound, a sign's mask)
};
struct Stencil {
    std::string name;
    std::vector<uint8_t> code;
    std::vector<Hole> holes;
};

}  // namespace

// An x64 COFF object file (Windows, clang-cl).
void readCoff(const std::vector<uint8_t>& obj, std::vector<Stencil>& stencils) {
    const uint16_t sectionCount = u16(obj, 2);
    const uint32_t symbolTable = u32(obj, 8), symbolCount = u32(obj, 12);
    const uint16_t optionalHeader = u16(obj, 16);
    const size_t sections = 20 + optionalHeader;
    const size_t strings = symbolTable + static_cast<size_t>(symbolCount) * 18;

    auto symbolName = [&](uint32_t index) {
        const size_t at = symbolTable + static_cast<size_t>(index) * 18;
        if (u32(obj, at) == 0) {
            const size_t s = strings + u32(obj, at + 4);
            return std::string(reinterpret_cast<const char*>(&obj[s]));
        }
        std::string name(reinterpret_cast<const char*>(&obj[at]), 8);
        return name.substr(0, name.find('\0'));
    };

    // Each stencil's section: the defined external symbol starting "st_" in it.
    std::map<uint16_t, std::string> stencilOf;
    for (uint32_t i = 0; i < symbolCount; i++) {
        const size_t at = symbolTable + static_cast<size_t>(i) * 18;
        const int16_t section = static_cast<int16_t>(u16(obj, at + 12));
        const uint8_t storage = obj[at + 16], aux = obj[at + 17];
        const std::string name = symbolName(i);
        if (storage == 2 && section > 0 && name.rfind("st_", 0) == 0) stencilOf[static_cast<uint16_t>(section)] = name.substr(3);
        i += aux;
    }

    for (uint16_t s = 1; s <= sectionCount; s++) {
        const size_t h = sections + static_cast<size_t>(s - 1) * 40;
        const uint32_t characteristics = u32(obj, h + 36);
        const bool code = (characteristics & 0x20) != 0;
        auto named = stencilOf.find(s);
        const uint32_t rawSize = u32(obj, h + 16), raw = u32(obj, h + 20), relocations = u32(obj, h + 24);
        const uint16_t relocationCount = u16(obj, h + 32);
        std::string sectionName(reinterpret_cast<const char*>(&obj[h]), 8);
        sectionName = sectionName.substr(0, sectionName.find('\0'));
        if (named == stencilOf.end()) {
            // Nothing else may hold code a stencil could need; data it needs is a gap of its own,
            // or a constant copied with it (below).
            if (code && rawSize > 0) fail("code that is no stencil, in section " + std::to_string(s));
            continue;
        }
        Stencil st;
        st.name = named->second;
        st.code.assign(obj.begin() + raw, obj.begin() + raw + rawSize);
        for (uint16_t r = 0; r < relocationCount; r++) {
            const size_t at = relocations + static_cast<size_t>(r) * 10;
            const uint32_t offset = u32(obj, at), symbol = u32(obj, at + 4);
            const uint16_t type = u16(obj, at + 8);
            const std::string target = symbolName(symbol);
            // IMAGE_REL_AMD64_REL32 and REL32_1 to REL32_5: 32 bits, counted from the end of the
            // instruction, which is that many bytes past them.
            if (type < 4 || type > 9) fail("stencil " + st.name + ": a relocation of kind " + std::to_string(type) + ", which is not filled in");
            auto hole = kHoles.find(target);
            if (hole != kHoles.end()) {
                st.holes.push_back(Hole{offset, hole->second, static_cast<uint32_t>(type - 4), {}});
                continue;
            }
            // A constant of read-only data (Clang's __real@ and __xmm@): its bytes go with the
            // stencil, at most 16, and are placed beside the region's code.
            const size_t sym = symbolTable + static_cast<size_t>(symbol) * 18;
            const int16_t dataSection = static_cast<int16_t>(u16(obj, sym + 12));
            const uint32_t value = u32(obj, sym + 8);
            if (dataSection <= 0) fail("stencil " + st.name + " refers to " + target + ", which is no gap");
            const size_t dh = sections + static_cast<size_t>(dataSection - 1) * 40;
            const uint32_t dataCharacteristics = u32(obj, dh + 36);
            const uint32_t dataSize = u32(obj, dh + 16), dataRaw = u32(obj, dh + 20);
            // Initialized data, not writable, with no relocations of its own.
            if ((dataCharacteristics & 0x40) == 0 || (dataCharacteristics & 0x80000000) != 0 || u16(obj, dh + 32) != 0 || value >= dataSize ||
                dataSize - value > 16)
                fail("stencil " + st.name + " refers to " + target + ", which is no gap nor a constant it can carry");
            std::vector<uint8_t> bytes(obj.begin() + dataRaw + value, obj.begin() + dataRaw + dataSize);
            st.holes.push_back(Hole{offset, "HoleData", static_cast<uint32_t>(type - 4), bytes});
        }
        stencils.push_back(std::move(st));
    }
}

uint64_t u64(const std::vector<uint8_t>& b, size_t at) { return u32(b, at) | (static_cast<uint64_t>(u32(b, at + 4)) << 32); }

// An x86-64 ELF relocatable object file (Linux, clang), its stencils each in a section of its own
// (-ffunction-sections), compiled without position-independent code: every gap a PC-relative
// reference to its name (R_X86_64_PC32, or PLT32 for a jump), whose addend says how many bytes
// of the instruction follow the 32 bits (-4 less them). A constant of Clang's is in a section of
// merged constants (.rodata.cst8, .cst16), reached through the section's own symbol, its place
// there the addend plus 4: Clang's SSE instructions end with the 32 bits.
void readElf(const std::vector<uint8_t>& obj, std::vector<Stencil>& stencils) {
    if (obj.size() < 64 || obj[4] != 2 || obj[5] != 1 || u16(obj, 16) != 1 || u16(obj, 18) != 62)
        fail("not an x86-64 little-endian ELF relocatable object file");
    const uint64_t sectionTable = u64(obj, 0x28);
    const uint16_t entry = u16(obj, 0x3A), count = u16(obj, 0x3C);
    struct Section {
        uint32_t name, type;
        uint64_t flags, offset, size, entsize;
        uint32_t link, info;
    };
    std::vector<Section> sections(count);
    for (uint16_t i = 0; i < count; i++) {
        const size_t h = sectionTable + static_cast<size_t>(i) * entry;
        sections[i] = Section{u32(obj, h), u32(obj, h + 4), u64(obj, h + 8), u64(obj, h + 24), u64(obj, h + 32), u64(obj, h + 56), u32(obj, h + 40), u32(obj, h + 44)};
    }
    constexpr uint32_t kSymtab = 2, kRela = 4;
    constexpr uint64_t kAlloc = 2, kExec = 4, kWrite = 1;
    size_t symtab = 0;
    for (size_t i = 0; i < sections.size(); i++)
        if (sections[i].type == kSymtab) symtab = i;
    if (symtab == 0) fail("no symbol table");
    const Section& syms = sections[symtab];
    const Section& names = sections[syms.link];
    auto nameAt = [&](uint32_t at) { return std::string(reinterpret_cast<const char*>(&obj[names.offset + at])); };
    struct Symbol {
        std::string name;
        uint8_t type;
        uint16_t section;
        uint64_t value, size;
    };
    std::vector<Symbol> symbols(syms.size / 24);
    for (size_t i = 0; i < symbols.size(); i++) {
        const size_t at = syms.offset + i * 24;
        symbols[i] = Symbol{nameAt(u32(obj, at)), static_cast<uint8_t>(obj[at + 4] & 0xF), u16(obj, at + 6), u64(obj, at + 8), u64(obj, at + 16)};
    }
    // Each stencil's section, and its relocations.
    std::map<size_t, size_t> stencilOf;  // section -> symbol
    for (size_t i = 0; i < symbols.size(); i++)
        if (symbols[i].type == 2 && symbols[i].section != 0 && symbols[i].name.rfind("st_", 0) == 0) stencilOf[symbols[i].section] = i;
    std::map<size_t, size_t> relaOf;
    for (size_t i = 0; i < sections.size(); i++)
        if (sections[i].type == kRela) relaOf[sections[i].info] = i;
    for (size_t s = 1; s < sections.size(); s++) {
        const Section& sec = sections[s];
        auto named = stencilOf.find(s);
        if (named == stencilOf.end()) {
            // Nothing else may hold code a stencil could need.
            if ((sec.flags & kExec) != 0 && sec.size > 0) fail("code that is no stencil, in section " + std::to_string(s));
            continue;
        }
        const Symbol& fn = symbols[named->second];
        if (fn.value != 0 || fn.size != sec.size) fail("stencil " + fn.name + " is not a section of its own");
        Stencil st;
        st.name = fn.name.substr(3);
        st.code.assign(obj.begin() + static_cast<std::ptrdiff_t>(sec.offset), obj.begin() + static_cast<std::ptrdiff_t>(sec.offset + sec.size));
        auto rela = relaOf.find(s);
        if (rela != relaOf.end()) {
            const Section& rs = sections[rela->second];
            for (uint64_t at = rs.offset; at < rs.offset + rs.size; at += 24) {
                const uint64_t offset = u64(obj, at), info = u64(obj, at + 8);
                const int64_t addend = static_cast<int64_t>(u64(obj, at + 16));
                const uint32_t type = static_cast<uint32_t>(info), symbol = static_cast<uint32_t>(info >> 32);
                if (type != 2 && type != 4) fail("stencil " + st.name + ": a relocation of kind " + std::to_string(type) + ", which is not filled in");
                const Symbol& target = symbols[symbol];
                auto hole = kHoles.find(target.name);
                if (hole != kHoles.end()) {
                    if (addend > -4 || addend < -9) fail("stencil " + st.name + ": a gap whose instruction does not end within 5 bytes of it");
                    st.holes.push_back(Hole{static_cast<uint32_t>(offset), hole->second, static_cast<uint32_t>(-addend - 4), {}});
                    continue;
                }
                // A constant of merged read-only data, its bytes carried with the stencil.
                if (target.section == 0 || target.section >= sections.size()) fail("stencil " + st.name + " refers to " + target.name + ", which is no gap");
                const Section& data = sections[target.section];
                const int64_t place = static_cast<int64_t>(target.value) + addend + 4;
                const uint64_t width = data.entsize == 4 || data.entsize == 8 || data.entsize == 16 ? data.entsize : 0;
                if ((data.flags & kAlloc) == 0 || (data.flags & (kExec | kWrite)) != 0 || relaOf.count(target.section) != 0 || width == 0 || place < 0 ||
                    static_cast<uint64_t>(place) + width > data.size)
                    fail("stencil " + st.name + " refers to " + target.name + ", which is no gap nor a constant it can carry");
                std::vector<uint8_t> bytes(obj.begin() + static_cast<std::ptrdiff_t>(data.offset + static_cast<uint64_t>(place)),
                                           obj.begin() + static_cast<std::ptrdiff_t>(data.offset + static_cast<uint64_t>(place) + width));
                st.holes.push_back(Hole{static_cast<uint32_t>(offset), "HoleData", 0, bytes});
            }
        }
        stencils.push_back(std::move(st));
    }
}

int main(int argc, char** argv) {
    if (argc != 3) fail("usage: stencils <object file> <header written>");
    std::ifstream in(argv[1], std::ios::binary);
    if (!in) fail(std::string("cannot read ") + argv[1]);
    const std::vector<uint8_t> obj((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    std::vector<Stencil> stencils;
    if (obj.size() >= 4 && obj[0] == 0x7F && obj[1] == 'E' && obj[2] == 'L' && obj[3] == 'F') readElf(obj, stencils);
    else if (obj.size() >= 20 && u16(obj, 0) == 0x8664) readCoff(obj, stencils);
    else fail("not an x64 COFF nor an x86-64 ELF object file");
    if (stencils.empty()) fail("no stencils found");
    // In name order, which jade looks them up by.
    std::sort(stencils.begin(), stencils.end(), [](const Stencil& a, const Stencil& b) { return a.name < b.name; });

    std::ostringstream out;
    out << "// Made by tools/stencils from src/native/stencils.cpp: do not edit.\n";
    out << "// " << stencils.size() << " stencils.\n\n";
    out << "namespace jade::native {\n\n";
    for (size_t k = 0; k < stencils.size(); k++) {
        const Stencil& st = stencils[k];
        out << "static const uint8_t kCode" << k << "[] = {";
        for (size_t i = 0; i < st.code.size(); i++) out << (i ? "," : "") << static_cast<int>(st.code[i]);
        out << "};\n";
        if (!st.holes.empty()) {
            out << "static const Hole kHoles" << k << "[] = {";
            for (size_t i = 0; i < st.holes.size(); i++) {
                out << (i ? ", " : "") << "{" << st.holes[i].offset << ", " << st.holes[i].kind << ", " << st.holes[i].after << ", "
                    << st.holes[i].data.size() << ", {";
                for (size_t b = 0; b < st.holes[i].data.size(); b++) out << (b ? "," : "") << static_cast<int>(st.holes[i].data[b]);
                out << "}}";
            }
            out << "};\n";
        }
    }
    out << "\nstatic const StencilCode kStencils[] = {\n";
    for (size_t k = 0; k < stencils.size(); k++) {
        const Stencil& st = stencils[k];
        out << "    {\"" << st.name << "\", kCode" << k << ", " << st.code.size() << ", " << (st.holes.empty() ? "nullptr" : "kHoles" + std::to_string(k))
            << ", " << st.holes.size() << "},\n";
    }
    out << "};\n\n}  // namespace jade::native\n";
    std::ofstream header(argv[2], std::ios::binary);
    header << out.str();
    if (!header) fail(std::string("cannot write ") + argv[2]);
    std::printf("stencils: %zu stencils written to %s\n", stencils.size(), argv[2]);
    return 0;
}
