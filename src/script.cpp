// Scripts compiled and run as a program runs (docs/scripting-plan.html, parts 1 and 2): the
// built-in __script, which src/std/script.ww is written on.
//
// A script is source text compiled while the program runs, into a program of its own: the
// standard library and the script, as a worker's is. It runs in a VM of its own, on a thread
// of its own, and shares nothing with the program. Compiling needs the compiler, so this file
// is on the compiler side: a program built into an executable keeps that side where it uses
// Script (D161), and nowhere else.
//
// The two talk by requests: the program calls the script's functions by name, asks for the
// object the script gives (its contract's interface), and calls the objects and functions the
// script has handed it; the script calls the program's `api` and the objects and functions
// handed to it. Each request goes to the side that owns what it calls, which serves it on its
// own thread: one waiting for an answer of its own serves what the other side asks meanwhile
// (a callback in the middle of a call), and one doing nothing hears of it on its event loop.
// What passes is copied as a worker's messages are (worker.cpp), frozen data shared; an object
// of an ordinary class, or a function, goes as a handle (HandleWays), which the other side
// makes into an object of the contract interface it implements (its __ScriptProxy_ class),
// or a function, each call of which comes back here.

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <mutex>
#include <sstream>
#include <thread>
#include <unordered_map>

#include "builtins.h"
#include "bytecode.h"
#include "cache.h"
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
    std::vector<int> modules;                    // to run, in order: the contract's, the script's
    std::unordered_map<std::string, int> names;  // what it declares at its top level: global slots
    bool gives = false;                          // it gives its contract's interface
};

enum Side : int { kProgram = 0, kScript = 1 };

// One thing one side asks of the other.
struct Request {
    enum Kind : uint8_t { TopLevel, Method, Property, Function, Gives } kind = TopLevel;
    int64_t target = 0;  // a handle's id, on the side that serves it
    std::string member;  // a function's or a member's name
    std::shared_ptr<Message> arguments;
    std::shared_ptr<Message> result;
    std::string failure;  // what was thrown, as text
    bool done = false;
};

// A script started. Everything in it is guarded by the mutex, so that either side may go away
// while the other still holds it.
struct ScriptRun {
    std::mutex mutex;
    std::condition_variable changed;
    std::shared_ptr<const CompiledScript> compiled;
    VM* sides[2] = {nullptr, nullptr};  // the program's VM, and the script's own while it runs
    bool ready = false;                 // its top level has run: it takes calls
    bool ended = false;
    std::string failure;  // why it ended, where it failed
    std::deque<std::shared_ptr<Request>> asked[2];  // what each side has to do for the other
    std::shared_ptr<Message> api;                   // the program's object for the script's `api`
};

// A handle: what one side's object or function stands for on the other side.
struct Handle {
    std::weak_ptr<ScriptRun> run;
    Side owner;
    int64_t id;
};

// The run a script's VM runs, for the script's own requests.
std::mutex runsMutex;
std::unordered_map<const VM*, std::weak_ptr<ScriptRun>> runOfScript;

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

template <class T>
T* resourceOf(VM& vm, Value v, const char* kind, const char* what) {
    if (!v.isObjType(ObjType::Resource) || std::strcmp(static_cast<ObjResource*>(v.as.obj)->kind, kind) != 0)
        throw vm.runtimeError(Thrown::IllegalArgumentException, std::string("not ") + what);
    return reinterpret_cast<T*>(static_cast<ObjResource*>(v.as.obj)->id);
}

std::shared_ptr<const CompiledScript>& compiledOf(VM& vm, Value v) {
    return *resourceOf<std::shared_ptr<const CompiledScript>>(vm, v, "script", "a compiled script");
}

std::shared_ptr<ScriptRun>& runOf(VM& vm, Value v) { return *resourceOf<std::shared_ptr<ScriptRun>>(vm, v, "running script", "a running script"); }

void releaseCompiled(int64_t id) { delete reinterpret_cast<std::shared_ptr<const CompiledScript>*>(id); }
void releaseHandle(int64_t id) { delete reinterpret_cast<Handle*>(id); }

// Stops the script, whatever it is doing.
void stopRun(ScriptRun& run) {
    std::lock_guard<std::mutex> hold(run.mutex);
    if (run.sides[kScript] != nullptr) run.sides[kScript]->requestStop();
}

