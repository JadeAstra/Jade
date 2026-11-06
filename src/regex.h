// Regular expressions (N5, M8): Java's syntax, as Kotlin's Regex takes it, matched by code point
// over Astra's strings. A pattern is compiled once into a small program; matching runs it with
// backtracking on a stack of its own, so a long text cannot run the C++ stack out.
#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace astra {

struct ObjString;

namespace regex {

// The options, as Java's Pattern flags (Kotlin's RegexOption).
enum Flags : uint32_t {
    kCaseInsensitive = 1,
    kMultiline = 2,
    kDotAll = 4,
    kComments = 8,
    kLiteral = 16,
    kUnixLines = 32,
    kUnicodeCase = 64,  // with kCaseInsensitive, every letter; without it, ASCII ones only
};

// What is wrong with a pattern, with where (as Java's PatternSyntaxException says it).
struct SyntaxError : std::runtime_error {
    std::string description;
    int index;
    SyntaxError(std::string d, int at, const std::string& pattern);
};

struct Program;

// A compiled pattern.
class Pattern {
public:
    Pattern(const std::u32string& pattern, uint32_t flags);  // throws SyntaxError
    ~Pattern();
    Pattern(const Pattern&) = delete;
    Pattern& operator=(const Pattern&) = delete;

    int groupCount() const;  // capturing groups, not counting the whole match
    const std::map<std::string, int>& groupNames() const;

    // How a match is looked for.
    enum class Mode { Search, AtStart, Entire };
    // A match in `text` from `from` (Search: the first at or after it; AtStart: one starting
    // there; Entire: one starting there and running to the end). On success `groups` holds
    // start and end for the whole match and each group, -1 for a group that took no part.
    bool match(const ObjString* text, size_t from, Mode mode, std::vector<int64_t>& groups) const;

private:
    std::unique_ptr<Program> program_;
};

}  // namespace regex
}  // namespace astra
