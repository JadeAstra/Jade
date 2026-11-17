// tools/stencils (M9.5, docs/native-plan.html): reads the object file Clang made of
// src/native/stencils.cpp and writes the stencils as a C++ table: each one's machine code and its
// gaps, where in the code each is and what fills it. Run when ww.exe is built; no third-party
// code. It refuses anything it does not know (a reference to a name that is no gap, a kind of
// relocation it does not fill), so that a change in what Clang makes shows when building.
//
//   stencils <object file> <header written>
//
// The object file is COFF (Windows, x64; clang-cl /Gy; the small form or the big) or ELF (Linux, x86-64; clang
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
    {"ww_next", "HoleNext"},   {"ww_jump", "HoleJump"},   {"ww_exit", "HoleExit"},
    {"ww_kf", "HoleFloat"},    {"ww_ki", "HoleInt"},      {"ww_slot", "HoleSlot"},
    {"ww_ret", "HoleRet"},     {"ww_attention", "HoleAttention"}, {"ww_slot2", "HoleSlot2"},
    {"ww_slotat", "HoleSlotAt"}, {"ww_slot2at", "HoleSlot2At"},
};
// The gaps filled with a value itself, not the distance to it: a slot's offset in the frame,
// built into the instruction (an absolute 32-bit relocation; what it adds kept in `after`).
bool absolute(const std::string& kind) { return kind == "HoleSlotAt" || kind == "HoleSlot2At"; }

struct Hole {
    uint32_t offset;   // of the 32 bits filled in, in the stencil
    std::string kind;
    uint32_t after;    // bytes of the instruction after the 32 bits (REL32_1 to _5), which the offset is counted from;
                       // for an absolute gap, what is added to the value (the 32 bits' addend)
    std::vector<uint8_t> data;  // HoleData: the constant Clang put in read-only data (a float's bound, a sign's mask)
};
struct Stencil {
    std::string name;
    std::vector<uint8_t> code;
    std::vector<Hole> holes;
};

}  // namespace

// Whether an object file is COFF's big form (Clang writes it past 65,279 sections: stencils.cpp
// since 2026-11-17, eleven registers for whole numbers): its header 56 bytes, a symbol 20, a
// symbol's section number 32 bits.
bool bigCoff(const std::vector<uint8_t>& obj) {
    return obj.size() >= 56 && u16(obj, 0) == 0 && u16(obj, 2) == 0xFFFF && u16(obj, 4) >= 2 && u16(obj, 6) == 0x8664;
}

