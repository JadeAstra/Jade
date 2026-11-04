#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <random>
#include <string>
#include <utility>
#include <vector>

#include "host.h"

namespace astra {

class VM;

// A failure a sandbox makes happen (M6.6.2): whenever `operation` ("read", "write", "list",
// "open", "delete", "move", "copy", "mkdir", or "any") is done to `path` ("*" for every path),
// with `error` as what the system would say. A read, a write and a list go `after` bytes or
// names first; the others fail at once.
struct SandboxFault {
    std::string operation;
    std::string path;
    int64_t after = 0;
    std::string error;
};

// A host that keeps everything in memory (sandbox.cpp): what a sandbox's code reaches in place of
// the system.
class SandboxHost : public Host {
public:
    bool isSandbox() const override { return true; }
    virtual void addFault(SandboxFault fault) = 0;
    // What is there now is what changes() compares with.
    virtual void keepStart() = 0;
    // What has been made, changed or deleted since keepStart, each as a kind and a path, by path.
    virtual std::vector<std::pair<std::string, std::string>> changes() = 0;
    // A directory of the real system copied in, at the root under `name`.
    virtual std::error_code copyIn(Host& real, const std::string& from, const std::string& name) = 0;

    int64_t id = 0;  // the number the program's Sandbox knows it by

    // Its clock (step 3), in nanoseconds since it was made: moved by the program's waits (a
    // sleep, a timer's delay) and by Sandbox.advance, never by real time. Its wall clock starts
    // at 2000-01-01 00:00 UTC.
    static constexpr int64_t kWallStart = 946684800LL * 1000000000LL;
    int64_t now() const { return clock_.load(); }
    void advance(int64_t nanoseconds) {
        if (nanoseconds > 0) clock_.fetch_add(nanoseconds);
    }
    void advanceTo(int64_t nanoseconds) {
        int64_t was = clock_.load();
        while (was < nanoseconds && !clock_.compare_exchange_weak(was, nanoseconds)) {}
    }

    // Its randomness: from a fixed seed, the same each run, unless the program seeds it.
    std::mt19937_64 random{1};
    std::mutex randomMutex;

private:
    std::atomic<int64_t> clock_{0};
};

std::shared_ptr<SandboxHost> makeSandbox(bool windowsPaths, int64_t diskSize);

// __sandbox, what Sandbox in the standard library (sandbox.astra) is built on.
void registerSandbox(VM& vm);

}  // namespace astra
