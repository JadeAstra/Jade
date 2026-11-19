// Snippy, a Python-like language, on WinterWolf's VM (docs/snippy-plan.html, part 5.2): a Snippy file (.py or
// .snippy) is made into WinterWolf source as the compiler reads it, by the translator in
// lib/Snippy (written in WinterWolf, run here in a VM of its own), and is compiled from there as
// any file is: run, built, kept and imported in the same ways. Each line of what it is made into
// is the Snippy line it came from, so that errors and tracebacks name the script's own lines.

#include "compiler.h"
#include "vm.h"

#include <filesystem>
#include <memory>
#include <mutex>

namespace ww {

std::vector<std::string> commandLibraryPaths();  // command.cpp
int commandLoadProgram(const std::string& path, VM& vm, CompilerSession*& session, Program*& program,
                       std::unique_ptr<CompilerSession>& sessionOwner, std::unique_ptr<Program>& programOwner);

namespace {

// The translator, loaded once for the process the first time a Snippy file is read: a VM of its
// own, with lib/Snippy/Translator/Translator.ww compiled (or taken from the cache, as any program is).
struct Translator {
    std::unique_ptr<VM> vm;
    std::unique_ptr<CompilerSession> session;
    std::unique_ptr<Program> program;
    Value translate = Value::null();
    std::string failure;  // why it could not be loaded, if it could not
};

std::mutex translating;

Translator& translator() {
    static Translator t = [] {
        Translator made;
        namespace fs = std::filesystem;
        std::string path;
        std::error_code ec;
        for (const std::string& library : commandLibraryPaths())
            if (fs::is_regular_file(fs::path(library) / "Snippy" / "Translator" / "Translator.ww", ec)) {
                path = (fs::path(library) / "Snippy" / "Translator" / "Translator.ww").string();
                break;
            }
        if (path.empty()) {
            made.failure = "Snippy's translator (Snippy/Translator/Translator.ww) is not among the libraries";
            return made;
        }
        made.vm = std::make_unique<VM>(true);
        CompilerSession* session = nullptr;
        Program* program = nullptr;
        if (commandLoadProgram(path, *made.vm, session, program, made.session, made.program) != 0) {
            made.failure = "Snippy's translator could not be compiled";
            return made;
        }
        try {
            made.vm->load(*program);
            made.vm->runModule(program->entry);
            made.translate = made.vm->globalNamed("snippyTranslate");
        } catch (WwThrow&) {
            made.failure = "Snippy's translator could not start";
        }
        return made;
    }();
    return t;
}

}  // namespace

bool isSnippyPath(const std::string& path) {
    const std::string ext = std::filesystem::path(path).extension().string();
    return ext == ".py" || ext == ".snippy";
}

// What `source`, the Snippy file at `path`, is made into; `main` for the program's own file,
// whose module is __main__ and runs as the program starts. A SyntaxError is a CompileError.
std::string translateSnippy(const std::string& path, const std::string& name, const std::string& source, bool main) {
    std::lock_guard<std::mutex> one(translating);
    Translator& t = translator();
    if (!t.failure.empty()) throw CompileError(name, 1, 1, t.failure);
    VM& vm = *t.vm;
    Value args[3] = {vm.str(source), vm.str(path), Value::boolean(main)};
    TempRoot keepSource(vm, args[0]);
    TempRoot keepPath(vm, args[1]);
    std::string made;
    try {
        const Value out = vm.call(t.translate, 3, args);
        made = asString(out)->utf8();
    } catch (WwThrow& thrown) {
        std::string text;
        try {
            text = vm.stringify(thrown.value);
        } catch (const WwThrow&) {
            text = "an error";
        }
        for (const std::string& line : thrown.trace) text += "\n  " + line;
        throw CompileError(name, 1, 1, "Snippy's translator failed: " + text);
    }
    // A syntax error comes back as "\x01line\x01col\x01text".
    if (!made.empty() && made[0] == '\x01') {
        const size_t a = made.find('\x01', 1), b = made.find('\x01', a + 1);
        const int line = std::stoi(made.substr(1, a - 1));
        const int col = std::stoi(made.substr(a + 1, b - a - 1));
        throw CompileError(name, line, col, made.substr(b + 1));
    }
    return made;
}

}  // namespace ww