// A running script the program no longer reaches: stopped, as nothing can call it any more.
void releaseRun(int64_t id) {
    auto* run = reinterpret_cast<std::shared_ptr<ScriptRun>*>(id);
    stopRun(**run);
    delete run;
}

// The handles a side has given in a run, let go of as the run ends there.
void forgetHandles(VM& vm, const ScriptRun* run) {
    for (auto it = vm.scriptHandles.begin(); it != vm.scriptHandles.end();)
        it = it->second.run == run ? vm.scriptHandles.erase(it) : std::next(it);
    vm.scriptHandleIds.erase(run);
}

// ---- Handles ----

// What one side's messages do with objects and functions (worker.cpp's HandleWays), in a run.
struct Ways final : HandleWays {
    std::shared_ptr<ScriptRun> run;
    Side me;
    HandleWays* outer;
    Ways(std::shared_ptr<ScriptRun> r, Side side) : run(std::move(r)), me(side), outer(handleWays) { handleWays = this; }
    ~Ways() override { handleWays = outer; }
    Ways(const Ways&) = delete;
    Ways& operator=(const Ways&) = delete;

    bool give(VM& vm, Value v, int64_t& id, bool& home, bool& function, std::vector<std::string>& interfaces) override {
        if (!v.isObj()) return false;
        // What stands here for the other side's own goes home as that.
        if (const ObjClass* cls = classOf(v); cls != nullptr && v.isObjType(ObjType::Instance) && cls->name->utf8().rfind("__ScriptProxy_", 0) == 0) {
            Value inner;
            if (asInstance(v)->read(vm.intern("__handle"), inner) && inner.isObjType(ObjType::Resource) &&
                std::strcmp(static_cast<ObjResource*>(inner.as.obj)->kind, "script handle") == 0) {
                const Handle& h = *reinterpret_cast<Handle*>(static_cast<ObjResource*>(inner.as.obj)->id);
                if (h.run.lock() == run && h.owner != me) {
                    id = h.id;
                    home = true;
                    return true;
                }
            }
        }
        if (v.isObjType(ObjType::BoundNative) && asBoundNative(v)->receiver.isObjType(ObjType::Resource) &&
            std::strcmp(static_cast<ObjResource*>(asBoundNative(v)->receiver.as.obj)->kind, "script handle") == 0) {
            const Handle& h = *reinterpret_cast<Handle*>(static_cast<ObjResource*>(asBoundNative(v)->receiver.as.obj)->id);
            if (h.run.lock() == run && h.owner != me) {
                id = h.id;
                home = true;
                function = true;
                return true;
            }
        }
        function = v.isObjType(ObjType::Closure) || v.isObjType(ObjType::Native) || v.isObjType(ObjType::BoundMethod) ||
                   v.isObjType(ObjType::BoundNative);
        const ObjClass* cls = classOf(v);
        if (!function) {
            if (!v.isObjType(ObjType::Instance) || cls == nullptr || cls->isEnum || cls->interfaces.empty()) return false;
            for (const ObjClass* face : cls->interfaces) interfaces.push_back(face->name->utf8());
        }
        // The same object, the same handle, for as long as the run lasts.
        auto& ids = vm.scriptHandleIds[run.get()];
        if (auto known = ids.find(v.as.obj); known != ids.end()) {
            id = known->second;
        } else {
            id = vm.nextScriptHandle++;
            ids[v.as.obj] = id;
            vm.scriptHandles[id] = VM::ScriptHandle{v, run.get()};
        }
        return true;
    }

    Value take(VM& vm, int64_t id, bool home, bool function, const std::vector<std::string>& interfaces) override {
        if (home) {
            auto found = vm.scriptHandles.find(id);
            if (found == vm.scriptHandles.end()) throw vm.runtimeError(Thrown::IllegalStateException, "a handle came back for something no longer held");
            return found->second.value;
        }
        auto* handle = new Handle{run, me == kProgram ? kScript : kProgram, id};
        const Value resource = Value::object(vm.alloc<ObjResource>("script handle", reinterpret_cast<int64_t>(handle), &releaseHandle));
        TempRoot keep(vm, resource);
        if (function) {
            static const NativeMethod forward{"a script's function", 0, 255, [](VM& vm, Value self, int argc, Value* args) -> Value {
                                                  return forwardCall(vm, self, Request::Function, "", argc, args);
                                              }};
            return Value::object(vm.alloc<ObjBoundNative>(resource, &forward));
        }
        for (const std::string& face : interfaces)
            if (ObjClass* proxy = vm.classNamed("__ScriptProxy_" + face)) {
                Value self = resource;
                return vm.call(Value::object(proxy), 1, &self);
            }
        std::string names;
        for (const std::string& face : interfaces) names += (names.empty() ? "" : ", ") + face;
        throw vm.runtimeError(Thrown::IllegalArgumentException,
                              "an object was handed over that implements no interface of the script's contract (" + (names.empty() ? std::string("none") : names) +
                                  "): what is not data goes as an object of a contract's interface");
    }

