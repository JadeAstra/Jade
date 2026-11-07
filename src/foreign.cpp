// Foreign functions (M8): what Library, Memory and Foreign (src/std/foreign.astra) are built on.
// A program loads a library of the system's, finds a function in it, and calls it with whole
// numbers, pointers and text; structures are blocks of native memory, read and written at
// offsets with every access checked against the block's size.
//
// A call is made with every argument as a 64-bit word, up to 16 of them: on x64, Windows' and
// the System V conventions pass whole numbers and pointers in the same registers and stack
// places whatever the function declares, and the caller clears the stack, so a function given
// more words than it takes reads those it takes and leaves the rest. The result comes back in
// the same register and is cut to the type the program declared. Floats, which go in other
// registers, are not passed yet.

#ifdef _WIN32
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <dlfcn.h>
#include <errno.h>
#endif

#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "builtins.h"
#include "sandbox.h"
#include "vm.h"

namespace astra {

namespace {

// The types a parameter or a result is declared as, as CType's entries number them.
enum CType : int64_t {
    kInt8, kUint8, kInt16, kUint16, kInt32, kUint32, kInt64, kPointer, kWideString, kUtf8String, kVoid,
};
constexpr size_t kMostArguments = 16;

// The last error the system recorded on this thread, read at once after each call.
thread_local int64_t lastError = 0;

// A block of native memory: where it is and how big; freed when closed, or by the collector.
struct Block {
    uint8_t* data = nullptr;
    size_t size = 0;
};

void releaseBlock(int64_t id) {
    Block* b = reinterpret_cast<Block*>(id);
    std::free(b->data);
    delete b;
}

void refuseSandbox(VM& vm, const char* what) {
    if (vm.currentHost().isSandbox())
        throw vm.runtimeError(Thrown::UnsupportedOperationException, std::string("a sandbox does not ") + what + ": it reaches past the sandbox");
}

Block& blockOf(VM& vm, const Value& v) {
    if (!v.isObjType(ObjType::Resource) || std::strcmp(static_cast<ObjResource*>(v.as.obj)->kind, "memory") != 0)
        throw vm.runtimeError(Thrown::IllegalArgumentException, "not a block of native memory");
    return *reinterpret_cast<Block*>(static_cast<ObjResource*>(v.as.obj)->id);
}

// Where `size` bytes at `at` are in the block, checked: an IndexOutOfBoundsException past its
// end, an IllegalStateException once it is closed.
uint8_t* placeIn(VM& vm, Block& b, int64_t at, int64_t size) {
    if (b.data == nullptr) throw vm.runtimeError(Thrown::IllegalStateException, "the memory has been closed");
    if (at < 0 || size < 0 || static_cast<uint64_t>(at) > b.size || b.size - static_cast<size_t>(at) < static_cast<size_t>(size))
        throw vm.runtimeError(Thrown::IndexOutOfBoundsException, std::to_string(size) + " bytes at " + std::to_string(at) + " run past the end of " +
                                                                     std::to_string(b.size) + " bytes of memory");
    return b.data + at;
}

size_t sizeOfKind(VM& vm, int64_t kind) {
    switch (kind) {
        case kInt8: case kUint8: return 1;
        case kInt16: case kUint16: return 2;
        case kInt32: case kUint32: return 4;
        case kInt64: case kPointer: return 8;
        default: throw vm.runtimeError(Thrown::IllegalArgumentException, "memory holds numbers and pointers, not text, at a place");
    }
}

// A word as the declared type gives it: cut to its size, signed or not.
int64_t asDeclared(uint64_t word, int64_t kind) {
    switch (kind) {
        case kInt8: return static_cast<int8_t>(word);
        case kUint8: return static_cast<uint8_t>(word);
        case kInt16: return static_cast<int16_t>(word);
        case kUint16: return static_cast<uint16_t>(word);
        case kInt32: return static_cast<int32_t>(word);
        case kUint32: return static_cast<uint32_t>(word);
        default: return static_cast<int64_t>(word);
    }
}

std::string systemError() {
#ifdef _WIN32
    const DWORD code = GetLastError();
    wchar_t* text = nullptr;
    FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr, code, 0,
                   reinterpret_cast<LPWSTR>(&text), 0, nullptr);
    std::string out;
    if (text != nullptr) {
        const int size = WideCharToMultiByte(CP_UTF8, 0, text, -1, nullptr, 0, nullptr, nullptr);
        out.resize(static_cast<size_t>(size > 0 ? size - 1 : 0));
        if (size > 0) WideCharToMultiByte(CP_UTF8, 0, text, -1, out.data(), size, nullptr, nullptr);
        LocalFree(text);
    }
    while (!out.empty() && (out.back() == '\n' || out.back() == '\r' || out.back() == '.')) out.pop_back();
    return out.empty() ? "error " + std::to_string(code) : out;
#else
    const char* e = dlerror();
    return e != nullptr ? e : "not found";
#endif
}

using Word = uint64_t;
using Call16 = Word (*)(Word, Word, Word, Word, Word, Word, Word, Word, Word, Word, Word, Word, Word, Word, Word, Word);

}  // namespace

