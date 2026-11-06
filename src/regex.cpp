// Regular expressions (N5, M8): see regex.h. Java's syntax and meaning, as Kotlin's Regex has
// them on the JVM (where Kotlin always adds UNICODE_CASE to IGNORE_CASE): the pattern is parsed
// into a tree, the tree compiled into a program of a few kinds of instruction, and the program
// run by backtracking, with the alternatives kept on a stack of its own.
//
// What a character is (a letter, a digit, upper or lower case) comes from the system's tables,
// as it does for char.isLetter() and the rest, until the text engine brings Unicode data of its
// own; so do the Unicode categories (\p{Lu}), the rarer ones (\p{Lm}, \p{Sc}) approximated by
// what those tables tell apart.

#ifdef _WIN32
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

#include "regex.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <mutex>

#include "builtins.h"
#include "value.h"

#ifndef _WIN32
#include <wctype.h>
#endif

namespace astra::regex {

// ---- What a character is ----

namespace {

enum Kind : uint16_t {
    kLetter = 1, kDigit = 2, kSpace = 4, kUpper = 8, kLower = 16, kPunct = 32, kControl = 64, kSymbol = 128, kMark = 256,
    kDefined = 512,
};

#ifdef _WIN32
uint16_t systemKinds(uint32_t c) {
    wchar_t units[2];
    int count = 1;
    if (c >= 0x10000) {
        units[0] = static_cast<wchar_t>(0xD800 + ((c - 0x10000) >> 10));
        units[1] = static_cast<wchar_t>(0xDC00 + ((c - 0x10000) & 0x3FF));
        count = 2;
    } else {
        units[0] = static_cast<wchar_t>(c);
    }
    WORD one[2] = {0, 0}, three[2] = {0, 0};
    GetStringTypeW(CT_CTYPE1, units, count, one);
    GetStringTypeW(CT_CTYPE3, units, count, three);
    uint16_t k = 0;
    if (one[0] & C1_ALPHA) k |= kLetter;
    if (one[0] & C1_DIGIT) k |= kDigit;
    if (one[0] & C1_SPACE) k |= kSpace;
    if (one[0] & C1_UPPER) k |= kUpper;
    if (one[0] & C1_LOWER) k |= kLower;
    if (one[0] & C1_PUNCT) k |= kPunct;
    if (one[0] & C1_CNTRL) k |= kControl;
    if (three[0] & C3_SYMBOL) k |= kSymbol;
    if (three[0] & C3_NONSPACING) k |= kMark;
    if (one[0] & C1_DEFINED) k |= kDefined;
    if (k & kLetter) k &= static_cast<uint16_t>(~kPunct);  // some letters come with a punctuation bit
    return k;
}
uint32_t systemCase(uint32_t c, bool upper) {
    if (c >= 0x10000 || (c >= 0xD800 && c < 0xE000)) return c;
    wchar_t unit = static_cast<wchar_t>(c);
    if (upper) CharUpperBuffW(&unit, 1);
    else CharLowerBuffW(&unit, 1);
    return unit;
}
#else
uint16_t systemKinds(uint32_t c) {
    const int k = unicodeKinds(c);
    uint16_t out = kDefined;
    if (k & kUnicodeLetter) out |= kLetter;
    if (k & kUnicodeDigit) out |= kDigit;
    if (k & kUnicodeSpace) out |= kSpace;
    if (k & kUnicodeUpper) out |= kUpper;
    if (k & kUnicodeLower) out |= kLower;
    if (c < 0x20 || (c >= 0x7F && c < 0xA0)) out |= kControl;
    else if (!(out & (kLetter | kDigit | kSpace))) out |= kPunct;
    return out;
}
uint32_t systemCase(uint32_t c, bool upper) { return unicodeCase(c, upper); }
#endif

// The kinds and the cases of the first 65,536 characters, worked out once.
struct Tables {
    std::vector<uint16_t> kinds;
    std::vector<uint32_t> upper, lower;
    Tables() : kinds(0x10000), upper(0x10000), lower(0x10000) {
        for (uint32_t c = 0; c < 0x10000; c++) {
            if (c < 0x80) {
                uint16_t k = kDefined;
                if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')) k |= kLetter;
                if (c >= 'A' && c <= 'Z') k |= kUpper;
                if (c >= 'a' && c <= 'z') k |= kLower;
                if (c >= '0' && c <= '9') k |= kDigit;
                if (c == ' ' || (c >= 9 && c <= 13) || (c >= 0x1C && c <= 0x1F)) k |= kSpace;
                if (c < 0x20 || c == 0x7F) k |= kControl;
                if (std::strchr("!\"#%&'()*,-./:;?@[\\]_{}", static_cast<int>(c)) != nullptr && c != 0) k |= kPunct;
                if (std::strchr("$+<=>^`|~", static_cast<int>(c)) != nullptr && c != 0) k |= kSymbol;
                kinds[c] = k;
                upper[c] = (c >= 'a' && c <= 'z') ? c - 32 : c;
                lower[c] = (c >= 'A' && c <= 'Z') ? c + 32 : c;
            } else {
                kinds[c] = (c >= 0xD800 && c < 0xE000) ? 0 : systemKinds(c);
                upper[c] = systemCase(c, true);
                lower[c] = systemCase(c, false);
            }
        }
        // The titlecase digraphs (Dž, Lj, Nj, Dz) each have an upper and a lower form.
        for (uint32_t t : {0x01C5u, 0x01C8u, 0x01CBu, 0x01F2u}) {
            upper[t] = t - 1;
            lower[t] = t + 1;
            upper[t + 1] = t - 1;
            lower[t - 1] = t + 1;
        }
    }
};
const Tables& tables() {
    static const Tables made;
    return made;
}

uint16_t kindsOf(uint32_t c) { return c < 0x10000 ? tables().kinds[c] : systemKinds(c); }
uint32_t upperOf(uint32_t c) { return c < 0x10000 ? tables().upper[c] : systemCase(c, true); }
uint32_t lowerOf(uint32_t c) { return c < 0x10000 ? tables().lower[c] : systemCase(c, false); }
// One form of a character for comparing without case, as Java's UNICODE_CASE does.
uint32_t fold(uint32_t c) { return lowerOf(upperOf(c)); }

bool isLineTerminator(uint32_t c, bool unixLines) {
    if (unixLines) return c == '\n';
    return c == '\n' || c == '\r' || c == 0x85 || c == 0x2028 || c == 0x2029;
}
bool isAsciiWord(uint32_t c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_'; }

// The classes a pattern names: \d, \p{Lu} and the like, each a test of one character.
enum class Pred : uint8_t {
    AsciiDigit, AsciiWord, AsciiSpace, HSpace, VSpace,
    PosixLower, PosixUpper, Ascii, PosixAlpha, PosixAlnum, PosixPunct, PosixGraph, PosixPrint, Blank, PosixCntrl, XDigit,
    Letter, Upper, Lower, TitleOrModifier, OtherLetter, Mark, Digit, Punct, Symbol, Separator, SpaceSeparator, LineSeparator,
    ParagraphSeparator, Control, Surrogate, PrivateUse, Unassigned, Format, Any, Whitespace, LetterOrDigit, Alphabetic,
    UnicodeWord,
};

bool test(Pred p, uint32_t c) {
    const auto posixPunct = [](uint32_t ch) { return ch < 0x80 && std::strchr("!\"#$%&'()*+,-./:;<=>?@[\\]^_`{|}~", static_cast<int>(ch)) != nullptr && ch != 0; };
    switch (p) {
        case Pred::AsciiDigit: return c >= '0' && c <= '9';
        case Pred::AsciiWord: return isAsciiWord(c);
        case Pred::AsciiSpace: return c == ' ' || c == '\t' || c == '\n' || c == 0x0B || c == '\f' || c == '\r';
        case Pred::HSpace:
            return c == ' ' || c == '\t' || c == 0xA0 || c == 0x1680 || c == 0x180E || (c >= 0x2000 && c <= 0x200A) || c == 0x202F || c == 0x205F ||
                   c == 0x3000;
        case Pred::VSpace: return c == '\n' || c == 0x0B || c == '\f' || c == '\r' || c == 0x85 || c == 0x2028 || c == 0x2029;
        case Pred::PosixLower: return c >= 'a' && c <= 'z';
        case Pred::PosixUpper: return c >= 'A' && c <= 'Z';
        case Pred::Ascii: return c < 0x80;
        case Pred::PosixAlpha: return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
        case Pred::PosixAlnum: return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9');
        case Pred::PosixPunct: return posixPunct(c);
        case Pred::PosixGraph: return c > 0x20 && c < 0x7F;
        case Pred::PosixPrint: return c >= 0x20 && c < 0x7F;
        case Pred::Blank: return c == ' ' || c == '\t';
        case Pred::PosixCntrl: return c < 0x20 || c == 0x7F;
        case Pred::XDigit: return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
        case Pred::Letter: return (kindsOf(c) & kLetter) != 0;
        case Pred::Upper: return (kindsOf(c) & kUpper) != 0;
        case Pred::Lower: return (kindsOf(c) & kLower) != 0;
        case Pred::TitleOrModifier: return c == 0x01C5 || c == 0x01C8 || c == 0x01CB || c == 0x01F2 || (c >= 0x02B0 && c <= 0x02FF && (kindsOf(c) & kLetter));
        case Pred::OtherLetter: {
            const uint16_t k = kindsOf(c);
            return (k & kLetter) && !(k & (kUpper | kLower));
        }
        case Pred::Mark: return (kindsOf(c) & kMark) != 0;
        case Pred::Digit: return (kindsOf(c) & kDigit) != 0;
        case Pred::Punct: {
            const uint16_t k = kindsOf(c);
            return c < 0x80 ? (k & kPunct) != 0 : (k & kPunct) && !(k & kSymbol);
        }
        case Pred::Symbol: return (kindsOf(c) & kSymbol) != 0;
        case Pred::Separator: return c == ' ' || c == 0xA0 || c == 0x1680 || (c >= 0x2000 && c <= 0x200A) || c == 0x2028 || c == 0x2029 || c == 0x202F || c == 0x205F || c == 0x3000;
        case Pred::SpaceSeparator: return c == ' ' || c == 0xA0 || c == 0x1680 || (c >= 0x2000 && c <= 0x200A) || c == 0x202F || c == 0x205F || c == 0x3000;
        case Pred::LineSeparator: return c == 0x2028;
        case Pred::ParagraphSeparator: return c == 0x2029;
        case Pred::Control: return c < 0x20 || (c >= 0x7F && c < 0xA0);
        case Pred::Surrogate: return c >= 0xD800 && c < 0xE000;
        case Pred::PrivateUse: return (c >= 0xE000 && c <= 0xF8FF) || (c >= 0xF0000 && c <= 0xFFFFD) || (c >= 0x100000 && c <= 0x10FFFD);
        case Pred::Unassigned: return !(kindsOf(c) & kDefined) && !(c >= 0xD800 && c < 0xE000);
        case Pred::Format: return c == 0xAD || (c >= 0x600 && c <= 0x605) || c == 0x200B || (c >= 0x200C && c <= 0x200F) || (c >= 0x202A && c <= 0x202E) ||
                                  (c >= 0x2060 && c <= 0x2064) || c == 0xFEFF;
        case Pred::Any: return true;
        case Pred::Whitespace: {
            // Java's Character.isWhitespace: space separators but the no-break ones, and \t to \r, \x1C to \x1F.
            if (c == 0xA0 || c == 0x2007 || c == 0x202F) return false;
            return (c >= 9 && c <= 13) || (c >= 0x1C && c <= 0x1F) || test(Pred::Separator, c);
        }
        case Pred::LetterOrDigit: return (kindsOf(c) & (kLetter | kDigit)) != 0;
        case Pred::Alphabetic: return (kindsOf(c) & kLetter) != 0;
        case Pred::UnicodeWord: return (kindsOf(c) & (kLetter | kDigit | kMark)) != 0 || c == '_' || c == 0x200C || c == 0x200D;
    }
    return false;
}

}  // namespace

SyntaxError::SyntaxError(std::string d, int at, const std::string& pattern)
    : std::runtime_error([&] {
          std::string m = d;
          if (at >= 0) {
              m += " near index " + std::to_string(at) + "\n" + pattern;
              if (at <= static_cast<int>(pattern.size())) m += "\n" + std::string(static_cast<size_t>(at), ' ') + "^";
          }
          return m;
      }()),
      description(std::move(d)),
      index(at) {}

// ---- The program ----

namespace {

// A class of characters: what is in it, and how it is made of others.
struct ClassNode {
    bool negated = false;
    bool intersection = false;  // a and b, both of which a character must be in
    int a = -1, b = -1;
    std::vector<std::pair<uint32_t, uint32_t>> ranges;
    std::vector<std::pair<Pred, bool>> preds;  // each, or each's opposite
    std::vector<int> kids;                     // classes inside it ([a-z[0-9]])
};

enum class Op : uint8_t {
    Char, CharFold, CharFoldAscii, AnyAll, AnyLine, Class, Split, Jmp, Save, Match,
    Begin, LineStart, End, EndFinal, LineEnd, WordBoundary, NotWordBoundary, PreviousEnd,
    BackRef, Look, Atomic, LoopSet, LoopCheck, AtTarget,
};

struct Inst {
    explicit Inst(Op o) : op(o) {}
    Op op;
    bool flag = false;  // Class: without case; BackRef: without case; Look: negative; LineStart/LineEnd/EndFinal: Unix lines
    bool behind = false;
    uint32_t c = 0;
    int a = 0, b = 0;   // Split: preferred, other; Jmp: target; Save/BackRef/Loop*: slot; Class: class; Look/Atomic: sub-program start
    int minLen = 0, maxLen = 0;
};

}  // namespace

struct Program {
    std::vector<Inst> code;
    std::vector<ClassNode> classes;
    std::vector<std::array<uint64_t, 2>> asciiBits;  // for each class: which ASCII characters are in it
    std::vector<uint8_t> classFold;  // 0 case counts, 1 ASCII letters either case, 2 every letter
    int start = 0;
    int groups = 0;  // capturing groups
    int loops = 0;   // registers for loops that may match nothing
    std::map<std::string, int> names;
    // Branch points of the main program, numbered, for remembering where they failed: where a
    // match from a branch point at a position depends on nothing else (no back references, no
    // loops that may match nothing), one that failed there fails there again, and is not tried
    // twice. That keeps patterns such as (a+)+$ from taking exponential time, as Java's do.
    std::vector<int> splitNumber;  // for each instruction of the main program, -1 if no branch point
    int splits = 0;
    bool memo = false;
    // Where a match can start: only at the start of the input, or only at a given character.
    bool anchored = false;
    bool hasFirst = false;
    uint32_t first = 0;
};

namespace {

bool inClassRaw(const Program& p, int index, uint32_t c) {
    const ClassNode& n = p.classes[static_cast<size_t>(index)];
    bool in;
    if (n.intersection) {
        in = inClassRaw(p, n.a, c) && inClassRaw(p, n.b, c);
    } else {
        in = false;
        for (const auto& [lo, hi] : n.ranges)
            if (c >= lo && c <= hi) {
                in = true;
                break;
            }
        if (!in)
            for (const auto& [pred, opposite] : n.preds)
                if (test(pred, c) != opposite) {
                    in = true;
                    break;
                }
        if (!in)
            for (int kid : n.kids)
                if (inClassRaw(p, kid, c)) {
                    in = true;
                    break;
                }
    }
    return n.negated ? !in : in;
}

bool inClass(const Program& p, int index, uint32_t c) {
    if (c < 128) {
        const auto& bits = p.asciiBits[static_cast<size_t>(index)];
        return (bits[c >> 6] >> (c & 63)) & 1;
    }
    if (inClassRaw(p, index, c)) return true;
    if (p.classFold[static_cast<size_t>(index)] != 2) return false;  // past ASCII only Unicode case folds
    const uint32_t u = upperOf(c), l = lowerOf(c);
    return (u != c && inClassRaw(p, index, u)) || (l != c && inClassRaw(p, index, l));
}

// ---- Parsing ----

struct Node {
    enum Kind { Empty, Char, Class, Dot, Assert, Group, Concat, Alt, Repeat, BackRef, Look, Atomic };
    explicit Node(Kind k) : kind(k) {}
    Kind kind;
    uint32_t c = 0;
    bool fold = false;       // Char: compared by its Unicode fold
    bool asciiFold = false;  // Char: an ASCII letter in either case
    int foldMode = 0;        // BackRef: 0 case counts, 1 ASCII, 2 Unicode
    bool dotAll = false, unixLines = false;
    Op assertOp = Op::Begin;
    int group = -1;            // Group: its number, or -1; BackRef: the group
    std::string groupName;     // BackRef by name, settled once every group is known
    int nameAt = -1;           // where that name ends in the pattern
    int cls = -1;
    std::vector<int> kids;
    int min = 0, max = 0;      // Repeat; max -1 for no end
    enum Greed { Greedy, Lazy, Possessive } greed = Greedy;
    bool negative = false, behind = false;  // Look
};

class Parser {
public:
    Parser(const std::u32string& pattern, uint32_t flags, Program& out) : s_(pattern), flags_(flags), p_(out) {
        for (char32_t ch : pattern) appendUtf8(utf8_, static_cast<uint32_t>(ch));
    }