    static Value forwardCall(VM& vm, Value handle, Request::Kind kind, const std::string& member, int argc, Value* args);
};

std::shared_ptr<Message> write(VM& vm, const std::shared_ptr<ScriptRun>& run, Side side, Value v) {
    Ways ways(run, side);
    return messageFor(vm, v);
}

Value read(VM& vm, const std::shared_ptr<ScriptRun>& run, Side side, const Message& message) {
    Ways ways(run, side);
    return readMessage(vm, message);
}

// ---- Requests ----

// Does one request, on the side that owns what it calls (`me`), with that VM's lock held.
void serve(VM& vm, const std::shared_ptr<ScriptRun>& run, Side me, const std::shared_ptr<Request>& request) {
    std::shared_ptr<Message> result;
    std::string failure;
    bool stopped = false;
    try {
        const Value arguments = request->arguments != nullptr ? read(vm, run, me, *request->arguments) : Value::null();
        TempRoot keepArguments(vm, arguments);
        std::vector<Value> given;
        if (arguments.isObjType(ObjType::List))
            for (size_t i = 0; i < asList(arguments)->size(); i++) given.push_back(vm.listGet(asList(arguments), i));
        TempVector keepGiven(vm, given);
        Value answered = Value::null();
        Value target = Value::null();
        if (request->kind != Request::TopLevel && request->kind != Request::Gives) {
            auto found = vm.scriptHandles.find(request->target);
            if (found == vm.scriptHandles.end()) throw vm.runtimeError(Thrown::IllegalStateException, "a call came for something no longer held");
            target = found->second.value;
        }
        TempRoot keepTarget(vm, target);
        switch (request->kind) {
            case Request::TopLevel: {
                const auto& names = run->compiled->names;
                const auto found = names.find(request->member);
                const Value function = found == names.end() ? Value::null() : vm.globalAt(found->second);
                if (!function.isObjType(ObjType::Closure)) {
                    failure = "the script has no function named '" + request->member + "'";
                    break;
                }
                answered = vm.call(function, static_cast<int>(given.size()), given.data());
                break;
            }
            case Request::Gives: answered = vm.globalNamed("__scriptGives"); break;
            case Request::Method: {
                const Value method = vm.memberGet(target, vm.intern(request->member));
                TempRoot keepMethod(vm, method);
                answered = vm.call(method, static_cast<int>(given.size()), given.data());
                break;
            }
            case Request::Property: answered = vm.memberGet(target, vm.intern(request->member)); break;
            case Request::Function: answered = vm.call(target, static_cast<int>(given.size()), given.data()); break;
        }
        TempRoot keepAnswer(vm, answered);
        if (failure.empty()) result = write(vm, run, me, answered);
    } catch (WwThrow& thrown) {
        failure = describe(vm, thrown);
    } catch (const WorkerStop&) {
        failure = "the script was stopped";
        stopped = true;
    }
    {
        std::lock_guard<std::mutex> hold(run->mutex);
        request->result = std::move(result);
        request->failure = std::move(failure);
        request->done = true;
    }
    run->changed.notify_all();
    if (stopped) throw WorkerStop{};
}

// Serves, in turn, what the other side has asked of `me`.
void serveAsked(VM& vm, const std::shared_ptr<ScriptRun>& run, Side me) {
    for (;;) {
        std::shared_ptr<Request> next;
        {
            std::lock_guard<std::mutex> hold(run->mutex);
            if (run->asked[me].empty()) return;
            next = run->asked[me].front();
            run->asked[me].pop_front();
        }
        serve(vm, run, me, next);
    }
}

