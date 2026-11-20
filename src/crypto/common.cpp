// What the processor has, and the helpers every primitive shares.

#include "crypto.h"

#include <atomic>
#include <cstdlib>

#if defined(__x86_64__) || defined(_M_X64)
#ifdef _MSC_VER
#include <intrin.h>
#else
#include <cpuid.h>
#endif
#endif

namespace ww {
namespace crypto {

namespace {

Cpu detect() {
    Cpu found;
#if defined(__x86_64__) || defined(_M_X64)
#ifdef _MSC_VER
    unsigned b = 0, c = 0;
    int regs[4];
    __cpuid(regs, 1);
    c = static_cast<unsigned>(regs[2]);
    __cpuidex(regs, 7, 0);
    b = static_cast<unsigned>(regs[1]);
#else
    unsigned a = 0, b = 0, c = 0, d = 0;
    __cpuid(1, a, b, c, d);
    const unsigned leaf1c = c;
    __cpuid_count(7, 0, a, b, c, d);
    c = leaf1c;
#endif
    found.ssse3 = (c >> 9) & 1;
    found.sse41 = (c >> 19) & 1;
    found.clmul = ((c >> 1) & 1) && found.ssse3;
    found.aes = ((c >> 25) & 1) && found.ssse3;
    found.sha = ((b >> 29) & 1) && found.sse41;
#endif
    (void)found;
    return found;
}

const Cpu& hardware() {
    static const Cpu found = detect();
    return found;
}

std::atomic<bool> forcedPortable{[] {
    const char* setting = std::getenv("WW_CRYPTO_PORTABLE");
    return setting != nullptr && setting[0] == '1';
}()};

}  // namespace

const Cpu& cpu() {
    static const Cpu none;
    return forcedPortable.load(std::memory_order_relaxed) ? none : hardware();
}

void setPortable(bool on) { forcedPortable.store(on); }
bool portable() { return forcedPortable.load(); }

void wipe(void* p, size_t n) {
    volatile uint8_t* bytes = static_cast<volatile uint8_t*>(p);
    while (n-- > 0) *bytes++ = 0;
}

bool equal(const uint8_t* a, const uint8_t* b, size_t n) {
    uint32_t differ = 0;
    for (size_t i = 0; i < n; i++) differ |= uint32_t(a[i] ^ b[i]);
    // (differ - 1) >> 31 is 1 exactly when differ is 0, with no branch on it.
    return ((differ - 1) >> 31) & 1;
}

}  // namespace crypto
}  // namespace ww