    int parse() {
        if (flags_ & kLiteral) {
            const int seq = add(Node{Node::Concat});
            for (char32_t ch : s_) {
                const int k = charNode(static_cast<uint32_t>(ch));
                nodes_[static_cast<size_t>(seq)].kids.push_back(k);
            }
            return seq;
        }
        const int root = alternation();
        if (at_ < s_.size()) {
            if (s_[at_] == ')') fail("Unmatched closing ')'", static_cast<int>(at_) - 1 < 0 ? 0 : static_cast<int>(at_) - 1);
            fail("Unexpected character", static_cast<int>(at_));
        }
        for (Node& n : nodes_)
            if (n.kind == Node::BackRef && !n.groupName.empty()) {
                auto found = p_.names.find(n.groupName);
                if (found == p_.names.end()) fail("named capturing group <" + n.groupName + "> does not exist", n.nameAt);
                n.group = found->second;
            }
        return root;
    }

    std::vector<Node> nodes_;

private:
    const std::u32string& s_;
    std::string utf8_;
    uint32_t flags_;
    Program& p_;
    size_t at_ = 0;

    [[noreturn]] void fail(const std::string& what, int at) { throw SyntaxError(what, at, utf8_); }

    int add(Node n) {
        nodes_.push_back(std::move(n));
        return static_cast<int>(nodes_.size()) - 1;
    }
    bool more() const { return at_ < s_.size(); }
    char32_t peek() const { return at_ < s_.size() ? s_[at_] : 0; }

