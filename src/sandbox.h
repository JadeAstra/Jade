#pragma once

#include <cstdint>
#include <memory>
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
};

std::shared_ptr<SandboxHost> makeSandbox(bool windowsPaths, int64_t diskSize);

// __sandbox, what Sandbox in the standard library (sandbox.astra) is built on.
void registerSandbox(VM& vm);

}  // namespace astra
