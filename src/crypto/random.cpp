// Secure random numbers: what SecureRandom (src/std/securerandom.ww) is built on. They come
// from the system, which keeps a generator seeded from the machine's own sources of chance:
// BCryptGenRandom on Windows, getrandom on Linux (which waits, once, at start-up, until the
// system's generator has been seeded). Nothing is kept here, so there is nothing to seed, share
// between threads or copy into a worker.

#ifdef _WIN32
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <bcrypt.h>
#else
#include <sys/random.h>

#include <cerrno>
#endif

#include <cstdint>
#include <cstring>
#include <string>

#include "../builtins.h"
#include "../vm.h"

namespace ww {

// Fills `size` bytes at `out` from the system's secure generator; false where it fails, which
// it does not on a working system.
bool secureRandomFill(uint8_t* out, size_t size) {
#ifdef _WIN32
    while (size > 0) {
        const ULONG part = static_cast<ULONG>(size > 0x40000000 ? 0x40000000 : size);
        if (!BCRYPT_SUCCESS(BCryptGenRandom(nullptr, out, part, BCRYPT_USE_SYSTEM_PREFERRED_RNG))) return false;
        out += part;
        size -= part;
    }
    return true;
#else
    while (size > 0) {
        const ssize_t got = ::getrandom(out, size > 0x2000000 ? 0x2000000 : size, 0);
        if (got < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        out += got;
        size -= static_cast<size_t>(got);
    }
    return true;
#endif
}

namespace {

uint64_t random64(VM& vm) {
    uint64_t value = 0;
    if (!secureRandomFill(reinterpret_cast<uint8_t*>(&value), sizeof(value)))
        throw vm.runtimeError(Thrown::IllegalStateException, "the system's secure random numbers are not available");
    return value;
}

}  // namespace

void registerSecureRandom(VM& vm) {
    ObjClass* holder = vm.alloc<ObjClass>(vm.intern("secureRandom"));
    auto def = [&](const char* name, int minArgs, int maxArgs, NativeFn fn) {
        holder->statics.set(vm.intern(name), Value::object(vm.alloc<ObjNative>(std::string("secureRandom.") + name, minArgs, maxArgs, fn)));
    };

    // fill(bytes, offset, count): `count` of the bytes from `offset` made random.
    def("fill", 3, 3, [](VM& vm, int, Value* args) -> Value {
        if (!args[0].isObjType(ObjType::Bytes)) throw vm.runtimeError(Thrown::ClassCastException, "SecureRandom fills Bytes, not " + vm.typeName(args[0]));
        ObjBytes* bytes = asBytes(args[0]);
        vm.guardWrite(bytes);
        const int64_t offset = vm.toInt(args[1], "offset");
        const int64_t count = vm.toInt(args[2], "count");
        const int64_t size = static_cast<int64_t>(bytes->data.size());
        if (offset < 0 || count < 0 || offset > size || count > size - offset)
            throw vm.runtimeError(Thrown::IndexOutOfBoundsException, std::to_string(count) + " bytes from " + std::to_string(offset) + " do not fit in bytes of size " + std::to_string(size));
        if (count > 0 && !secureRandomFill(bytes->data.data() + offset, static_cast<size_t>(count)))
            throw vm.runtimeError(Thrown::IllegalStateException, "the system's secure random numbers are not available");
        return args[0];
    });

    // randomInt(low, high): from `low` to `high`, both included, every one as likely: a draw
    // that would favour some is thrown away and drawn again.
    def("randomInt", 2, 2, [](VM& vm, int, Value* args) -> Value {
        const int64_t low = vm.toInt(args[0], "low");
        const int64_t high = vm.toInt(args[1], "high");
        if (low > high)
            throw vm.runtimeError(Thrown::IllegalArgumentException, "randomInt's low (" + std::to_string(low) + ") is above its high (" + std::to_string(high) + ")");
        const uint64_t span = static_cast<uint64_t>(high) - static_cast<uint64_t>(low);  // the count less one
        if (span == UINT64_MAX) return Value::integer(static_cast<int64_t>(random64(vm)));
        const uint64_t count = span + 1;
        // Draws below 2^64 - (2^64 mod count), the largest multiple of `count` there is room
        // for, give every value equally often; those at or past it would favour the low ones.
        const uint64_t lastFair = UINT64_MAX - (UINT64_MAX % count + 1) % count;
        for (;;) {
            const uint64_t draw = random64(vm);
            if (draw <= lastFair) return Value::integer(static_cast<int64_t>(static_cast<uint64_t>(low) + draw % count));
        }
    });

    // random(): from 0.0 up to 1.0, not including 1.0, each of the 2^53 steps as likely.
    def("random", 0, 0, [](VM& vm, int, Value*) -> Value {
        return Value::number(static_cast<double>(random64(vm) >> 11) * (1.0 / 9007199254740992.0));
    });

    vm.defineGlobal("__secureRandom", Value::object(holder));
}

}  // namespace ww