    // Whitespace and # comments, in COMMENTS mode.
    void skipComments() {
        if (!(flags_ & kComments)) return;
        while (at_ < s_.size()) {
            const char32_t ch = s_[at_];
            if (ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r' || ch == '\f' || ch == 0x0B) {
                at_++;
            } else if (ch == '#') {
                while (at_ < s_.size() && !isLineTerminator(s_[at_], (flags_ & kUnixLines) != 0)) at_++;
            } else {
                break;
            }
        }
    }

    int alternation() {
        std::vector<int> branches{sequence()};
        while (more() && peek() == '|') {
            at_++;
            branches.push_back(sequence());
        }
        if (branches.size() == 1) return branches[0];
        Node alt{Node::Alt};
        alt.kids = branches;
        return add(alt);
    }

    int sequence() {
        Node seq{Node::Concat};
        for (;;) {
            skipComments();
            if (!more() || peek() == '|' || peek() == ')') break;
            int atom = this->atom();
            if (atom == -2) continue;  // a flag group such as (?i), which changes what follows
            atom = quantified(atom);
            seq.kids.push_back(atom);
        }
        if (seq.kids.size() == 1) return seq.kids[0];
        return add(seq);
    }

    bool parseNumber(int& out) {
        const size_t begin = at_;
        int64_t n = 0;
        while (more() && peek() >= '0' && peek() <= '9') {
            n = n * 10 + (peek() - '0');
            if (n > 1000000000) fail("Illegal repetition range", static_cast<int>(begin));
            at_++;
        }
        out = static_cast<int>(n);
        return at_ > begin;
    }

    int quantified(int atom) {
        for (bool once = false;; once = true) {
            skipComments();
            if (!more()) return atom;
            const char32_t ch = peek();
            int min, max;
            const size_t qAt = at_;
            if (ch == '*') {
                min = 0;
                max = -1;
                at_++;
            } else if (ch == '+') {
                min = 1;
                max = -1;
                at_++;
            } else if (ch == '?') {
                min = 0;
                max = 1;
                at_++;
            } else if (ch == '{') {
                at_++;
                if (!parseNumber(min)) fail("Illegal repetition", static_cast<int>(qAt) + 1);
                max = min;
                if (more() && peek() == ',') {
                    at_++;
                    if (!parseNumber(max)) max = -1;
                }
                if (!more() || peek() != '}') fail("Unclosed counted closure", static_cast<int>(at_));
                at_++;
                if (max >= 0 && max < min) fail("Illegal repetition range", static_cast<int>(at_) - 1);
            } else {
                return atom;
            }
            // One quantifier to an atom, as in Java: a** is an error, not a repeat of a repeat.
            if (once) fail(std::string("Dangling meta character '") + static_cast<char>(ch) + "'", static_cast<int>(qAt));
            Node rep{Node::Repeat};
            rep.kids = {atom};
            rep.min = min;
            rep.max = max;
            if (more() && peek() == '?') {
                rep.greed = Node::Lazy;
                at_++;
            } else if (more() && peek() == '+') {
                rep.greed = Node::Possessive;
                at_++;
            }
            atom = add(rep);
        }
    }