void registerForeign(VM& vm) {
    ObjClass* holder = vm.alloc<ObjClass>(vm.intern("Foreign"));
    auto def = [&](const char* name, int minArgs, int maxArgs, NativeFn fn) {
        holder->statics.set(vm.intern(name), Value::object(vm.alloc<ObjNative>(std::string("Foreign.") + name, minArgs, maxArgs, fn)));
    };

    // load(name): the library's handle, as a number; it stays loaded while the program runs.
    def("load", 1, 1, [](VM& vm, int, Value* args) -> Value {
        refuseSandbox(vm, "load libraries");
        const std::string& name = vm.toStr(args[0], "library name");
#ifdef _WIN32
        const int size = MultiByteToWideChar(CP_UTF8, 0, name.data(), static_cast<int>(name.size()), nullptr, 0);
        std::wstring wide(static_cast<size_t>(size), L'\0');
        MultiByteToWideChar(CP_UTF8, 0, name.data(), static_cast<int>(name.size()), wide.data(), size);
        HMODULE module = LoadLibraryW(wide.c_str());
        if (module == nullptr) throw vm.errorOf("IOException", "cannot load the library '" + name + "': " + systemError());
        return Value::integer(reinterpret_cast<int64_t>(module));
#else
        void* module = nullptr;
        std::string tried;
        for (const std::string& candidate : {name, "lib" + name + ".so", name + ".so", "lib" + name + ".so.6"}) {
            module = dlopen(candidate.c_str(), RTLD_NOW);
            if (module != nullptr) break;
            tried = systemError();
        }
        if (module == nullptr) throw vm.errorOf("IOException", "cannot load the library '" + name + "': " + tried);
        return Value::integer(reinterpret_cast<int64_t>(module));
#endif
    });
    // find(library, name): the function's address, or 0 where the library has none of that name.
    def("find", 2, 2, [](VM& vm, int, Value* args) -> Value {
        const int64_t module = vm.toInt(args[0], "library");
        const std::string& name = vm.toStr(args[1], "function name");
#ifdef _WIN32
        return Value::integer(reinterpret_cast<int64_t>(GetProcAddress(reinterpret_cast<HMODULE>(module), name.c_str())));
#else
        return Value::integer(reinterpret_cast<int64_t>(dlsym(reinterpret_cast<void*>(module), name.c_str())));
#endif
    });
    // call(address, arguments, kinds, result, name): the function called with each argument as
    // its kind says (a number for a number or a pointer, a string for text), its result cut to
    // `result`. Other threads run meanwhile.
    def("call", 5, 5, [](VM& vm, int, Value* args) -> Value {
        refuseSandbox(vm, "call foreign functions");
        const int64_t address = vm.toInt(args[0], "function");
        if (!args[1].isObjType(ObjType::List) || !args[2].isObjType(ObjType::List))
            throw vm.runtimeError(Thrown::IllegalArgumentException, "a foreign call takes its arguments and their kinds as lists");
        const std::vector<Value> given = vm.listCopy(asList(args[1]));
        const std::vector<Value> kinds = vm.listCopy(asList(args[2]));
        const int64_t result = vm.toInt(args[3], "result kind");
        const std::string& name = vm.toStr(args[4], "function name");
        if (given.size() != kinds.size())
            throw vm.runtimeError(Thrown::IllegalArgumentException, name + "() takes " + std::to_string(kinds.size()) + " arguments, not " + std::to_string(given.size()));
        if (given.size() > kMostArguments)
            throw vm.runtimeError(Thrown::IllegalArgumentException, "a foreign function takes at most " + std::to_string(kMostArguments) + " arguments");
        Word words[kMostArguments] = {};
        // Text given for a parameter of text: kept here, in the system's form, while the call runs.
        std::vector<std::wstring> wide;
        std::vector<std::string> narrow;
        wide.reserve(given.size());
        narrow.reserve(given.size());
        for (size_t i = 0; i < given.size(); i++) {
            const int64_t kind = vm.toInt(kinds[i], "kind");
            const Value& a = given[i];
            if (kind == kWideString || kind == kUtf8String) {
                if (a.isNull()) continue;
                if (!a.isString()) throw vm.runtimeError(Thrown::IllegalArgumentException, "argument " + std::to_string(i + 1) + " of " + name + "() is text, not a value of type " + vm.typeName(a));
                const std::string& utf8 = asString(a)->utf8();
                if (kind == kUtf8String) {
                    narrow.push_back(utf8);
                    words[i] = reinterpret_cast<Word>(narrow.back().c_str());
                } else {
#ifdef _WIN32
                    const int size = MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), nullptr, 0);
                    std::wstring w(static_cast<size_t>(size), L'\0');
                    MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), w.data(), size);
                    wide.push_back(std::move(w));
#else
                    std::wstring w;
                    const Characters chars = Characters::fromUtf8(utf8);
                    for (size_t k = 0; k < chars.size(); k++) w.push_back(static_cast<wchar_t>(chars[k]));
                    wide.push_back(std::move(w));
#endif
                    words[i] = reinterpret_cast<Word>(wide.back().c_str());
                }
            } else if (a.isNull()) {
                words[i] = 0;
            } else {
                words[i] = static_cast<Word>(vm.toInt(a, "argument"));
            }
        }
        const Call16 fn = reinterpret_cast<Call16>(address);
        Word got;
        {
            LockReleased waiting(vm);
#ifdef _WIN32
            SetLastError(0);
#else
            errno = 0;
#endif
            got = fn(words[0], words[1], words[2], words[3], words[4], words[5], words[6], words[7], words[8], words[9], words[10], words[11], words[12],
                     words[13], words[14], words[15]);
#ifdef _WIN32
            lastError = static_cast<int64_t>(GetLastError());
#else
            lastError = errno;
#endif
        }
        if (result == kVoid) return Value::null();
        return Value::integer(asDeclared(got, result));
    });
    def("lastError", 0, 0, [](VM&, int, Value*) -> Value { return Value::integer(lastError); });

    // ---- Blocks of native memory ----

    // allocate(size): a block of that many bytes, all zero.
    def("allocate", 1, 1, [](VM& vm, int, Value* args) -> Value {
        refuseSandbox(vm, "give out native memory");
        const int64_t size = vm.toInt(args[0], "size");
        if (size < 0) throw vm.runtimeError(Thrown::IllegalArgumentException, "a block of memory cannot have " + std::to_string(size) + " bytes");
        vm.checkRoomFor(static_cast<uint64_t>(size));
        auto* b = new Block;
        b->size = static_cast<size_t>(size);
        b->data = static_cast<uint8_t*>(std::calloc(b->size > 0 ? b->size : 1, 1));
        if (b->data == nullptr) {
            delete b;
            throw vm.outOfMemory();
        }
        // Counted as the program's memory, so that blocks nobody closed bring a collection round.
        vm.account(b->size);
        return Value::object(vm.alloc<ObjResource>("memory", reinterpret_cast<int64_t>(b), &releaseBlock));
    });
    def("free", 1, 1, [](VM& vm, int, Value* args) -> Value {
        Block& b = blockOf(vm, args[0]);
        std::free(b.data);
        b.data = nullptr;
        return Value::null();
    });
    def("isOpen", 1, 1, [](VM& vm, int, Value* args) -> Value { return Value::boolean(blockOf(vm, args[0]).data != nullptr); });
    def("size", 1, 1, [](VM& vm, int, Value* args) -> Value { return Value::integer(static_cast<int64_t>(blockOf(vm, args[0]).size)); });
    def("address", 1, 1, [](VM& vm, int, Value* args) -> Value {
        Block& b = blockOf(vm, args[0]);
        if (b.data == nullptr) throw vm.runtimeError(Thrown::IllegalStateException, "the memory has been closed");
        return Value::integer(reinterpret_cast<int64_t>(b.data));
    });
    // read(block, at, kind): the number at a place, as its kind says.
    def("read", 3, 3, [](VM& vm, int, Value* args) -> Value {
        Block& b = blockOf(vm, args[0]);
        const int64_t kind = vm.toInt(args[2], "kind");
        const size_t size = sizeOfKind(vm, kind);
        const uint8_t* at = placeIn(vm, b, vm.toInt(args[1], "place"), static_cast<int64_t>(size));
        uint64_t word = 0;
        std::memcpy(&word, at, size);  // the machine's own order: x64 puts the low byte first
        return Value::integer(asDeclared(word, kind));
    });
    // write(block, at, kind, value): the number at a place; an ArithmeticException where it does
    // not fit the kind.
    def("write", 4, 4, [](VM& vm, int, Value* args) -> Value {
        Block& b = blockOf(vm, args[0]);
        const int64_t kind = vm.toInt(args[2], "kind");
        const size_t size = sizeOfKind(vm, kind);
        uint8_t* at = placeIn(vm, b, vm.toInt(args[1], "place"), static_cast<int64_t>(size));
        const int64_t n = vm.toInt(args[3], "number");
        if (size < 8) {
            const bool sign = kind == kInt8 || kind == kInt16 || kind == kInt32;
            const int bits = static_cast<int>(size * 8);
            const int64_t low = sign ? -(int64_t{1} << (bits - 1)) : 0;
            const int64_t high = sign ? (int64_t{1} << (bits - 1)) - 1 : (int64_t{1} << bits) - 1;
            if (n < low || n > high)
                throw vm.runtimeError(Thrown::ArithmeticException, std::to_string(n) + " does not fit in " + std::to_string(bits) + " bits " + (sign ? "signed" : "unsigned"));
        }
        const uint64_t word = static_cast<uint64_t>(n);
        std::memcpy(at, &word, size);
        return Value::null();
    });
    // bytes(block, at, count): a copy of that many bytes, as Bytes.
    def("bytes", 3, 3, [](VM& vm, int, Value* args) -> Value {
        Block& b = blockOf(vm, args[0]);
        const int64_t count = vm.toInt(args[2], "count");
        const uint8_t* at = placeIn(vm, b, vm.toInt(args[1], "place"), count);
        ObjBytes* made = vm.alloc<ObjBytes>();
        made->data.assign(at, at + count);
        vm.account(made->data.size());
        return Value::object(made);
    });
    // putBytes(block, at, bytes)
    def("putBytes", 3, 3, [](VM& vm, int, Value* args) -> Value {
        Block& b = blockOf(vm, args[0]);
        if (!args[2].isObjType(ObjType::Bytes)) throw vm.runtimeError(Thrown::IllegalArgumentException, "putBytes takes Bytes");
        const std::vector<uint8_t>& from = asBytes(args[2])->data;
        uint8_t* at = placeIn(vm, b, vm.toInt(args[1], "place"), static_cast<int64_t>(from.size()));
        if (!from.empty()) std::memcpy(at, from.data(), from.size());
        return Value::null();
    });
    // string16(block, at, most): the UTF-16 text there, up to a 0 or `most` characters (-1 for
    // none), within the block; string8 the same for UTF-8.
    def("string16", 3, 3, [](VM& vm, int, Value* args) -> Value {
        Block& b = blockOf(vm, args[0]);
        const int64_t at = vm.toInt(args[1], "place");
        int64_t most = vm.toInt(args[2], "length");
        placeIn(vm, b, at, 0);
        const int64_t room = static_cast<int64_t>(b.size - static_cast<size_t>(at)) / 2;
        if (most < 0 || most > room) most = room;
        std::u16string units;
        for (int64_t i = 0; i < most; i++) {
            uint16_t u;
            std::memcpy(&u, b.data + at + 2 * i, 2);
            if (u == 0 && vm.toInt(args[2], "length") < 0) break;
            units.push_back(static_cast<char16_t>(u));
        }
        std::string utf8;
        for (size_t i = 0; i < units.size(); i++) {
            uint32_t c = units[i];
            if (c >= 0xD800 && c < 0xDC00 && i + 1 < units.size() && units[i + 1] >= 0xDC00 && units[i + 1] < 0xE000) {
                c = 0x10000 + ((c - 0xD800) << 10) + (static_cast<uint32_t>(units[i + 1]) - 0xDC00);
                i++;
            }
            appendUtf8(utf8, c);
        }
        return vm.str(std::move(utf8));
    });
    def("string8", 3, 3, [](VM& vm, int, Value* args) -> Value {
        Block& b = blockOf(vm, args[0]);
        const int64_t at = vm.toInt(args[1], "place");
        int64_t most = vm.toInt(args[2], "length");
        placeIn(vm, b, at, 0);
        const int64_t room = static_cast<int64_t>(b.size - static_cast<size_t>(at));
        const bool toZero = most < 0;
        if (most < 0 || most > room) most = room;
        const char* from = reinterpret_cast<const char*>(b.data + at);
        size_t length = static_cast<size_t>(most);
        if (toZero) length = strnlen(from, static_cast<size_t>(most));
        return vm.str(std::string(from, length));
    });
    // putString16(block, at, text): the text as UTF-16 with a 0 after it; putString8 as UTF-8.
    def("putString16", 3, 3, [](VM& vm, int, Value* args) -> Value {
        Block& b = blockOf(vm, args[0]);
        const std::string& text = vm.toStr(args[2], "text");
        std::vector<uint16_t> units;
        const Characters chars = Characters::fromUtf8(text);
        for (size_t i = 0; i < chars.size(); i++) {
            const uint32_t c = chars[i];
            if (c >= 0x10000) {
                units.push_back(static_cast<uint16_t>(0xD800 + ((c - 0x10000) >> 10)));
                units.push_back(static_cast<uint16_t>(0xDC00 + ((c - 0x10000) & 0x3FF)));
            } else {
                units.push_back(static_cast<uint16_t>(c));
            }
        }
        units.push_back(0);
        uint8_t* at = placeIn(vm, b, vm.toInt(args[1], "place"), static_cast<int64_t>(units.size() * 2));
        std::memcpy(at, units.data(), units.size() * 2);
        return Value::null();
    });
    def("putString8", 3, 3, [](VM& vm, int, Value* args) -> Value {
        Block& b = blockOf(vm, args[0]);
        const std::string& text = vm.toStr(args[2], "text");
        uint8_t* at = placeIn(vm, b, vm.toInt(args[1], "place"), static_cast<int64_t>(text.size() + 1));
        std::memcpy(at, text.c_str(), text.size() + 1);
        return Value::null();
    });

    vm.defineGlobal("__foreign", Value::object(holder));
}

}  // namespace astra