// Asks the other side of `me` to do `request`: queued, and its event loop told, should its thread
// be doing nothing. False where that side is not there to ask.
bool ask(const std::shared_ptr<ScriptRun>& run, Side me, const std::shared_ptr<Request>& request) {
    const Side other = me == kProgram ? kScript : kProgram;
    std::lock_guard<std::mutex> hold(run->mutex);
    VM* there = run->sides[other];
    if (there == nullptr || run->ended) return false;
    run->asked[other].push_back(request);
    there->postNative([run, other](VM& vm) { serveAsked(vm, run, other); });
    run->changed.notify_all();
    return true;
}

// Waits, with the VM's lock given up, until `finished` (read under the run's mutex), the run
// ends, or the other side asks something, which is then served here.
template <class Finished>
void waitServing(VM& vm, const std::shared_ptr<ScriptRun>& run, Side me, Finished finished) {
    for (;;) {
        bool serveNow = false;
        {
            LockReleased released(vm);
            std::unique_lock<std::mutex> hold(run->mutex);
            run->changed.wait(hold, [&] { return finished() || run->ended || !run->asked[me].empty(); });
            if (finished() || (run->ended && run->asked[me].empty())) return;
            serveNow = !run->asked[me].empty();
        }
        if (serveNow) serveAsked(vm, run, me);
    }
}

// A list of two: [true, the answer] or [false, what went wrong, as text].
Value answerPair(VM& vm, bool ok, Value v) {
    TempRoot keepValue(vm, v);
    ObjList* pair = newList(vm);
    TempRoot keep(vm, Value::object(pair));
    vm.listAdd(pair, Value::boolean(ok));
    vm.listAdd(pair, v);
    return Value::object(pair);
}

// Asks the other side and waits for the answer, as a pair.
Value request(VM& vm, const std::shared_ptr<ScriptRun>& run, Side me, const std::shared_ptr<Request>& asked) {
    if (!ask(run, me, asked)) {
        std::string failure;
        {
            std::lock_guard<std::mutex> hold(run->mutex);
            failure = run->failure;
        }
        return answerPair(vm, false, vm.str(std::string(me == kProgram ? "the script is not running" : "the program is not there to ask") +
                                             (failure.empty() ? std::string() : ": " + failure)));
    }
    waitServing(vm, run, me, [&] { return asked->done; });
    std::string failure;
    bool done = false;
    {
        std::lock_guard<std::mutex> hold(run->mutex);
        done = asked->done;
        failure = done ? asked->failure : "the " + std::string(me == kProgram ? "script" : "program") + " ended before it answered" +
                                              (run->failure.empty() ? std::string() : ": " + run->failure);
    }
    if (!failure.empty()) return answerPair(vm, false, vm.str(failure));
    // What the program cannot make again (a data class it has not) fails as the call.
    try {
        return answerPair(vm, true, asked->result != nullptr ? read(vm, run, me, *asked->result) : Value::null());
    } catch (WwThrow& thrown) {
        return answerPair(vm, false, vm.str(describe(vm, thrown)));
    }
}

Value Ways::forwardCall(VM& vm, Value handleValue, Request::Kind kind, const std::string& member, int argc, Value* args) {
    const Handle& handle = *resourceOf<Handle>(vm, handleValue, "script handle", "a script's handle");
    std::shared_ptr<ScriptRun> run = handle.run.lock();
    if (run == nullptr) throw vm.runtimeError(Thrown::IllegalStateException, "a handle of a script that has ended");
    const Side me = handle.owner == kProgram ? kScript : kProgram;
    auto asked = std::make_shared<Request>();
    asked->kind = kind;
    asked->target = handle.id;
    asked->member = member;
    ObjList* list = newList(vm);
    TempRoot keepList(vm, Value::object(list));
    for (int i = 0; i < argc; i++) vm.listAdd(list, args[i]);
    asked->arguments = write(vm, run, me, Value::object(list));
    const Value answer = request(vm, run, me, asked);
    // A function value's call answers itself, or throws where the other side threw.
    if (kind == Request::Function) {
        const ObjList* pair = asList(answer);
        if (!vm.listGet(const_cast<ObjList*>(pair), 0).truthy())
            throw vm.runtimeError(Thrown::IllegalStateException, vm.stringify(vm.listGet(const_cast<ObjList*>(pair), 1)));
        return vm.listGet(const_cast<ObjList*>(pair), 1);
    }
    return answer;
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
    {
        std::lock_guard<std::mutex> hold(runsMutex);
        runOfScript[vm] = run;
    }

    std::string failure;
    bool waiting = false;
    try {
        readProgram(*program, *vm, compiled->image, true);
        vm->load(*program);
        vm->jobStarted();  // it waits for calls until it is stopped
        waiting = true;
        {
            std::lock_guard<std::mutex> hold(run->mutex);
            run->sides[kScript] = vm;
        }
        for (int module : compiled->modules) vm->runModule(module);
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
        run->sides[kScript] = nullptr;
        run->ended = true;
        run->failure = failure.empty() && !run->ready ? std::string("the script ended before its top level had run") : failure;
        run->asked[kScript].clear();
    }
    run->changed.notify_all();
    {
        std::lock_guard<std::mutex> hold(runsMutex);
        runOfScript.erase(vm);
    }
    if (waiting) vm->jobFinished();
    if (!vm->threadsAlive()) {
        delete vm;
        delete program;
    }
    parent->scriptEnded();
}

