// The ww executable's start (M6.5 part 5): a program built into the executable runs, and
// otherwise this is the ww command. In no section a built program leaves out, and not in
// the build's proofs, which have a start of their own (proof_main.cpp): it names the command,
// and the scripts a program compiles as it runs, both on the compiler side.

#include "run.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <cstdio>
#include <cstdlib>

namespace {

// WW_CRASH_STACK=1: a crash (an exception nothing handles) writes to standard error the calls
// the crashing thread was in, as addresses a debugger names (cdb's ln, in any process of the
// same executable until the machine restarts), for a crash that a debugger attached beforehand
// would change the timing of and so never see (2026-11-19, o-native-results-crash).
LONG WINAPI writeCrashStack(EXCEPTION_POINTERS* info) {
    std::fprintf(stderr, "ww: crashed: exception %08lX at %p (thread %lu), the calls it was in:\n", info->ExceptionRecord->ExceptionCode,
                 info->ExceptionRecord->ExceptionAddress, GetCurrentThreadId());
    CONTEXT context = *info->ContextRecord;
    for (int i = 0; i < 48 && context.Rip != 0; i++) {
        std::fprintf(stderr, "  %p\n", reinterpret_cast<void*>(context.Rip));
        DWORD64 imageBase = 0;
        PRUNTIME_FUNCTION function = RtlLookupFunctionEntry(context.Rip, &imageBase, nullptr);
        if (function == nullptr) {
            context.Rip = *reinterpret_cast<DWORD64*>(context.Rsp);
            context.Rsp += 8;
            continue;
        }
        void* handlerData = nullptr;
        DWORD64 establisher = 0;
        RtlVirtualUnwind(UNW_FLAG_NHANDLER, imageBase, context.Rip, function, &context, &handlerData, &establisher, nullptr);
    }
    std::fflush(stderr);
    return EXCEPTION_CONTINUE_SEARCH;
}

}  // namespace
#endif

int main(int argc, char** argv) {
#ifdef _WIN32
    if (const char* asked = std::getenv("WW_CRASH_STACK"); asked != nullptr && *asked != 0 && *asked != '0') SetUnhandledExceptionFilter(writeCrashStack);
#endif
    ww::registerScripts = ww::registerScriptsHere;  // called only where that side is in the file
    return ww::startExecutable(argc, argv, ww::wwCommandMain);
}
