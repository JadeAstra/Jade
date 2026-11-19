// Scripts compiled and run as a program runs (docs/scripting-plan.html, part 1): the built-in
// __script, which src/std/script.ww is written on.
//
// A script is source text compiled while the program runs, into a program of its own: the
// standard library and the script, as a worker's is. It runs in a VM of its own, on a thread
// of its own, and shares nothing with the program; the program calls its functions by name,
// and what passes between them is copied as a worker's messages are (worker.cpp), frozen data
// shared. Compiling needs the compiler, so this file is on the compiler side: a program built
// into an executable keeps that side where it uses Script (D161), and nowhere else.

#include <algorithm>
#include <condition_variable>
#include <cstring>
#include <mutex>
#include <sstream>
#include <thread>
#include <unordered_map>

#include "builtins.h"
#include "bytecode.h"
#include "compiler.h"
#include "run.h"
#include "vm.h"

#include "section_compiler.h"

namespace ww {

std::vector<std::string> commandLibraryPaths();  // command.cpp: where `import Jade` and the like look

namespace {

// A script compiled: its program's image, which each start reads into a VM of its own.
struct CompiledScript {
    std::string name;
    std::string image;
    int entry = -1;
    std::unordered_map<std::string, int> names;  // what it declares at its top level: global slots
};

// One call from the program into a running script.
struct Call {
    std::string function;
    std::shared_ptr<Message> arguments;
    std::shared_ptr<Message> result;
    std::string failure;  // the script's error, as text, where it threw
    bool done = false;
};

// A script started. Everything in it is guarded by the mutex, so that either side may go away
// while the other still holds it.
struct ScriptRun {
    std::mutex mutex;
    std::condition_variable changed;
    std::shared_ptr<const CompiledScript> compiled;
    VM* vm = nullptr;     // the script's own, while it runs
    bool ready = false;   // its top level has run: it takes calls
    bool ended = false;
    std::string failure;  // why it ended, where it failed
};

// One compile at a time, whatever thread asks: the compiler keeps a few things of its own for
// the whole process (the times of its parts, the facts it learns about paths).
std::mutex compiling;

struct Problem {
    std::string file;
    int line = 0;
    int col = 0;
    std::string message;
};

// A problem as CompileError::format writes the ones after the first: "file:line:col: error: what".
Problem problemOf(const std::string& text) {
    Problem p;
    const size_t mark = text.find(": error: ");
    if (mark == std::string::npos) {
        p.message = text;
        return p;
    }
    p.message = text.substr(mark + 9);
    const std::string where = text.substr(0, mark);
    const size_t colAt = where.rfind(':');
    const size_t lineAt = colAt == std::string::npos || colAt == 0 ? std::string::npos : where.rfind(':', colAt - 1);
    if (lineAt == std::string::npos) {
        p.file = where;
        return p;
    }
    p.file = where.substr(0, lineAt);
    p.line = std::atoi(where.substr(lineAt + 1, colAt - lineAt - 1).c_str());
    p.col = std::atoi(where.substr(colAt + 1).c_str());
    return p;
}

// What an error thrown in a VM says, with where it was thrown.
std::string describe(VM& vm, WwThrow& thrown) {
    std::string text;
    try {
        text = thrown.value.isObjType(ObjType::Instance) ? vm.stringify(thrown.value) : "exception: " + vm.stringify(thrown.value);
    } catch (const WwThrow&) {
        text = "exception (its toString() failed)";
    }
    for (const std::string& line : thrown.trace) text += "\n  " + line;
    return text;
}

std::shared_ptr<const CompiledScript>* compiledOf(VM& vm, Value v) {
    if (!v.isObjType(ObjType::Resource) || std::strcmp(static_cast<ObjResource*>(v.as.obj)->kind, "script") != 0)
        throw vm.runtimeError(Thrown::IllegalArgumentException, "not a compiled script");
    return reinterpret_cast<std::shared_ptr<const CompiledScript>*>(static_cast<ObjResource*>(v.as.obj)->id);
}

std::shared_ptr<ScriptRun>* runOf(VM& vm, Value v) {
    if (!v.isObjType(ObjType::Resource) || std::strcmp(static_cast<ObjResource*>(v.as.obj)->kind, "running script") != 0)
        throw vm.runtimeError(Thrown::IllegalArgumentException, "not a running script");
    return reinterpret_cast<std::shared_ptr<ScriptRun>*>(static_cast<ObjResource*>(v.as.obj)->id);
}

void releaseCompiled(int64_t id) { delete reinterpret_cast<std::shared_ptr<const CompiledScript>*>(id); }

// A running script the program no longer reaches: stopped, as nothing can call it any more.
void stopRun(ScriptRun& run) {
    std::lock_guard<std::mutex> hold(run.mutex);
    if (run.vm != nullptr) run.vm->requestStop();
}

void releaseRun(int64_t id) {
    auto* run = reinterpret_cast<std::shared_ptr<ScriptRun>*>(id);
    stopRun(**run);
    delete run;
}

// ---- The script's thread ----

// `parent` counts this thread among its own until the very end: while it does, the program ends
// its process at once rather than taking itself apart under the thread (run.cpp, runProgram).
void scriptMain(std::shared_ptr<ScriptRun> run, std::shared_ptr<Host> host, VM* parent) {
    // Kept on the heap: if threads of its own are still running when it ends, the VM is left as
    // it is rather than taken apart under them (as a worker's is).
    VM* vm = new VM(true);
    Program* program = new Program;
    std::shared_ptr<const CompiledScript> compiled = run->compiled;
    vm->host = std::move(host);  // in the sandbox of the code that started it, if any
    vm->programImage = [compiled] { return compiled->image; };

    std::string failure;
    bool waiting = false;
    try {
        readProgram(*program, *vm, compiled->image, true);
        vm->load(*program);
        vm->jobStarted();  // it waits for calls until it is stopped
        waiting = true;
        {
            std::lock_guard<std::mutex> hold(run->mutex);
            run->vm = vm;
        }
        vm->runModule(compiled->entry);
        {
            std::lock_guard<std::mutex> hold(run->mutex);
            run->ready = true;
        }
        run->changed.notify_all();
        vm->runEventLoop(true);
    } catch (WwThrow& thrown) {
        failure = describe(*vm, thrown);
    } catch (const WwExit&) {
    } catch (const WorkerStop&) {
    } catch (const std::exception& problem) {
        failure = std::string("the script could not start: ") + problem.what();
    }
    {
        std::lock_guard<std::mutex> hold(run->mutex);
        run->vm = nullptr;
        run->ended = true;
        run->failure = failure.empty() && !run->ready ? std::string("the script ended before its top level had run") : failure;
    }
    run->changed.notify_all();
    if (waiting) vm->jobFinished();
    if (!vm->threadsAlive()) {
        delete vm;
        delete program;
    }
    parent->scriptEnded();
}

// Run on the script's loop: one call from the program.
void answer(VM& vm, const std::shared_ptr<ScriptRun>& run, const std::shared_ptr<Call>& call) {
    std::shared_ptr<Message> result;
    std::string failure;
    bool stopped = false;
    try {
        const Value arguments = readMessage(vm, *call->arguments);
        TempRoot keepArguments(vm, arguments);
        const auto found = run->compiled->names.find(call->function);
        const Value function = found == run->compiled->names.end() ? Value::null() : vm.globalAt(found->second);
        if (!function.isObjType(ObjType::Closure)) {
            failure = "the script has no function named '" + call->function + "'";
        } else {
            std::vector<Value> given;
            if (arguments.isObjType(ObjType::List))
                for (size_t i = 0; i < asList(arguments)->size(); i++) given.push_back(vm.listGet(asList(arguments), i));
            TempVector keepGiven(vm, given);
            const Value answered = vm.call(function, static_cast<int>(given.size()), given.data());
            TempRoot keepAnswer(vm, answered);
            result = messageFor(vm, answered);
        }
    } catch (WwThrow& thrown) {
        failure = describe(vm, thrown);
    } catch (const WorkerStop&) {
        failure = "the script was stopped";
        stopped = true;
    }
    {
        std::lock_guard<std::mutex> hold(run->mutex);
        call->result = std::move(result);
        call->failure = std::move(failure);
        call->done = true;
    }
    run->changed.notify_all();
    if (stopped) throw WorkerStop{};
}

// A list of the problems, each [file, line, column, message], as script.ww reads them.
Value problemList(VM& vm, const std::vector<Problem>& problems) {
    ObjList* list = newList(vm);
    TempRoot keep(vm, Value::object(list));
    for (const Problem& p : problems) {
        ObjList* item = newList(vm);
        TempRoot keepItem(vm, Value::object(item));
        vm.listAdd(item, vm.str(p.file));
        vm.listAdd(item, Value::integer(p.line));
        vm.listAdd(item, Value::integer(p.col));
        vm.listAdd(item, vm.str(p.message));
        vm.listAdd(list, Value::object(item));
    }
    return Value::object(list);
}

}  // namespace

void registerScriptsHere(VM& vm) {
    ObjClass* script = vm.alloc<ObjClass>(vm.intern("script"));
    auto def = [&](const char* name, int minArgs, int maxArgs, NativeFn fn) {
        script->statics.set(vm.intern(name), Value::object(vm.alloc<ObjNative>(std::string("script.") + name, minArgs, maxArgs, fn)));
    };

    // compile(source, name, directory): the script compiled, or a list of what is wrong with it.
    // Compiled on a thread of its own, with a VM of its own (a VM keeps things for its thread),
    // while this thread lets the program's other threads go on.
    def("compile", 3, 3, [](VM& vm, int, Value* args) -> Value {
        const std::string source = vm.toStr(args[0], "the script's source");
        const std::string name = vm.toStr(args[1], "the script's name");
        const std::string directory = vm.toStr(args[2], "the script's folder");
        std::shared_ptr<CompiledScript> made;
        std::vector<Problem> problems;
        std::string broke;
        {
            LockReleased released(vm);
            std::thread([&] {
                std::lock_guard<std::mutex> one(compiling);
                try {
                    VM own(true);
                    CompilerSession session(own);
                    session.libraryPaths = commandLibraryPaths();
                    auto compiled = std::make_shared<CompiledScript>();
                    compiled->name = name;
                    compiled->entry = session.compileScript(source, name, directory, compiled->names);
                    std::ostringstream image;
                    writeProgram(session.program, image);
                    compiled->image = image.str();
                    made = std::move(compiled);
                } catch (const CompileError& e) {
                    problems.push_back({e.file, e.line, e.col, e.what()});
                    for (const std::string& more : e.more) problems.push_back(problemOf(more));
                } catch (const std::exception& e) {
                    broke = e.what();
                }
            }).join();
        }
        if (!broke.empty()) throw vm.runtimeError(Thrown::IllegalStateException, "the script could not be compiled: " + broke);
        if (made == nullptr) {
            // In the order of the text, as they are read (the checker finds them in its own order).
            std::stable_sort(problems.begin(), problems.end(), [](const Problem& a, const Problem& b) {
                return a.file != b.file ? a.file < b.file : a.line != b.line ? a.line < b.line : a.col < b.col;
            });
            return problemList(vm, problems);
        }
        auto* kept = new std::shared_ptr<const CompiledScript>(std::move(made));
        return Value::object(vm.alloc<ObjResource>("script", reinterpret_cast<int64_t>(kept), &releaseCompiled));
    });

    // The names a compiled script declares at its top level, sorted.
    def("names", 1, 1, [](VM& vm, int, Value* args) -> Value {
        const auto& compiled = **compiledOf(vm, args[0]);
        std::vector<std::string> names;
        for (const auto& [name, slot] : compiled.names) names.push_back(name);
        std::sort(names.begin(), names.end());
        ObjList* list = newList(vm);
        TempRoot keep(vm, Value::object(list));
        for (const std::string& name : names) vm.listAdd(list, vm.str(name));
        return Value::object(list);
    });

    // start(script): the script running, its top level run; or, where that failed, what it
    // threw, as text.
    def("start", 1, 1, [](VM& vm, int, Value* args) -> Value {
        auto run = std::make_shared<ScriptRun>();
        run->compiled = *compiledOf(vm, args[0]);
        vm.scriptStarted();
        std::thread(scriptMain, run, vm.currentHostShared(), &vm).detach();
        std::string failure;
        {
            LockReleased released(vm);
            std::unique_lock<std::mutex> hold(run->mutex);
            run->changed.wait(hold, [&] { return run->ready || run->ended; });
            if (!run->ready) failure = run->failure;
        }
        if (!failure.empty()) return vm.str(failure);
        auto* kept = new std::shared_ptr<ScriptRun>(run);
        return Value::object(vm.alloc<ObjResource>("running script", reinterpret_cast<int64_t>(kept), &releaseRun));
    });

    // call(running, name, arguments): [true, what the function gave] or [false, what it threw,
    // as text]. The arguments and the answer are copied as a worker's messages are.
    def("call", 3, 3, [](VM& vm, int, Value* args) -> Value {
        std::shared_ptr<ScriptRun> run = *runOf(vm, args[0]);
        auto call = std::make_shared<Call>();
        call->function = vm.toStr(args[1], "the function's name");
        call->arguments = messageFor(vm, args[2]);
        bool running = false;
        {
            std::lock_guard<std::mutex> hold(run->mutex);
            running = run->vm != nullptr && !run->ended;
            if (running) run->vm->postNative([run, call](VM& script) { answer(script, run, call); });
        }
        bool answered = false;
        if (running) {
            LockReleased released(vm);
            std::unique_lock<std::mutex> hold(run->mutex);
            run->changed.wait(hold, [&] { return call->done || run->ended; });
            answered = call->done;
        }
        ObjList* pair = newList(vm);
        TempRoot keep(vm, Value::object(pair));
        if (!answered) {
            vm.listAdd(pair, Value::boolean(false));
            vm.listAdd(pair, vm.str(std::string(running ? "the script ended before it answered" : "the script is not running") +
                                    (run->failure.empty() ? std::string() : ": " + run->failure)));
        } else if (!call->failure.empty()) {
            vm.listAdd(pair, Value::boolean(false));
            vm.listAdd(pair, vm.str(call->failure));
        } else {
            // What the program cannot make again (a data class it has not) fails as the call.
            Value result;
            std::string unread;
            try {
                result = readMessage(vm, *call->result);
            } catch (WwThrow& thrown) {
                unread = describe(vm, thrown);
            }
            TempRoot keepResult(vm, result);
            vm.listAdd(pair, Value::boolean(unread.empty()));
            vm.listAdd(pair, unread.empty() ? result : vm.str(unread));
        }
        return Value::object(pair);
    });

    // Stops the script, whatever it is doing, and waits until it has ended.
    def("stop", 1, 1, [](VM& vm, int, Value* args) -> Value {
        std::shared_ptr<ScriptRun> run = *runOf(vm, args[0]);
        stopRun(*run);
        LockReleased released(vm);
        std::unique_lock<std::mutex> hold(run->mutex);
        run->changed.wait(hold, [&] { return run->ended; });
        return Value::null();
    });

    def("isRunning", 1, 1, [](VM& vm, int, Value* args) -> Value {
        std::shared_ptr<ScriptRun> run = *runOf(vm, args[0]);
        std::lock_guard<std::mutex> hold(run->mutex);
        return Value::boolean(!run->ended);
    });

    vm.defineGlobal("__script", Value::object(script));
}

}  // namespace ww