std::shared_ptr<ScriptRun> runOfThisScript(VM& vm) {
    std::lock_guard<std::mutex> hold(runsMutex);
    auto found = runOfScript.find(&vm);
    return found == runOfScript.end() ? nullptr : found->second.lock();
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

    // compile(source, name, directory, contractName, contractSource, api, gives): the script
    // compiled, or a list of what is wrong with it. Compiled on a thread of its own, with a VM
    // of its own (a VM keeps things for its thread), while this thread lets the program's other
    // threads go on.
    def("compile", 7, 7, [](VM& vm, int, Value* args) -> Value {
        const std::string source = vm.toStr(args[0], "the script's source");
        const std::string name = vm.toStr(args[1], "the script's name");
        const std::string directory = vm.toStr(args[2], "the script's folder");
        const std::string contractName = vm.toStr(args[3], "the contract's name");
        const std::string contractSource = vm.toStr(args[4], "the contract");
        const std::string api = vm.toStr(args[5], "the api's interface");
        const std::string gives = vm.toStr(args[6], "the interface it gives");
        std::shared_ptr<CompiledScript> made;
        std::vector<Problem> problems;
        std::string broke;
        {
            LockReleased released(vm);
            std::thread([&] {
                std::lock_guard<std::mutex> one(compiling);
                try {
                    // With WW_TIMES set, how long each part of a script's compile took, on standard error.
                    using Clock = std::chrono::steady_clock;
                    const auto began = Clock::now();
                    auto since = [](Clock::time_point from) { return std::chrono::duration<double, std::milli>(Clock::now() - from).count(); };
                    VM own(true);
                    const double vmMade = since(began);
                    CompilerSession session(own);
                    const double prelude = since(began) - vmMade;
                    session.libraryPaths = commandLibraryPaths();
                    auto compiled = std::make_shared<CompiledScript>();
                    compiled->name = name;
                    compiled->gives = !gives.empty();
                    compiled->entry = session.compileScript(source, name, directory, compiled->names, contractSource, contractName, api, gives, compiled->modules);
                    const double compiledAt = since(began);
                    std::ostringstream image;
                    writeProgram(session.program, image);
                    // What it kept compiled (a library it imports) is written from this session's
                    // code, so before the session goes.
                    finishKeeping();
                    if (std::getenv("WW_TIMES") != nullptr)
                        std::fprintf(stderr, "ww: script %s compiled in %.1f ms: a VM %.1f, the standard library %.1f, the script %.1f, its image (%zu bytes) and what was kept %.1f\n",
                                     name.c_str(), since(began), vmMade, prelude, compiledAt - vmMade - prelude, image.str().size(), since(began) - compiledAt);
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

    // The names a compiled script declares at its top level, sorted (not those the contract
    // brings, nor what the compiler adds).
    def("names", 1, 1, [](VM& vm, int, Value* args) -> Value {
        const auto& compiled = *compiledOf(vm, args[0]);
        std::vector<std::string> names;
        for (const auto& [name, slot] : compiled.names)
            if (name.rfind("__", 0) != 0 && name != "api") names.push_back(name);
        std::sort(names.begin(), names.end());
        ObjList* list = newList(vm);
        TempRoot keep(vm, Value::object(list));
        for (const std::string& name : names) vm.listAdd(list, vm.str(name));
        return Value::object(list);
    });

    // start(script, api): the script running, its top level run; or, where that failed, what it
    // threw, as text. `api` is what the script's `api` stands for, if anything.
    def("start", 2, 2, [](VM& vm, int, Value* args) -> Value {
        auto run = std::make_shared<ScriptRun>();
        run->compiled = compiledOf(vm, args[0]);
        run->sides[kProgram] = &vm;
        if (!args[1].isNull()) run->api = write(vm, run, kProgram, args[1]);
        vm.scriptStarted();
        std::thread(scriptMain, run, vm.currentHostShared(), &vm).detach();
        // Its top level may call the program's api: served here while this waits.
        waitServing(vm, run, kProgram, [&] { return run->ready; });
        std::string failure;
        {
            std::lock_guard<std::mutex> hold(run->mutex);
            if (!run->ready) failure = run->failure;
        }
        if (!failure.empty()) {
            forgetHandles(vm, run.get());
            return vm.str(failure);
        }
        auto* kept = new std::shared_ptr<ScriptRun>(run);
        return Value::object(vm.alloc<ObjResource>("running script", reinterpret_cast<int64_t>(kept), &releaseRun));
    });

    // call(running, name, arguments): [true, what the function gave] or [false, what it threw,
    // as text].
    def("call", 3, 3, [](VM& vm, int, Value* args) -> Value {
        std::shared_ptr<ScriptRun> run = runOf(vm, args[0]);
        auto asked = std::make_shared<Request>();
        asked->kind = Request::TopLevel;
        asked->member = vm.toStr(args[1], "the function's name");
        asked->arguments = write(vm, run, kProgram, args[2]);
        return request(vm, run, kProgram, asked);
    });

    // gives(running): [true, the script's object of the interface it gives] or [false, why not].
    def("gives", 1, 1, [](VM& vm, int, Value* args) -> Value {
        std::shared_ptr<ScriptRun> run = runOf(vm, args[0]);
        if (!run->compiled->gives) return answerPair(vm, false, vm.str("the script was compiled with no gives = ...: it gives no object"));
        auto asked = std::make_shared<Request>();
        asked->kind = Request::Gives;
        return request(vm, run, kProgram, asked);
    });

    // forward(handle, kind, member, arguments): a call of an object of the other side's, from its
    // proxy here (kind 1 a method, 2 a property): [true, the answer] or [false, what went wrong].
    def("forward", 4, 4, [](VM& vm, int, Value* args) -> Value {
        const Request::Kind kind = vm.toInt(args[1], "kind") == 2 ? Request::Property : Request::Method;
        std::vector<Value> given;
        if (args[3].isObjType(ObjType::List))
            for (size_t i = 0; i < asList(args[3])->size(); i++) given.push_back(vm.listGet(asList(args[3]), i));
        TempVector keep(vm, given);
        return Ways::forwardCall(vm, args[0], kind, vm.toStr(args[2], "the member's name"), static_cast<int>(given.size()), given.data());
    });

    // In a script: the object its `api` stands for, the program's own.
    def("api", 0, 0, [](VM& vm, int, Value*) -> Value {
        std::shared_ptr<ScriptRun> run = runOfThisScript(vm);
        if (run == nullptr || run->api == nullptr) throw vm.runtimeError(Thrown::IllegalStateException, "this script was started with no api object");
        return read(vm, run, kScript, *run->api);
    });

    // Stops the script, whatever it is doing, and waits until it has ended.
    def("stop", 1, 1, [](VM& vm, int, Value* args) -> Value {
        std::shared_ptr<ScriptRun> run = runOf(vm, args[0]);
        stopRun(*run);
        {
            LockReleased released(vm);
            std::unique_lock<std::mutex> hold(run->mutex);
            run->changed.wait(hold, [&] { return run->ended; });
        }
        forgetHandles(vm, run.get());
        return Value::null();
    });

    def("isRunning", 1, 1, [](VM& vm, int, Value* args) -> Value {
        std::shared_ptr<ScriptRun> run = runOf(vm, args[0]);
        std::lock_guard<std::mutex> hold(run->mutex);
        return Value::boolean(!run->ended);
    });

    vm.defineGlobal("__script", Value::object(script));
}

}  // namespace ww