    int charNode(uint32_t ch) {
        Node n{Node::Char};
        // Without case only where the character has another case at all: any letter with
        // Unicode case, an ASCII one without.
        if ((flags_ & kCaseInsensitive) && (flags_ & kUnicodeCase)) {
            n.fold = upperOf(ch) != ch || lowerOf(ch) != ch;
            n.c = n.fold ? regex::fold(ch) : ch;
        } else if ((flags_ & kCaseInsensitive) && ((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z'))) {
            n.asciiFold = true;
            n.c = ch | 0x20;
        } else {
            n.c = ch;
        }
        return add(n);
    }

    int classOf(std::vector<std::pair<Pred, bool>> preds, std::vector<std::pair<uint32_t, uint32_t>> ranges = {}) {
        ClassNode c;
        c.preds = std::move(preds);
        c.ranges = std::move(ranges);
        p_.classes.push_back(std::move(c));
        p_.classFold.push_back(static_cast<uint8_t>((flags_ & kCaseInsensitive) ? ((flags_ & kUnicodeCase) ? 2 : 1) : 0));
        Node n{Node::Class};
        n.cls = static_cast<int>(p_.classes.size()) - 1;
        return add(n);
    }

    int assertNode(Op op) {
        Node n{Node::Assert};
        n.assertOp = op;
        n.unixLines = (flags_ & kUnixLines) != 0;
        return add(n);
    }

    int atom() {
        const size_t begin = at_;
        const char32_t ch = s_[at_++];
        switch (ch) {
            case '(':
                return group(begin);
            case '[': {
                const int cls = charClass(begin);
                Node n{Node::Class};
                n.cls = cls;
                return add(n);
            }
            case '.': {
                Node n{Node::Dot};
                n.dotAll = (flags_ & kDotAll) != 0;
                n.unixLines = (flags_ & kUnixLines) != 0;
                return add(n);
            }
            case '^':
                return assertNode((flags_ & kMultiline) ? Op::LineStart : Op::Begin);
            case '$':
                return assertNode((flags_ & kMultiline) ? Op::LineEnd : Op::EndFinal);
            case '\\':
                return escape(begin);
            case '*':
            case '+':
            case '?':
                fail(std::string("Dangling meta character '") + static_cast<char>(ch) + "'", static_cast<int>(begin));
            case '{':
                fail("Illegal repetition", static_cast<int>(begin));
            default:
                return charNode(static_cast<uint32_t>(ch));
        }
    }

    // After "(".
    int group(size_t begin) {
        int index = -1;
        std::string name;
        Node::Kind kind = Node::Group;
        bool negative = false, behind = false;
        const uint32_t saved = flags_;
        if (more() && peek() == '?') {
            at_++;
            if (!more()) fail("Unknown inline modifier", static_cast<int>(at_));
            const char32_t ch = s_[at_++];
            if (ch == ':') {
                // (?:...)
            } else if (ch == '=' || ch == '!') {
                kind = Node::Look;
                negative = ch == '!';
            } else if (ch == '>') {
                kind = Node::Atomic;
            } else if (ch == '<') {
                if (more() && (peek() == '=' || peek() == '!')) {
                    kind = Node::Look;
                    behind = true;
                    negative = s_[at_++] == '!';
                } else {
                    name = groupName();
                    if (p_.names.count(name) != 0) fail("Named capturing group <" + name + "> is already defined", static_cast<int>(at_) - 1);
                    index = ++p_.groups;
                    p_.names[name] = index;
                }
            } else {
                // Flags: (?i) for the rest of the group around, or (?i:...) for this group alone.
                at_--;
                bool off = false;
                for (;;) {
                    if (!more()) fail("Unknown inline modifier", static_cast<int>(at_));
                    const char32_t f = s_[at_++];
                    uint32_t bit = 0;
                    switch (f) {
                        case 'i': bit = kCaseInsensitive; break;
                        case 'm': bit = kMultiline; break;
                        case 's': bit = kDotAll; break;
                        case 'x': bit = kComments; break;
                        case 'd': bit = kUnixLines; break;
                        case 'u': bit = kUnicodeCase; break;
                        case 'U': case 'c': bit = 0; break;  // no effect here
                        case '-':
                            if (off) fail("Unknown inline modifier", static_cast<int>(at_) - 1);
                            off = true;
                            continue;
                        case ')':
                            return -2;  // flags for what follows, in this group
                        case ':': {
                            const int body = alternation();
                            if (!more() || peek() != ')') fail("Unclosed group", static_cast<int>(s_.size()));
                            at_++;
                            flags_ = saved;
                            Node g{Node::Group};
                            g.kids = {body};
                            return add(g);
                        }
                        default:
                            fail("Unknown inline modifier", static_cast<int>(at_) - 1);
                    }
                    flags_ = off ? (flags_ & ~bit) : (flags_ | bit);
                }
            }
        } else {
            index = ++p_.groups;
        }
        const int body = alternation();
        if (!more() || peek() != ')') fail("Unclosed group", static_cast<int>(s_.size()));
        at_++;
        flags_ = saved;
        Node g{kind};
        g.group = index;
        g.kids = {body};
        g.negative = negative;
        g.behind = behind;
        (void)begin;
        return add(g);
    }

    std::string groupName() {
        std::string name;
        const size_t begin = at_;
        while (more() && peek() != '>') {
            const char32_t ch = s_[at_++];
            const bool letter = (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z');
            const bool digit = ch >= '0' && ch <= '9';
            if (!letter && !(digit && at_ - 1 > begin)) fail("named capturing group is missing trailing '>'", static_cast<int>(at_) - 1);
            name.push_back(static_cast<char>(ch));
        }
        if (!more()) fail("named capturing group is missing trailing '>'", static_cast<int>(at_));
        if (name.empty()) fail("capturing group name does not start with a Latin letter", static_cast<int>(at_));
        at_++;
        return name;
    }

    uint32_t hexDigits(size_t count, size_t begin) {
        uint32_t n = 0;
        for (size_t k = 0; k < count; k++) {
            if (!more()) fail("Illegal hexadecimal escape sequence", static_cast<int>(begin));
            const char32_t ch = s_[at_++];
            int d;
            if (ch >= '0' && ch <= '9') d = static_cast<int>(ch - '0');
            else if (ch >= 'a' && ch <= 'f') d = static_cast<int>(ch - 'a' + 10);
            else if (ch >= 'A' && ch <= 'F') d = static_cast<int>(ch - 'A' + 10);
            else fail(count == 4 ? "Illegal Unicode escape sequence" : "Illegal hexadecimal escape sequence", static_cast<int>(at_) - 1);
            n = n * 16 + static_cast<uint32_t>(d);
        }
        return n;
    }

    // A character an escape stands for, where it stands for one: true and `out` set.
    bool charEscape(char32_t ch, size_t begin, uint32_t& out) {
        switch (ch) {
            case 't': out = '\t'; return true;
            case 'n': out = '\n'; return true;
            case 'r': out = '\r'; return true;
            case 'f': out = '\f'; return true;
            case 'a': out = 7; return true;
            case 'e': out = 27; return true;
            case '0': {
                // \0n, \0nn, \0mnn: octal
                int n = 0, digits = 0;
                while (more() && peek() >= '0' && peek() <= '7' && digits < 3) {
                    const int next = n * 8 + static_cast<int>(peek() - '0');
                    if (next > 0377) break;
                    n = next;
                    at_++;
                    digits++;
                }
                if (digits == 0) fail("Illegal octal escape sequence", static_cast<int>(at_));
                out = static_cast<uint32_t>(n);
                return true;
            }
            case 'x':
                if (more() && peek() == '{') {
                    at_++;
                    uint32_t n = 0;
                    size_t digits = 0;
                    while (more() && peek() != '}') {
                        const char32_t d = s_[at_++];
                        int v;
                        if (d >= '0' && d <= '9') v = static_cast<int>(d - '0');
                        else if (d >= 'a' && d <= 'f') v = static_cast<int>(d - 'a' + 10);
                        else if (d >= 'A' && d <= 'F') v = static_cast<int>(d - 'A' + 10);
                        else fail("Illegal hexadecimal escape sequence", static_cast<int>(at_) - 1);
                        n = n * 16 + static_cast<uint32_t>(v);
                        if (n > 0x10FFFF) fail("Hexadecimal codepoint is too big", static_cast<int>(at_) - 1);
                        digits++;
                    }
                    if (!more() || digits == 0) fail("Unclosed hexadecimal escape sequence", static_cast<int>(at_));
                    at_++;
                    out = n;
                    return true;
                }
                out = hexDigits(2, begin);
                return true;
            case 'u': {
                uint32_t n = hexDigits(4, begin);
                // A surrogate pair written as two escapes is the one character.
                if (n >= 0xD800 && n < 0xDC00 && at_ + 6 <= s_.size() && s_[at_] == '\\' && s_[at_ + 1] == 'u') {
                    const size_t back = at_;
                    at_ += 2;
                    const uint32_t low = hexDigits(4, begin);
                    if (low >= 0xDC00 && low < 0xE000) n = 0x10000 + ((n - 0xD800) << 10) + (low - 0xDC00);
                    else at_ = back;
                }
                out = n;
                return true;
            }
            case 'c':
                if (!more()) fail("Illegal control escape sequence", static_cast<int>(at_));
                out = static_cast<uint32_t>(s_[at_++]) ^ 64;
                return true;
            default:
                break;
        }
        // Anything not a letter or digit stands for itself.
        if (!((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9'))) {
            out = static_cast<uint32_t>(ch);
            return true;
        }
        return false;
    }

    // \d, \w, \p{..} and the rest: the class they name, as tests; false where it names none.
    bool classEscape(char32_t ch, std::vector<std::pair<Pred, bool>>& preds) {
        switch (ch) {
            case 'd': preds.push_back({Pred::AsciiDigit, false}); return true;
            case 'D': preds.push_back({Pred::AsciiDigit, true}); return true;
            case 'w': preds.push_back({Pred::AsciiWord, false}); return true;
            case 'W': preds.push_back({Pred::AsciiWord, true}); return true;
            case 's': preds.push_back({Pred::AsciiSpace, false}); return true;
            case 'S': preds.push_back({Pred::AsciiSpace, true}); return true;
            case 'h': preds.push_back({Pred::HSpace, false}); return true;
            case 'H': preds.push_back({Pred::HSpace, true}); return true;
            case 'v': preds.push_back({Pred::VSpace, false}); return true;
            case 'V': preds.push_back({Pred::VSpace, true}); return true;
            case 'p':
            case 'P': {
                const bool opposite = ch == 'P';
                std::string name;
                const size_t begin = at_;
                if (more() && peek() == '{') {
                    at_++;
                    while (more() && peek() != '}') name.push_back(static_cast<char>(s_[at_++]));
                    if (!more()) fail("Unclosed character family", static_cast<int>(at_));
                    at_++;
                } else if (more()) {
                    name.push_back(static_cast<char>(s_[at_++]));
                } else {
                    fail("Illegal character family escape", static_cast<int>(at_));
                }
                bool negate = false;
                const Pred p = property(name, begin, negate);
                preds.push_back({p, opposite != negate});
                return true;
            }
            default:
                return false;
        }
    }

    Pred property(std::string name, size_t at, bool& negate) {
        static const std::map<std::string, Pred> posix = {
            {"Lower", Pred::PosixLower}, {"Upper", Pred::PosixUpper}, {"ASCII", Pred::Ascii}, {"Alpha", Pred::PosixAlpha},
            {"Digit", Pred::AsciiDigit}, {"Alnum", Pred::PosixAlnum}, {"Punct", Pred::PosixPunct}, {"Graph", Pred::PosixGraph},
            {"Print", Pred::PosixPrint}, {"Blank", Pred::Blank}, {"Cntrl", Pred::PosixCntrl}, {"XDigit", Pred::XDigit},
            {"Space", Pred::AsciiSpace},
            {"javaLowerCase", Pred::Lower}, {"javaUpperCase", Pred::Upper}, {"javaWhitespace", Pred::Whitespace},
            {"javaLetter", Pred::Letter}, {"javaDigit", Pred::Digit}, {"javaLetterOrDigit", Pred::LetterOrDigit},
            {"javaAlphabetic", Pred::Alphabetic}, {"javaSpaceChar", Pred::Separator},
        };
        static const std::map<std::string, Pred> categories = {
            {"L", Pred::Letter}, {"Lu", Pred::Upper}, {"Ll", Pred::Lower}, {"Lt", Pred::TitleOrModifier}, {"Lm", Pred::TitleOrModifier},
            {"Lo", Pred::OtherLetter}, {"M", Pred::Mark}, {"Mn", Pred::Mark}, {"Mc", Pred::Mark}, {"Me", Pred::Mark},
            {"N", Pred::Digit}, {"Nd", Pred::Digit}, {"Nl", Pred::Digit}, {"No", Pred::Digit},
            {"P", Pred::Punct}, {"Pc", Pred::Punct}, {"Pd", Pred::Punct}, {"Ps", Pred::Punct}, {"Pe", Pred::Punct},
            {"Pi", Pred::Punct}, {"Pf", Pred::Punct}, {"Po", Pred::Punct},
            {"S", Pred::Symbol}, {"Sm", Pred::Symbol}, {"Sc", Pred::Symbol}, {"Sk", Pred::Symbol}, {"So", Pred::Symbol},
            {"Z", Pred::Separator}, {"Zs", Pred::SpaceSeparator}, {"Zl", Pred::LineSeparator}, {"Zp", Pred::ParagraphSeparator},
            {"C", Pred::Control}, {"Cc", Pred::Control}, {"Cf", Pred::Format}, {"Cs", Pred::Surrogate}, {"Co", Pred::PrivateUse},
            {"Cn", Pred::Unassigned},
        };
        static const std::map<std::string, Pred> binary = {
            {"ALPHABETIC", Pred::Alphabetic}, {"LETTER", Pred::Letter}, {"LOWERCASE", Pred::Lower}, {"UPPERCASE", Pred::Upper},
            {"PUNCTUATION", Pred::Punct}, {"CONTROL", Pred::Control}, {"WHITE_SPACE", Pred::Whitespace}, {"WHITESPACE", Pred::Whitespace},
            {"DIGIT", Pred::Digit}, {"HEX_DIGIT", Pred::XDigit}, {"HEXDIGIT", Pred::XDigit}, {"ASSIGNED", Pred::Unassigned},
            {"WORD", Pred::UnicodeWord},
        };
        negate = false;
        if (auto f = posix.find(name); f != posix.end()) return f->second;
        std::string rest = name;
        for (const char* prefix : {"general_category=", "gc="})
            if (rest.rfind(prefix, 0) == 0) rest = rest.substr(std::strlen(prefix));
        if (rest.rfind("Is", 0) == 0 && rest.size() > 2) {
            const std::string after = rest.substr(2);
            if (auto f = categories.find(after); f != categories.end()) return f->second;
            std::string upper;
            for (char c : after) upper.push_back(static_cast<char>(c >= 'a' && c <= 'z' ? c - 32 : c));
            if (auto f = binary.find(upper); f != binary.end()) {
                negate = upper == "ASSIGNED";
                return f->second;
            }
        }
        if (auto f = categories.find(rest); f != categories.end()) return f->second;
        if (name == "all") return Pred::Any;
        fail("Unknown character property name {" + name + "}: Astra knows Java's POSIX and java classes, the general categories, and IsAlphabetic, "
             "IsLetter and the like; scripts and blocks (IsLatin, InGreek) wait for the text engine's Unicode data",
             static_cast<int>(at));
    }

    int escape(size_t begin) {
        if (!more()) fail("Unexpected internal error", static_cast<int>(at_));
        const char32_t ch = s_[at_++];
        switch (ch) {
            case 'b':
                if (more() && peek() == '{') fail("\\b{g} is not supported", static_cast<int>(at_));
                return assertNode(Op::WordBoundary);
            case 'B': return assertNode(Op::NotWordBoundary);
            case 'A': return assertNode(Op::Begin);
            case 'G': return assertNode(Op::PreviousEnd);
            case 'Z': return assertNode(Op::EndFinal);
            case 'z': return assertNode(Op::End);
            case 'R': {
                // (?>\r\n|[\n\x0B\f\r\x85  ])
                Node crlf{Node::Concat};
                Node cr{Node::Char};
                cr.c = '\r';
                Node lf{Node::Char};
                lf.c = '\n';
                crlf.kids = {add(cr), add(lf)};
                const int pair = add(crlf);
                const int one = classOf({{Pred::VSpace, false}});
                Node alt{Node::Alt};
                alt.kids = {pair, one};
                Node atomic{Node::Atomic};
                atomic.kids = {add(alt)};
                return add(atomic);
            }
            case 'X': fail("\\X (a grapheme cluster) is not supported", static_cast<int>(begin));
            case 'N': fail("\\N{name} is not supported", static_cast<int>(begin));
            case 'Q': {
                Node seq{Node::Concat};
                while (more()) {
                    if (peek() == '\\' && at_ + 1 < s_.size() && s_[at_ + 1] == 'E') {
                        at_ += 2;
                        break;
                    }
                    seq.kids.push_back(charNode(static_cast<uint32_t>(s_[at_++])));
                }
                return add(seq);
            }
            case 'E': return add(Node{Node::Empty});
            case 'k': {
                if (!more() || peek() != '<') fail("\\k is not followed by '<' for named capturing group", static_cast<int>(at_));
                at_++;
                Node ref{Node::BackRef};
                ref.groupName = groupName();
                ref.foldMode = (flags_ & kCaseInsensitive) ? ((flags_ & kUnicodeCase) ? 2 : 1) : 0;
                ref.nameAt = static_cast<int>(at_) - 1;
                return add(ref);
            }
            default:
                break;
        }
        if (ch >= '1' && ch <= '9') {
            // A back reference: as many digits as still name a group that exists by now.
            int n = static_cast<int>(ch - '0');
            while (more() && peek() >= '0' && peek() <= '9') {
                const int longer = n * 10 + static_cast<int>(peek() - '0');
                if (longer > p_.groups) break;
                n = longer;
                at_++;
            }
            Node ref{Node::BackRef};
            ref.group = n;
            ref.foldMode = (flags_ & kCaseInsensitive) ? ((flags_ & kUnicodeCase) ? 2 : 1) : 0;
            return add(ref);
        }
        std::vector<std::pair<Pred, bool>> preds;
        if (classEscape(ch, preds)) return classOf(std::move(preds));
        uint32_t c;
        if (charEscape(ch, begin, c)) return charNode(c);
        fail("Illegal/unsupported escape sequence", static_cast<int>(at_) - 1);
    }

    // After "[": the class, as an index into p_.classes.
    int charClass(size_t begin) {
        ClassNode node;
        if (more() && peek() == '^') {
            node.negated = true;
            at_++;
        }
        const int self = static_cast<int>(p_.classes.size());
        p_.classes.push_back(ClassNode{});
        p_.classFold.push_back(static_cast<uint8_t>((flags_ & kCaseInsensitive) ? ((flags_ & kUnicodeCase) ? 2 : 1) : 0));
        bool firstItem = true;
        int left = -1;  // what came before && in this class
        auto finishOperand = [&](ClassNode& part) -> int {
            const int index = static_cast<int>(p_.classes.size());
            p_.classes.push_back(std::move(part));
            p_.classFold.push_back(static_cast<uint8_t>((flags_ & kCaseInsensitive) ? ((flags_ & kUnicodeCase) ? 2 : 1) : 0));
            part = ClassNode{};
            return index;
        };
        ClassNode current;
        for (;;) {
            if (flags_ & kComments) skipComments();
            if (!more()) fail("Unclosed character class", static_cast<int>(s_.size()) - 1);
            const char32_t ch = s_[at_];
            if (ch == ']' && !firstItem) {
                at_++;
                break;
            }
            if (ch == '[') {
                at_++;
                current.kids.push_back(charClass(at_ - 1));
                firstItem = false;
                continue;
            }
            if (ch == '&' && at_ + 1 < s_.size() && s_[at_ + 1] == '&') {
                at_ += 2;
                const int operand = finishOperand(current);
                if (left < 0) {
                    left = operand;
                } else {
                    ClassNode both;
                    both.intersection = true;
                    both.a = left;
                    both.b = operand;
                    left = finishOperand(both);
                }
                firstItem = false;
                continue;
            }
            uint32_t lo;
            at_++;
            if (ch == '\\') {
                if (!more()) fail("Unclosed character class", static_cast<int>(at_));
                const char32_t e = s_[at_++];
                std::vector<std::pair<Pred, bool>> preds;
                if (classEscape(e, preds)) {
                    for (auto& p : preds) current.preds.push_back(p);
                    firstItem = false;
                    continue;
                }
                if (e == 'Q') {
                    while (more()) {
                        if (peek() == '\\' && at_ + 1 < s_.size() && s_[at_ + 1] == 'E') {
                            at_ += 2;
                            break;
                        }
                        const uint32_t q = s_[at_++];
                        current.ranges.push_back({q, q});
                    }
                    firstItem = false;
                    continue;
                }
                if (!charEscape(e, at_ - 2, lo)) fail("Illegal/unsupported escape sequence", static_cast<int>(at_) - 1);
            } else {
                lo = static_cast<uint32_t>(ch);
            }
            firstItem = false;
            // A range, unless the - ends the class or is followed by [.
            if (more() && peek() == '-' && at_ + 1 < s_.size() && s_[at_ + 1] != ']' && s_[at_ + 1] != '[') {
                at_++;
                uint32_t hi;
                const char32_t h = s_[at_++];
                if (h == '\\') {
                    if (!more()) fail("Unclosed character class", static_cast<int>(at_));
                    const char32_t e = s_[at_++];
                    if (!charEscape(e, at_ - 2, hi)) fail("Illegal character range", static_cast<int>(at_) - 1);
                } else {
                    hi = static_cast<uint32_t>(h);
                }
                if (hi < lo) fail("Illegal character range", static_cast<int>(at_) - 1);
                current.ranges.push_back({lo, hi});
            } else {
                current.ranges.push_back({lo, lo});
            }
        }
        (void)begin;
        if (left >= 0) {
            const int operand = finishOperand(current);
            node.intersection = true;
            node.a = left;
            node.b = operand;
        } else {
            node.ranges = std::move(current.ranges);
            node.preds = std::move(current.preds);
            node.kids = std::move(current.kids);
        }
        p_.classes[static_cast<size_t>(self)] = std::move(node);
        return self;
    }
};

// ---- Compiling ----

class Compiler {
public:
    Compiler(std::vector<Node>& nodes, Program& p) : n_(nodes), p_(p) {}

    bool canBeEmpty(int i) const {
        const Node& n = n_[static_cast<size_t>(i)];
        switch (n.kind) {
            case Node::Char: case Node::Class: case Node::Dot: return false;
            case Node::Empty: case Node::Assert: case Node::Look: case Node::BackRef: return true;
            case Node::Group: case Node::Atomic: return canBeEmpty(n.kids[0]);
            case Node::Concat:
                for (int k : n.kids)
                    if (!canBeEmpty(k)) return false;
                return true;
            case Node::Alt:
                for (int k : n.kids)
                    if (canBeEmpty(k)) return true;
                return false;
            case Node::Repeat: return n.min == 0 || canBeEmpty(n.kids[0]);
        }
        return true;
    }

    // The fewest and most characters it can match; most -1 for no end.
    void lengths(int i, int64_t& lo, int64_t& hi) const {
        const Node& n = n_[static_cast<size_t>(i)];
        switch (n.kind) {
            case Node::Char: case Node::Class: case Node::Dot: lo = hi = 1; return;
            case Node::Empty: case Node::Assert: case Node::Look: lo = hi = 0; return;
            case Node::BackRef: lo = 0; hi = -1; return;
            case Node::Group: case Node::Atomic: lengths(n.kids[0], lo, hi); return;
            case Node::Concat: {
                lo = hi = 0;
                for (int k : n.kids) {
                    int64_t a, b;
                    lengths(k, a, b);
                    lo += a;
                    hi = (hi < 0 || b < 0) ? -1 : hi + b;
                }
                return;
            }
            case Node::Alt: {
                lo = INT64_MAX;
                hi = 0;
                for (int k : n.kids) {
                    int64_t a, b;
                    lengths(k, a, b);
                    lo = std::min(lo, a);
                    hi = (hi < 0 || b < 0) ? -1 : std::max(hi, b);
                }
                return;
            }
            case Node::Repeat: {
                int64_t a, b;
                lengths(n.kids[0], a, b);
                lo = a * n.min;
                hi = (n.max < 0 || b < 0) ? (b == 0 ? 0 : -1) : b * n.max;
                return;
            }
        }
    }

    int emit(Inst inst) {
        if (p_.code.size() > 2000000) throw SyntaxError("The pattern is too large to compile", -1, "");
        p_.code.push_back(inst);
        return static_cast<int>(p_.code.size()) - 1;
    }
    Inst& at(int pc) { return p_.code[static_cast<size_t>(pc)]; }
    int here() const { return static_cast<int>(p_.code.size()); }

    // A part compiled as a program of its own (a look-around or an atomic group): its start.
    int sub(int node, bool behind) {
        std::vector<Inst> saved;
        saved.swap(p_.code);
        compile(node);
        if (behind) emit(Inst{Op::AtTarget});
        emit(Inst{Op::Match});
        std::vector<Inst> body;
        body.swap(p_.code);
        p_.code.swap(saved);
        // Placed after everything else; its jumps shift by where it lands.
        pending_.push_back(std::move(body));
        return -static_cast<int>(pending_.size());  // settled in finish()
    }

    void compile(int i) {
        const Node& n = n_[static_cast<size_t>(i)];
        switch (n.kind) {
            case Node::Empty: return;
            case Node::Char: {
                Inst inst{n.fold ? Op::CharFold : n.asciiFold ? Op::CharFoldAscii : Op::Char};
                inst.c = n.c;
                emit(inst);
                return;
            }
            case Node::Class: {
                Inst inst{Op::Class};
                inst.a = n.cls;
                emit(inst);
                return;
            }
            case Node::Dot: {
                Inst inst{n.dotAll ? Op::AnyAll : Op::AnyLine};
                inst.flag = n.unixLines;
                emit(inst);
                return;
            }
            case Node::Assert: {
                Inst inst{n.assertOp};
                inst.flag = n.unixLines;
                emit(inst);
                return;
            }
            case Node::Group: {
                if (n.group >= 0) {
                    Inst open{Op::Save};
                    open.a = 2 * n.group;
                    emit(open);
                }
                compile(n.kids[0]);
                if (n.group >= 0) {
                    Inst close{Op::Save};
                    close.a = 2 * n.group + 1;
                    emit(close);
                }
                return;
            }
            case Node::Concat:
                for (int k : n.kids) compile(k);
                return;
            case Node::Alt: {
                std::vector<int> jumps;
                for (size_t k = 0; k < n.kids.size(); k++) {
                    if (k + 1 < n.kids.size()) {
                        const int split = emit(Inst{Op::Split});
                        at(split).a = here();
                        compile(n.kids[k]);
                        jumps.push_back(emit(Inst{Op::Jmp}));
                        at(split).b = here();
                    } else {
                        compile(n.kids[k]);
                    }
                }
                for (int j : jumps) at(j).a = here();
                return;
            }
            case Node::BackRef: {
                if (n.group > p_.groups || n.group < 1) {
                    // A reference to a group that never takes part matches nothing, as in Java.
                    Inst inst{Op::BackRef};
                    inst.a = -1;
                    emit(inst);
                    return;
                }
                Inst inst{Op::BackRef};
                inst.a = n.group;
                inst.c = static_cast<uint32_t>(n.foldMode);
                emit(inst);
                return;
            }
            case Node::Look: {
                Inst inst{Op::Look};
                inst.flag = n.negative;
                inst.behind = n.behind;
                if (n.behind) {
                    int64_t lo, hi;
                    lengths(n.kids[0], lo, hi);
                    inst.minLen = static_cast<int>(lo);
                    inst.maxLen = hi < 0 || hi > INT32_MAX ? INT32_MAX : static_cast<int>(hi);
                }
                inst.a = sub(n.kids[0], n.behind);
                emit(inst);
                return;
            }
            case Node::Atomic: {
                Inst inst{Op::Atomic};
                inst.a = sub(n.kids[0], false);
                emit(inst);
                return;
            }
            case Node::Repeat: {
                if (n.greed == Node::Possessive && i != greedyOnce_) {
                    // X*+ is (?>X*): this node again, compiled as a greedy repeat, in a program of its own.
                    const int saved = greedyOnce_;
                    greedyOnce_ = i;
                    Inst inst{Op::Atomic};
                    inst.a = sub(i, false);
                    greedyOnce_ = saved;
                    emit(inst);
                    return;
                }
                const int child = n.kids[0];
                const int min = n.min, max = n.max;
                const bool lazy = n.greed == Node::Lazy && i != greedyOnce_;
                for (int k = 0; k < min; k++) compile(child);
                if (max < 0) {
                    // X*: L: split body, out; body: [mark] X [check] jmp L
                    const bool empty = canBeEmpty(child);
                    const int reg = empty ? p_.loops++ : -1;
                    const int loop = emit(Inst{Op::Split});
                    const int body = here();
                    if (empty) {
                        Inst mark{Op::LoopSet};
                        mark.a = reg;
                        emit(mark);
                    }
                    compile(child);
                    int checkAt = -1;
                    if (empty) {
                        Inst check{Op::LoopCheck};
                        check.a = reg;
                        checkAt = emit(check);
                    }
                    Inst back{Op::Jmp};
                    back.a = loop;
                    emit(back);
                    const int out = here();
                    if (checkAt >= 0) at(checkAt).b = out;
                    at(loop).a = lazy ? out : body;
                    at(loop).b = lazy ? body : out;
                } else {
                    // X{0,k} after the required ones: k optional copies, each inside the last.
                    std::vector<int> splits;
                    for (int k = min; k < max; k++) {
                        const int split = emit(Inst{Op::Split});
                        splits.push_back(split);
                        at(split).a = here();
                        compile(child);
                    }
                    const int out = here();
                    for (int split : splits) {
                        const int body = at(split).a;
                        at(split).a = lazy ? out : body;
                        at(split).b = lazy ? body : out;
                    }
                }
                return;
            }
        }
    }

    // Places the programs of look-arounds and atomic groups after the main one, settling where
    // each starts, and their own jumps.
    void finish() {
        const int mainEnd = here();
        p_.splitNumber.assign(static_cast<size_t>(mainEnd), -1);
        bool memo = p_.loops == 0;
        for (int pc = 0; pc < mainEnd; pc++) {
            const Op op = p_.code[static_cast<size_t>(pc)].op;
            if (op == Op::Split) p_.splitNumber[static_cast<size_t>(pc)] = p_.splits++;
            if (op == Op::BackRef) memo = false;
        }
        // A back reference in a part's program counts too.
        for (const auto& body : pending_)
            for (const Inst& inst : body)
                if (inst.op == Op::BackRef || inst.op == Op::LoopSet) memo = false;
        p_.memo = memo && p_.splits > 0;
        for (size_t k = 0; k < pending_.size(); k++) {
            const int base = here();
            starts_.push_back(base);
            for (Inst inst : pending_[k]) {
                if (inst.op == Op::Split) {
                    inst.a += base;
                    inst.b += base;
                } else if (inst.op == Op::LoopCheck) {
                    inst.b += base;
                } else if (inst.op == Op::Jmp) {
                    inst.a += base;
                }
                p_.code.push_back(inst);
            }
        }
        // Each Look/Atomic names its program by -(k + 1).
        for (Inst& inst : p_.code)
            if ((inst.op == Op::Look || inst.op == Op::Atomic) && inst.a < 0) inst.a = starts_[static_cast<size_t>(-inst.a - 1)];
    }

private:
    std::vector<Node>& n_;
    Program& p_;
    std::vector<std::vector<Inst>> pending_;
    std::vector<int> starts_;
    int greedyOnce_ = -1;  // a possessive repeat being compiled as the greedy one inside it
};

// Where a match must start: the start of the input, or at a given character.
void firstOf(const std::vector<Node>& nodes, int i, Program& p) {
    const Node& n = nodes[static_cast<size_t>(i)];
    switch (n.kind) {
        case Node::Char:
            if (!n.fold && !n.asciiFold) {
                p.hasFirst = true;
                p.first = n.c;
            }
            return;
        case Node::Assert:
            if (n.assertOp == Op::Begin) p.anchored = true;
            return;
        case Node::Group:
            firstOf(nodes, n.kids[0], p);
            return;
        case Node::Concat:
            for (int k : n.kids) {
                const Node& kid = nodes[static_cast<size_t>(k)];
                if (kid.kind == Node::Empty) continue;
                firstOf(nodes, k, p);
                return;
            }
            return;
        case Node::Repeat:
            if (n.min > 0) firstOf(nodes, n.kids[0], p);
            return;
        default:
            return;
    }
}

}  // namespace

// ---- Matching ----

namespace {

struct Entry {
    enum Kind : uint8_t { Alt, Capture, Register } kind;
    int pc;
    int64_t value;  // Alt: the position; Capture, Register: the value to put back
    int slot;
};

class Runner {
public:
    Runner(const Program& p, const ObjString* s, std::vector<int64_t>& caps, size_t searchStart)
        : p_(p), s_(s), n_(s->length), caps_(caps), regs_(static_cast<size_t>(p.loops), -1), searchStart_(searchStart) {}

    // Runs the program from `pc` at `pos`; on success `end` is where the match ended. With a
    // target, a look-behind's program, it must end there; with requireEnd, at the end of the input.
    bool run(int pc, int64_t pos, int64_t& end, int64_t target = -1, bool requireEnd = false) {
        const size_t base = stack_.size();
        for (;;) {
            const Inst& inst = p_.code[static_cast<size_t>(pc)];
            bool ok = true;
            switch (inst.op) {
                case Op::Char:
                    ok = pos < static_cast<int64_t>(n_) && s_->at(static_cast<size_t>(pos)) == inst.c;
                    if (ok) pos++, pc++;
                    break;
                case Op::CharFold:
                    ok = pos < static_cast<int64_t>(n_) && fold(s_->at(static_cast<size_t>(pos))) == inst.c;
                    if (ok) pos++, pc++;
                    break;
                case Op::CharFoldAscii: {
                    const uint32_t c = pos < static_cast<int64_t>(n_) ? s_->at(static_cast<size_t>(pos)) : 0;
                    ok = pos < static_cast<int64_t>(n_) && c < 0x80 && (c | 0x20) == inst.c;
                    if (ok) pos++, pc++;
                    break;
                }
                case Op::AnyAll:
                    ok = pos < static_cast<int64_t>(n_);
                    if (ok) pos++, pc++;
                    break;
                case Op::AnyLine:
                    ok = pos < static_cast<int64_t>(n_) && !isLineTerminator(s_->at(static_cast<size_t>(pos)), inst.flag);
                    if (ok) pos++, pc++;
                    break;
                case Op::Class:
                    ok = pos < static_cast<int64_t>(n_) && inClass(p_, inst.a, s_->at(static_cast<size_t>(pos)));
                    if (ok) pos++, pc++;
                    break;
                case Op::Split:
                    if (p_.memo && target < 0 && static_cast<size_t>(pc) < p_.splitNumber.size()) {
                        if (++splitsTaken_ > kMemoAfter && memo_.empty() && !memoRefused_) startMemo();
                        if (!memo_.empty()) {
                            const size_t bit = static_cast<size_t>(p_.splitNumber[static_cast<size_t>(pc)]) * (n_ + 1) + static_cast<size_t>(pos);
                            if ((memo_[bit >> 6] >> (bit & 63)) & 1) {
                                ok = false;  // tried from here before, and it failed
                                break;
                            }
                            memo_[bit >> 6] |= uint64_t{1} << (bit & 63);
                        }
                    }
                    stack_.push_back(Entry{Entry::Alt, inst.b, pos, 0});
                    pc = inst.a;
                    break;
                case Op::Jmp:
                    pc = inst.a;
                    break;
                case Op::Save:
                    stack_.push_back(Entry{Entry::Capture, 0, caps_[static_cast<size_t>(inst.a)], inst.a});
                    caps_[static_cast<size_t>(inst.a)] = pos;
                    pc++;
                    break;
                case Op::LoopSet:
                    stack_.push_back(Entry{Entry::Register, 0, regs_[static_cast<size_t>(inst.a)], inst.a});
                    regs_[static_cast<size_t>(inst.a)] = pos;
                    pc++;
                    break;
                case Op::LoopCheck:
                    // An iteration that took nothing ends the loop, what it captured kept, as in Java.
                    pc = regs_[static_cast<size_t>(inst.a)] == pos ? inst.b : pc + 1;
                    break;
                case Op::Match:
                    if ((target >= 0 && pos != target) || (requireEnd && pos != static_cast<int64_t>(n_))) {
                        ok = false;
                        break;
                    }
                    end = pos;
                    return true;
                case Op::AtTarget:
                    ok = pos == target;
                    if (ok) pc++;
                    break;
                case Op::Begin:
                    ok = pos == 0;
                    if (ok) pc++;
                    break;
                case Op::End:
                    ok = pos == static_cast<int64_t>(n_);
                    if (ok) pc++;
                    break;
                case Op::LineStart:
                    ok = lineStart(pos, inst.flag);
                    if (ok) pc++;
                    break;
                case Op::EndFinal:
                    ok = dollar(pos, false, inst.flag);
                    if (ok) pc++;
                    break;
                case Op::LineEnd:
                    ok = dollar(pos, true, inst.flag);
                    if (ok) pc++;
                    break;
                case Op::WordBoundary:
                case Op::NotWordBoundary: {
                    const bool before = pos > 0 && isAsciiWord(s_->at(static_cast<size_t>(pos - 1)));
                    const bool after = pos < static_cast<int64_t>(n_) && isAsciiWord(s_->at(static_cast<size_t>(pos)));
                    ok = (before != after) == (inst.op == Op::WordBoundary);
                    if (ok) pc++;
                    break;
                }
                case Op::PreviousEnd:
                    ok = pos == static_cast<int64_t>(searchStart_);
                    if (ok) pc++;
                    break;
                case Op::BackRef: {
                    ok = inst.a >= 0;
                    if (!ok) break;
                    const int64_t from = caps_[static_cast<size_t>(2 * inst.a)], to = caps_[static_cast<size_t>(2 * inst.a + 1)];
                    ok = from >= 0 && to >= from;
                    if (!ok) break;
                    const int64_t length = to - from;
                    ok = pos + length <= static_cast<int64_t>(n_);
                    for (int64_t k = 0; ok && k < length; k++) {
                        const uint32_t a = s_->at(static_cast<size_t>(from + k)), b = s_->at(static_cast<size_t>(pos + k));
                        if (inst.c == 2) ok = fold(a) == fold(b);
                        else if (inst.c == 1) ok = a == b || (a < 0x80 && b < 0x80 && ((a | 0x20) == (b | 0x20)) && (a | 0x20) >= 'a' && (a | 0x20) <= 'z');
                        else ok = a == b;
                    }
                    if (ok) pos += length, pc++;
                    break;
                }
                case Op::Look: {
                    const size_t mark = stack_.size();
                    bool found = false;
                    if (!inst.behind) {
                        int64_t e;
                        found = run(inst.a, pos, e);
                    } else {
                        for (int64_t k = inst.minLen; k <= inst.maxLen && !found; k++) {
                            if (pos - k < 0) break;
                            int64_t e;
                            found = run(inst.a, pos - k, e, pos);
                        }
                    }
                    if (inst.flag) {
                        // Negative: what the part did is undone either way.
                        if (found) unwindTo(mark);
                        ok = !found;
                    } else {
                        ok = found;
                        if (found) keepOnlyRestores(mark);
                    }
                    if (ok) pc++;
                    break;
                }
                case Op::Atomic: {
                    const size_t mark = stack_.size();
                    int64_t e;
                    ok = run(inst.a, pos, e);
                    if (ok) {
                        keepOnlyRestores(mark);
                        pos = e;
                        pc++;
                    }
                    break;
                }
            }
            if (ok) continue;
            // Back to the last alternative, putting back what was changed since.
            bool resumed = false;
            while (stack_.size() > base) {
                const Entry e = stack_.back();
                stack_.pop_back();
                if (e.kind == Entry::Capture) {
                    caps_[static_cast<size_t>(e.slot)] = e.value;
                } else if (e.kind == Entry::Register) {
                    regs_[static_cast<size_t>(e.slot)] = e.value;
                } else {
                    pc = e.pc;
                    pos = e.value;
                    resumed = true;
                    break;
                }
            }
            if (!resumed) return false;
        }
    }

    // Leaves only the entries that put captures back, above `mark`: the part's alternatives
    // are forgotten, and what it captured is still undone should the match back out past it.
    void keepOnlyRestores(size_t mark) {
        size_t out = mark;
        for (size_t k = mark; k < stack_.size(); k++)
            if (stack_[k].kind != Entry::Alt) stack_[out++] = stack_[k];
        stack_.resize(out);
    }
    void unwindTo(size_t mark) {
        while (stack_.size() > mark) {
            const Entry e = stack_.back();
            stack_.pop_back();
            if (e.kind == Entry::Capture) caps_[static_cast<size_t>(e.slot)] = e.value;
            else if (e.kind == Entry::Register) regs_[static_cast<size_t>(e.slot)] = e.value;
        }
    }
    void reset() {
        stack_.clear();
        std::fill(regs_.begin(), regs_.end(), -1);
        // What failed from a branch point at a position fails whatever the start: the table is kept.
    }

private:
    const Program& p_;
    const ObjString* s_;
    const size_t n_;
    std::vector<int64_t>& caps_;
    std::vector<int64_t> regs_;
    std::vector<Entry> stack_;
    const size_t searchStart_;
    // Which branch points failed at which positions, once the search has taken enough branches
    // for it to pay; refused where the table would be too large.
    static constexpr uint64_t kMemoAfter = 20000;
    static constexpr uint64_t kMemoMostBits = uint64_t{1} << 31;  // 256 MB
    uint64_t splitsTaken_ = 0;
    std::vector<uint64_t> memo_;
    bool memoRefused_ = false;
    void startMemo() {
        const uint64_t bits = static_cast<uint64_t>(p_.splits) * (n_ + 1);
        if (bits > kMemoMostBits) {
            memoRefused_ = true;
            return;
        }
        memo_.assign(static_cast<size_t>((bits + 63) / 64), 0);
    }

    uint32_t ch(int64_t i) const { return s_->at(static_cast<size_t>(i)); }

    // ^ in MULTILINE, as Java has it: at the start, or after a line terminator but not between
    // \r and \n, and never at the end of the input.
    bool lineStart(int64_t i, bool unix) const {
        const int64_t end = static_cast<int64_t>(n_);
        if (i > 0) {
            const uint32_t before = ch(i - 1);
            if (unix) {
                if (before != '\n') return false;
            } else {
                if (!isLineTerminator(before, false)) return false;
                if (before == '\r' && i < end && ch(i) == '\n') return false;
            }
        }
        return i != end;
    }

    // $ (and \Z), as Java's Dollar: at the end, or before a line terminator; without
    // MULTILINE only one that ends the input.
    bool dollar(int64_t i, bool multiline, bool unix) const {
        const int64_t end = static_cast<int64_t>(n_);
        if (unix) {
            if (i < end) {
                if (ch(i) != '\n') return false;
                return multiline || i == end - 1;
            }
            return true;
        }
        if (!multiline) {
            if (i < end - 2) return false;
            if (i == end - 2) {
                if (ch(i) != '\r' || ch(i + 1) != '\n') return false;
            }
        }
        if (i < end) {
            const uint32_t c = ch(i);
            if (c == '\n') {
                if (i > 0 && ch(i - 1) == '\r') return false;
                return true;
            }
            if (c == '\r' || c == 0x85 || c == 0x2028 || c == 0x2029) return true;
            return false;
        }
        return true;
    }
};

}  // namespace

// ---- The pattern ----

Pattern::Pattern(const std::u32string& pattern, uint32_t flags) : program_(std::make_unique<Program>()) {
    Program& p = *program_;
    std::string utf8;
    for (char32_t ch : pattern) appendUtf8(utf8, static_cast<uint32_t>(ch));
    Parser parser(pattern, flags, p);
    int root;
    try {
        root = parser.parse();
    } catch (const SyntaxError&) {
        throw;
    }
    std::vector<Node>& nodes = parser.nodes_;
    Compiler compiler(nodes, p);
    try {
        compiler.compile(root);
        compiler.emit(Inst{Op::Match});
        compiler.finish();
    } catch (const SyntaxError& e) {
        throw SyntaxError(e.description, -1, utf8);
    }
    p.start = 0;
    firstOf(nodes, root, p);
    // Which ASCII characters each class holds, worked out once.
    p.asciiBits.resize(p.classes.size());
    for (size_t k = 0; k < p.classes.size(); k++) {
        std::array<uint64_t, 2> bits{0, 0};
        for (uint32_t c = 0; c < 128; c++) {
            bool in = inClassRaw(p, static_cast<int>(k), c);
            const bool letter = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
            if (!in && p.classFold[k] != 0 && letter) {
                const uint32_t other = c ^ 0x20;
                in = inClassRaw(p, static_cast<int>(k), other);
                // A letter past ASCII whose other case is this one: the Kelvin sign is K, the long s is S.
                if (!in && p.classFold[k] == 2) {
                    if (c == 'k' || c == 'K') in = inClassRaw(p, static_cast<int>(k), 0x212A);
                    if (c == 's' || c == 'S') in = in || inClassRaw(p, static_cast<int>(k), 0x017F);
                }
            }
            if (in) bits[c >> 6] |= uint64_t{1} << (c & 63);
        }
        p.asciiBits[k] = bits;
    }
}

Pattern::~Pattern() = default;

int Pattern::groupCount() const { return program_->groups; }
const std::map<std::string, int>& Pattern::groupNames() const { return program_->names; }

bool Pattern::match(const ObjString* text, size_t from, Mode mode, std::vector<int64_t>& groups) const {
    const Program& p = *program_;
    groups.assign(static_cast<size_t>(2 * (p.groups + 1)), -1);
    const size_t n = text->length;
    if (from > n) return false;
    Runner runner(p, text, groups, from);
    if (mode != Mode::Search) {
        int64_t end;
        if (!runner.run(p.start, static_cast<int64_t>(from), end, -1, mode == Mode::Entire)) return false;
        groups[0] = static_cast<int64_t>(from);
        groups[1] = end;
        return true;
    }
    for (size_t at = from; at <= n; at++) {
        if (p.anchored && at > 0) return false;
        if (p.hasFirst) {
            while (at < n && text->at(at) != p.first) at++;
            if (at >= n) return false;
        }
        runner.reset();
        int64_t end;
        if (runner.run(p.start, static_cast<int64_t>(at), end)) {
            groups[0] = static_cast<int64_t>(at);
            groups[1] = end;
            return true;
        }
        std::fill(groups.begin(), groups.end(), -1);
    }
    return false;
}

}  // namespace astra::regex