// An x64 COFF object file (Windows, clang-cl), of either form.
void readCoff(const std::vector<uint8_t>& obj, std::vector<Stencil>& stencils) {
    const bool big = bigCoff(obj);
    const uint32_t sectionCount = big ? u32(obj, 44) : u16(obj, 2);
    const uint32_t symbolTable = big ? u32(obj, 48) : u32(obj, 8), symbolCount = big ? u32(obj, 52) : u32(obj, 12);
    const size_t sections = big ? 56 : 20 + u16(obj, 16);
    const size_t symbolSize = big ? 20 : 18;
    const size_t strings = symbolTable + static_cast<size_t>(symbolCount) * symbolSize;
    // A symbol's section number: 0 is none; -1 and -2 (0xFFFF and 0xFFFE in the small form, whose
    // numbers past 32767 are sections: stencils.cpp has had more since 2026-11-14) are no section either.
    auto sectionOf = [&](size_t symbolAt) -> uint32_t {
        if (big) {
            const int32_t n = static_cast<int32_t>(u32(obj, symbolAt + 12));
            return n > 0 ? static_cast<uint32_t>(n) : 0;
        }
        const uint16_t n = u16(obj, symbolAt + 12);
        return n != 0 && n < 0xFF00 ? n : 0;
    };

    auto symbolName = [&](uint32_t index) {
        const size_t at = symbolTable + static_cast<size_t>(index) * symbolSize;
        if (u32(obj, at) == 0) {
            const size_t s = strings + u32(obj, at + 4);
            return std::string(reinterpret_cast<const char*>(&obj[s]));
        }
        std::string name(reinterpret_cast<const char*>(&obj[at]), 8);
        return name.substr(0, name.find('\0'));
    };

    // Each stencil's section: the defined external symbol starting "st_" in it.
    std::map<uint32_t, std::string> stencilOf;
    for (uint32_t i = 0; i < symbolCount; i++) {
        const size_t at = symbolTable + static_cast<size_t>(i) * symbolSize;
        const uint32_t section = sectionOf(at);
        const uint8_t storage = obj[at + symbolSize - 2], aux = obj[at + symbolSize - 1];
        const std::string name = symbolName(i);
        if (storage == 2 && section != 0 && name.rfind("st_", 0) == 0) stencilOf[section] = name.substr(3);
        i += aux;
    }

    for (uint32_t s = 1; s <= sectionCount; s++) {
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
            auto hole = kHoles.find(target);
            // IMAGE_REL_AMD64_ADDR32: the value itself, its addend in the 32 bits (a slot's offset).
            if (hole != kHoles.end() && absolute(hole->second)) {
                if (type != 2) fail("stencil " + st.name + ": " + target + " reached other than as a 32-bit value");
                const int32_t addend = static_cast<int32_t>(u32(st.code, offset));
                if (addend < 0 || addend > 15) fail("stencil " + st.name + ": " + target + " with an addend past its slot");
                std::memset(&st.code[offset], 0, 4);
                st.holes.push_back(Hole{offset, hole->second, static_cast<uint32_t>(addend), {}});
                continue;
            }
            // IMAGE_REL_AMD64_REL32 and REL32_1 to REL32_5: 32 bits, counted from the end of the
            // instruction, which is that many bytes past them.
            if (type < 4 || type > 9) fail("stencil " + st.name + ": a relocation of kind " + std::to_string(type) + ", which is not filled in");
            if (hole != kHoles.end()) {
                // The 32 bits' own addend counted (2026-11-15: eqfk_8 to eqfk_15, `cmpeqsd
                // -0x1(%rip)` with the predicate after, read the float one byte off): the bytes
                // after them, as for a constant below.
                const int32_t inPlace = static_cast<int32_t>(u32(st.code, offset));
                const int32_t after = static_cast<int32_t>(type - 4) - inPlace;
                if (inPlace > 0 || after > 5) fail("stencil " + st.name + " reaches " + target + " with an addend of " + std::to_string(inPlace));
                std::memset(&st.code[offset], 0, 4);
                st.holes.push_back(Hole{offset, hole->second, static_cast<uint32_t>(after), {}});
                continue;
            }
            // A constant of read-only data (Clang's __real@ and __xmm@): its bytes go with the
            // stencil, at most 16, and are placed beside the region's code.
            const size_t sym = symbolTable + static_cast<size_t>(symbol) * symbolSize;
            const uint32_t dataSection = sectionOf(sym);
            const uint32_t value = u32(obj, sym + 8);
            if (dataSection == 0) fail("stencil " + st.name + " refers to " + target + ", which is no gap");
            const size_t dh = sections + static_cast<size_t>(dataSection - 1) * 40;
            const uint32_t dataCharacteristics = u32(obj, dh + 36);
            const uint32_t dataSize = u32(obj, dh + 16), dataRaw = u32(obj, dh + 20);
            // Initialized data, not writable, with no relocations of its own.
            if ((dataCharacteristics & 0x40) == 0 || (dataCharacteristics & 0x80000000) != 0 || u16(obj, dh + 32) != 0 || value >= dataSize ||
                dataSize - value > 16)
                fail("stencil " + st.name + " refers to " + target + ", which is no gap nor a constant it can carry");
            std::vector<uint8_t> bytes(obj.begin() + dataRaw + value, obj.begin() + dataRaw + dataSize);
            // The 32 bits hold an addend: where an instruction has a byte after them (an
            // immediate: cmppd's predicate, pshufd's order), Clang may write a plain REL32 with
            // -1 there rather than REL32_1 (2026-11-15: a cmpltpd read one byte past its bound,
            // and faulted, as an SSE operand must be on 16). The bytes after them, counted both
            // ways; the constant itself is what is reached, from its first byte.
            const int32_t inPlace = static_cast<int32_t>(u32(st.code, offset));
            const int32_t after = static_cast<int32_t>(type - 4) - inPlace;
            if (inPlace > 0 || after > 5)
                fail("stencil " + st.name + " reaches " + target + " other than at its first byte (" + std::to_string(inPlace) + ")");
            std::memset(&st.code[offset], 0, 4);
            st.holes.push_back(Hole{offset, "HoleData", static_cast<uint32_t>(after), bytes});
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
    const uint16_t entry = u16(obj, 0x3A);
    // Past 65,279 sections (the stencils since 2026-11-14) the count is 0 here and section 0's
    // size holds it.
    size_t count = u16(obj, 0x3C);
    if (count == 0) count = static_cast<size_t>(u64(obj, sectionTable + 32));
    struct Section {
        uint32_t name, type;
        uint64_t flags, offset, size, entsize;
        uint32_t link, info;
    };
    std::vector<Section> sections(count);
    for (size_t i = 0; i < count; i++) {
        const size_t h = sectionTable + i * entry;
        sections[i] = Section{u32(obj, h), u32(obj, h + 4), u64(obj, h + 8), u64(obj, h + 24), u64(obj, h + 32), u64(obj, h + 56), u32(obj, h + 40), u32(obj, h + 44)};
    }
    constexpr uint32_t kSymtab = 2, kRela = 4, kSymtabShndx = 18;
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
        uint32_t section;
        uint64_t value, size;
    };
    // A symbol's section past 65,279: SHN_XINDEX in the symbol, its number in SYMTAB_SHNDX.
    size_t shndx = 0;
    for (size_t i = 0; i < sections.size(); i++)
        if (sections[i].type == kSymtabShndx && sections[i].link == symtab) shndx = i;
    std::vector<Symbol> symbols(syms.size / 24);
    for (size_t i = 0; i < symbols.size(); i++) {
        const size_t at = syms.offset + i * 24;
        uint32_t section = u16(obj, at + 6);
        if (section == 0xFFFF) {
            if (shndx == 0) fail("a symbol's section in a table there is none of");
            section = u32(obj, sections[shndx].offset + i * 4);
        } else if (section >= 0xFF00) {
            section = 0;  // absolute or common: no section
        }
        symbols[i] = Symbol{nameAt(u32(obj, at)), static_cast<uint8_t>(obj[at + 4] & 0xF), section, u64(obj, at + 8), u64(obj, at + 16)};
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
                const Symbol& target = symbols[symbol];
                auto hole = kHoles.find(target.name);
                // R_X86_64_32 and _32S: the value itself, plus the addend (a slot's offset).
                if (hole != kHoles.end() && absolute(hole->second)) {
                    if (type != 10 && type != 11) fail("stencil " + st.name + ": " + target.name + " reached other than as a 32-bit value");
                    if (addend < 0 || addend > 15) fail("stencil " + st.name + ": " + target.name + " with an addend past its slot");
                    st.holes.push_back(Hole{static_cast<uint32_t>(offset), hole->second, static_cast<uint32_t>(addend), {}});
                    continue;
                }
                if (type != 2 && type != 4) fail("stencil " + st.name + ": a relocation of kind " + std::to_string(type) + ", which is not filled in");
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
                // The addend counts the byte after the 32 bits too, in an instruction with an
                // immediate (cmpps and cmppd 0F C2, pshufd 0F 70, shufps and shufpd 0F C6, the
                // opcode just before the ModRM byte): the constant is then the one a byte on,
                // read from its own first byte with that byte counted, so that it stays on its
                // width (an SSE operand must be on 16; 2026-11-15). Else the bytes from `place`.
                const uint8_t opcode = offset >= 2 ? st.code[static_cast<size_t>(offset) - 2] : 0;
                const bool immediate = opcode == 0xC2 || opcode == 0x70 || opcode == 0xC6;
                const uint64_t from = immediate && static_cast<uint64_t>(place) + 1 + width <= data.size && (static_cast<uint64_t>(place) + 1) % width == 0
                                          ? static_cast<uint64_t>(place) + 1
                                          : static_cast<uint64_t>(place);
                std::vector<uint8_t> bytes(obj.begin() + static_cast<std::ptrdiff_t>(data.offset + from),
                                           obj.begin() + static_cast<std::ptrdiff_t>(data.offset + from + width));
                st.holes.push_back(Hole{static_cast<uint32_t>(offset), "HoleData", static_cast<uint32_t>(from - static_cast<uint64_t>(place)), bytes});
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
    else if ((obj.size() >= 20 && u16(obj, 0) == 0x8664) || bigCoff(obj)) readCoff(obj, stencils);
    else fail("not an x64 COFF nor an x86-64 ELF object file");
    if (stencils.empty()) fail("no stencils found");
    // In name order, which ww looks them up by.
    std::sort(stencils.begin(), stencils.end(), [](const Stencil& a, const Stencil& b) { return a.name < b.name; });

    std::ostringstream out;
    out << "// Made by tools/stencils from src/native/stencils.cpp: do not edit.\n";
    out << "// " << stencils.size() << " stencils.\n\n";
    out << "namespace ww::native {\n\n";
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
    out << "};\n\n}  // namespace ww::native\n";
    std::ofstream header(argv[2], std::ios::binary);
    header << out.str();
    if (!header) fail(std::string("cannot write ") + argv[2]);
    std::printf("stencils: %zu stencils written to %s\n", stencils.size(), argv[2]);
    return 0;
}
