// What Xml.scan (src/std/xml.jade) is built on: one pass in C++ over an element and all that
// is inside it, giving where each part is as numbers in one List<int>, from which the XML
// reader in Jade builds its tree. The pass takes well-formed XML of ASCII names only; at
// anything else (a malformed tag, a name past ASCII, `]]>` in text, an end tag that closes
// the wrong element) it stops and says where, and the reader goes on from there a part at a
// time, to read it or to say what is wrong with it.

#include <cstring>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include "builtins.h"
#include "vm.h"

namespace jade {

namespace {

// The kinds of record, each its kind and then its numbers (see Xml.scan).
enum : int64_t { kStop = 0, kStart = 1, kEnd = 2, kText = 3, kComment = 4, kInstruction = 5, kCData = 6 };

constexpr size_t kNone = static_cast<size_t>(-1);

template <typename Unit>
class Scanner {
public:
    Scanner(const Unit* text, size_t size, std::vector<int64_t>& out) : t_(text), n_(size), out_(out) {}

    void scan(size_t from) {
        size_t pos = from;
        std::vector<std::pair<size_t, size_t>> open;  // the names of the elements not yet closed
        bool started = false;
        for (;;) {
            if (started && open.empty()) return stop(pos);
            const size_t lt = find('<', pos);
            if (lt == kNone) return stop(pos);
            if (lt > pos) {
                if (!started) return stop(pos);
                int64_t flags = 0;
                for (size_t i = pos; i < lt; i++) {
                    const Unit c = t_[i];
                    if (c == '&') flags |= 1;
                    else if (c == '>' && i >= pos + 2 && t_[i - 1] == ']' && t_[i - 2] == ']') return stop(pos);
                }
                push({kText, static_cast<int64_t>(pos), static_cast<int64_t>(lt), flags});
            }
            pos = lt;
            if (lt + 1 >= n_) return stop(lt);
            const Unit next = t_[lt + 1];
            if (next == '/') {
                size_t end;
                if (open.empty() || !name(lt + 2, end)) return stop(lt);
                const auto [openStart, openEnd] = open.back();
                if (end - (lt + 2) != openEnd - openStart || !std::equal(t_ + lt + 2, t_ + end, t_ + openStart)) return stop(lt);
                const size_t p = skipSpace(end);
                if (p >= n_ || t_[p] != '>') return stop(lt);
                push({kEnd, static_cast<int64_t>(lt)});
                open.pop_back();
                pos = p + 1;
            } else if (next == '!') {
                if (!started) return stop(lt);
                if (startsWith(lt, "<!--")) {
                    const size_t end = findText("--", lt + 4);
                    if (end == kNone || end + 2 >= n_ || t_[end + 2] != '>') return stop(lt);
                    push({kComment, static_cast<int64_t>(lt), static_cast<int64_t>(lt + 4), static_cast<int64_t>(end)});
                    pos = end + 3;
                } else if (startsWith(lt, "<![CDATA[")) {
                    const size_t end = findText("]]>", lt + 9);
                    if (end == kNone) return stop(lt);
                    push({kCData, static_cast<int64_t>(lt), static_cast<int64_t>(lt + 9), static_cast<int64_t>(end)});
                    pos = end + 3;
                } else {
                    return stop(lt);
                }
            } else if (next == '?') {
                size_t end;
                if (!started || !name(lt + 2, end)) return stop(lt);
                if (end - (lt + 2) == 3 && (t_[lt + 2] | 0x20) == 'x' && (t_[lt + 3] | 0x20) == 'm' && (t_[lt + 4] | 0x20) == 'l') return stop(lt);
                const size_t close = findText("?>", end);
                if (close == kNone) return stop(lt);
                size_t data = end;
                if (close > end) {
                    if (!space(t_[end])) return stop(lt);
                    while (data < close && space(t_[data])) data++;
                }
                push({kInstruction, static_cast<int64_t>(lt), static_cast<int64_t>(lt + 2), static_cast<int64_t>(end), static_cast<int64_t>(data),
                      static_cast<int64_t>(close)});
                pos = close + 2;
            } else {
                size_t nameEnd;
                if (!name(lt + 1, nameEnd)) return stop(lt);
                const size_t head = out_.size();
                push({kStart, static_cast<int64_t>(lt), static_cast<int64_t>(lt + 1), static_cast<int64_t>(nameEnd), nameNumber(lt + 1, nameEnd), 0, 0});
                size_t p = nameEnd;
                int64_t count = 0;
                for (;;) {
                    const size_t q = skipSpace(p);
                    if (q < n_ && (t_[q] == '>' || t_[q] == '/')) {
                        p = q;
                        break;
                    }
                    size_t attrEnd;
                    if (q == p || !name(q, attrEnd)) return drop(head, lt);
                    size_t r = skipSpace(attrEnd);
                    if (r >= n_ || t_[r] != '=') return drop(head, lt);
                    r = skipSpace(r + 1);
                    if (r >= n_ || (t_[r] != '"' && t_[r] != '\'')) return drop(head, lt);
                    const Unit quote = t_[r];
                    const size_t valueStart = r + 1;
                    size_t v = valueStart;
                    int64_t flags = 0;
                    for (; v < n_ && t_[v] != quote; v++) {
                        const Unit c = t_[v];
                        if (c == '<') return drop(head, lt);
                        if (c == '&') flags |= 1;
                        else if (c == '\n' || c == '\t' || c == '\r') flags |= 2;
                    }
                    if (v >= n_) return drop(head, lt);
                    for (int64_t k = 0; k < count; k++) {  // the same name twice: left to the reader to say so
                        const size_t at = head + 7 + static_cast<size_t>(k) * 6;
                        const size_t s = static_cast<size_t>(out_[at]), e = static_cast<size_t>(out_[at + 1]);
                        if (e - s == attrEnd - q && std::equal(t_ + s, t_ + e, t_ + q)) return drop(head, lt);
                    }
                    push({static_cast<int64_t>(q), static_cast<int64_t>(attrEnd), nameNumber(q, attrEnd), static_cast<int64_t>(valueStart), static_cast<int64_t>(v), flags});
                    count++;
                    p = v + 1;
                }
                bool selfClosed = false;
                if (t_[p] == '/') {
                    if (p + 1 >= n_ || t_[p + 1] != '>') return drop(head, lt);
                    selfClosed = true;
                    p += 2;
                } else {
                    p += 1;
                }
                out_[head + 5] = selfClosed ? 1 : 0;
                out_[head + 6] = count;
                started = true;
                if (!selfClosed) open.push_back({lt + 1, nameEnd});
                pos = p;
            }
        }
    }

private:
    const Unit* t_;
    const size_t n_;
    std::vector<int64_t>& out_;
    // Each name met, numbered in the order first met: a reader makes each name's string once.
    std::unordered_map<std::string_view, int64_t> names_;

    int64_t nameNumber(size_t start, size_t end) {
        const std::string_view key(reinterpret_cast<const char*>(t_ + start), (end - start) * sizeof(Unit));
        return names_.emplace(key, static_cast<int64_t>(names_.size())).first->second;
    }

    void push(std::initializer_list<int64_t> numbers) { out_.insert(out_.end(), numbers); }
    void stop(size_t at) { push({kStop, static_cast<int64_t>(at)}); }
    // A start tag given up part way: what was written of it taken back.
    void drop(size_t head, size_t at) {
        out_.resize(head);
        stop(at);
    }

    static bool space(Unit c) { return c == ' ' || c == '\n' || c == '\t' || c == '\r'; }
    size_t skipSpace(size_t p) const {
        while (p < n_ && space(t_[p])) p++;
        return p;
    }
    // An ASCII name at `p`, its end in `end`; false for none, or one with a character past ASCII.
    bool name(size_t p, size_t& end) const {
        auto first = [](Unit c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_' || c == ':'; };
        if (p >= n_ || !first(t_[p])) return false;
        size_t q = p + 1;
        while (q < n_) {
            const Unit c = t_[q];
            if (first(c) || (c >= '0' && c <= '9') || c == '.' || c == '-') q++;
            else if (c >= 0x80) return false;
            else break;
        }
        end = q;
        return true;
    }
    size_t find(Unit c, size_t from) const {
        if constexpr (sizeof(Unit) == 1) {
            if (from >= n_) return kNone;
            const void* hit = std::memchr(t_ + from, c, n_ - from);
            return hit == nullptr ? kNone : static_cast<size_t>(static_cast<const Unit*>(hit) - t_);
        } else {
            for (size_t i = from; i < n_; i++)
                if (t_[i] == c) return i;
            return kNone;
        }
    }
    size_t findText(const char* s, size_t from) const {
        const size_t len = std::strlen(s);
        for (size_t i = find(static_cast<Unit>(s[0]), from); i != kNone; i = find(static_cast<Unit>(s[0]), i + 1)) {
            if (i + len > n_) return kNone;
            size_t k = 1;
            while (k < len && t_[i + k] == static_cast<Unit>(s[k])) k++;
            if (k == len) return i;
        }
        return kNone;
    }
    bool startsWith(size_t at, const char* s) const {
        const size_t len = std::strlen(s);
        if (at + len > n_) return false;
        for (size_t k = 0; k < len; k++)
            if (t_[at + k] != static_cast<Unit>(s[k])) return false;
        return true;
    }
};

// A List<int> of the numbers, kept packed.
Value intList(VM& vm, const std::vector<int64_t>& numbers) {
    ObjList* list = newList(vm);
    TempRoot keep(vm, Value::object(list));
    vm.typeList(list, vm.intListType());
    if (numbers.empty()) return Value::object(list);
    if (list->store() == ListStore::Ints) {
        list->pack->block.resize(numbers.size() * sizeof(int64_t));
        std::memcpy(list->pack->block.data(), numbers.data(), numbers.size() * sizeof(int64_t));
        list->pack->count = numbers.size();
        vm.account(list->pack->block.size());
    } else {
        for (int64_t n : numbers) vm.listAdd(list, Value::integer(n));
    }
    return Value::object(list);
}

}  // namespace

void registerXml(VM& vm) {
    ObjClass* holder = vm.alloc<ObjClass>(vm.intern("Xml"));
    auto def = [&](const char* name, int minArgs, int maxArgs, NativeFn fn) {
        holder->statics.set(vm.intern(name), Value::object(vm.alloc<ObjNative>(std::string("Xml.") + name, minArgs, maxArgs, fn)));
    };
    // The parts of the element whose start tag is at `from`, and of all inside it (Xml.scan).
    def("scan", 2, 2, [](VM& vm, int, Value* args) -> Value {
        if (!args[0].isString()) throw vm.runtimeError(Thrown::IllegalArgumentException, "Xml.scan reads a string, not a value of type " + vm.typeName(args[0]));
        const ObjString* text = asString(args[0]);
        const int64_t from = vm.toInt(args[1], "startIndex");
        if (from < 0 || static_cast<size_t>(from) > text->length)
            throw vm.runtimeError(Thrown::IndexOutOfBoundsException,
                                  "start index " + std::to_string(from) + " is out of bounds of a text of " + std::to_string(text->length) + " characters");
        static thread_local std::vector<int64_t> out;
        out.clear();
        const char* units = text->units().data();
        if (text->width == 1) Scanner<uint8_t>(reinterpret_cast<const uint8_t*>(units), text->length, out).scan(static_cast<size_t>(from));
        else if (text->width == 2) Scanner<uint16_t>(reinterpret_cast<const uint16_t*>(units), text->length, out).scan(static_cast<size_t>(from));
        else Scanner<uint32_t>(reinterpret_cast<const uint32_t*>(units), text->length, out).scan(static_cast<size_t>(from));
        return intList(vm, out);
    });
    vm.defineGlobal("__xml", Value::object(holder));
}

}  // namespace jade
