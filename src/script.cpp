// Scripts compiled and run as a program runs (docs/scripting-plan.html, parts 1 and 2): the
// built-in __script, which src/std/script.ww is written on.
//
// A script runs in a VM other than the program's, on a thread of its own, and shares nothing
// with the program. That VM is a host: either the script's own, made as it starts from the
// image its compile wrote (the standard library and the script), or a ScriptVM, which the
// program makes and gives several scripts, each compiled into it as it runs (by a compiler that
// lives on its thread, as the shell's does), so that they share one copy of the standard library
// and of what they import. Compiling needs the compiler, so this file is on the compiler side: a
// program built into an executable keeps that side where it uses Script (D161).
//
// The two sides talk by requests: the program calls a script's functions by name, asks for the
// object the script gives (its contract's interface), and calls the objects and functions the
// script has handed it; the script calls the program's `api` and the objects and functions
// handed to it. Each request goes to the side that owns what it calls, which serves it on its
// own thread: one waiting for an answer of its own serves what the other side asks meanwhile (a
// callback in the middle of a call), for every script of the host (one script's call may be
// served by calling another's), and one doing nothing hears of it on its event loop. What
// passes is copied as a worker's messages are (worker.cpp), frozen data shared; an object of an
// ordinary class, or a function, goes as a handle (HandleWays), which the other side makes into
// an object of the contract interface it implements (its __ScriptProxy_ class), or a function,
// each call of which comes back here.

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
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

struct ScriptHost;

// A script compiled: what a start runs. For one with a VM of its own, its program's image,
// which the start reads into that VM; for one compiled in a ScriptVM, its modules are that VM's
// already, and it starts once.
struct CompiledScript {
    std::string name;
    std::string image;                           // empty for one compiled in a ScriptVM
    std::weak_ptr<ScriptHost> in;                // the ScriptVM it was compiled in, if any
    std::vector<int> modules;                    // to run, in order: the contract's, the script's
    std::unordered_map<std::string, int> names;  // what it declares at its top level: global slots
    std::unordered_map<std::string, int> contractSlots;  // its contract's classes and proxies
    bool gives = false;                          // it gives its contract's interface
    mutable bool started = false;
};

enum Side : int { kProgram = 0, kScript = 1 };

struct ScriptRun;

// One thing one side asks of the other, for one script.
struct Request {
    enum Kind : uint8_t { TopLevel, Method, Property, Function, Gives } kind = TopLevel;
    std::shared_ptr<ScriptRun> run;
    int64_t target = 0;  // a handle's id, on the side that serves it
    std::string member;  // a function's or a member's name
    std::shared_ptr<Message> arguments;
    std::shared_ptr<Message> result;
    std::string failure;  // what was thrown, as text
    bool done = false;
};

// A VM scripts run in, and its thread. Everything in it is guarded by the mutex, so that either
// side may go away while the other still holds it.
struct ScriptHost {
    std::mutex mutex;
    std::condition_variable changed;
    VM* sides[2] = {nullptr, nullptr};  // the program's VM, and the scripts' own while it runs
    bool shared = false;                // a ScriptVM, which compiles what it runs
    bool up = false;                    // its VM is made and loaded
    bool ended = false;
    std::string failure;  // why it ended, where it failed
    std::deque<std::shared_ptr<Request>> asked[2];  // what each side has to do for the other
    CompilerSession* session = nullptr;             // a ScriptVM's compiler, on its own thread
};

// A script started, in a host.
struct ScriptRun {
    std::shared_ptr<ScriptHost> host;
    std::shared_ptr<const CompiledScript> compiled;
    bool ready = false;   // its top level has run: it takes calls
    bool ended = false;   // it, or its host, has ended (a ScriptVM's: it was stopped alone)
    std::string failure;  // why it ended, where it failed
    std::shared_ptr<Message> api;  // the program's object for the script's `api`
};

// A handle: what one side's object or function stands for on the other side.
struct Handle {
    std::weak_ptr<ScriptRun> run;
    Side owner;
    int64_t id;
};

// The script a host's VM is starting, for its `api` (__script.api), read as its top level runs.
std::mutex runsMutex;
std::unordered_map<const VM*, std::weak_ptr<ScriptRun>> starting;

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

void addProblems(const CompileError& e, std::vector<Problem>& problems) {
    problems.push_back({e.file, e.line, e.col, e.what()});
    for (const std::string& more : e.more) problems.push_back(problemOf(more));
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
std::shared_ptr<ScriptHost>& hostOf(VM& vm, Value v) { return *resourceOf<std::shared_ptr<ScriptHost>>(vm, v, "script vm", "a ScriptVM"); }

void releaseCompiled(int64_t id) { delete reinterpret_cast<std::shared_ptr<const CompiledScript>*>(id); }
void releaseHandle(int64_t id) { delete reinterpret_cast<Handle*>(id); }

// Stops a host's VM, whatever it is doing.
void stopHost(ScriptHost& host) {
    std::lock_guard<std::mutex> hold(host.mutex);
    if (host.sides[kScript] != nullptr) host.sides[kScript]->requestStop();
}

// A ScriptVM the program no longer reaches: stopped, as nothing can give it a script any more
// (those it runs hold it while the program holds them).
void releaseHost(int64_t id) {
    auto* host = reinterpret_cast<std::shared_ptr<ScriptHost>*>(id);
    stopHost(**host);
    delete host;
}

// A running script the program no longer reaches: stopped, as nothing can call it any more;
// in a ScriptVM, it alone, which ends it there (the VM going on for the others).
void releaseRun(int64_t id) {
    auto* run = reinterpret_cast<std::shared_ptr<ScriptRun>*>(id);
    ScriptHost& host = *(*run)->host;
    if (host.shared) {
        std::lock_guard<std::mutex> hold(host.mutex);
        (*run)->ended = true;
    } else {
        stopHost(host);
    }
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

    // A script's own (its contract's), in a VM where each script has its own.
    ObjClass* classNamed(VM& vm, const std::string& name) override {
        if (me == kScript) {
            const auto& slots = run->compiled->contractSlots;
            if (auto found = slots.find(name); found != slots.end()) {
                const Value v = vm.globalAt(found->second);
                if (v.isObjType(ObjType::Class)) return asClass(v);
            }
        }
        return vm.classNamed(name);
    }

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
            if (ObjClass* proxy = classNamed(vm, "__ScriptProxy_" + face)) {
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
void serve(VM& vm, Side me, const std::shared_ptr<Request>& request) {
    const std::shared_ptr<ScriptRun>& run = request->run;
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
        const auto& names = run->compiled->names;
        switch (request->kind) {
            case Request::TopLevel: {
                const auto found = names.find(request->member);
                const Value function = found == names.end() || request->member.rfind("__", 0) == 0 ? Value::null() : vm.globalAt(found->second);
                if (!function.isObjType(ObjType::Closure)) {
                    failure = "the script has no function named '" + request->member + "'";
                    break;
                }
                answered = vm.call(function, static_cast<int>(given.size()), given.data());
                break;
            }
            case Request::Gives: {
                const auto found = names.find("__scriptGives");
                answered = found == names.end() ? Value::null() : vm.globalAt(found->second);
                break;
            }
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
    ScriptHost& host = *run->host;
    {
        std::lock_guard<std::mutex> hold(host.mutex);
        request->result = std::move(result);
        request->failure = std::move(failure);
        request->done = true;
    }
    host.changed.notify_all();
    if (stopped) throw WorkerStop{};
}

// Serves, in turn, what the other side has asked of `me` in this host, for any of its scripts.
void serveAsked(VM& vm, const std::shared_ptr<ScriptHost>& host, Side me) {
    for (;;) {
        std::shared_ptr<Request> next;
        {
            std::lock_guard<std::mutex> hold(host->mutex);
            if (host->asked[me].empty()) return;
            next = host->asked[me].front();
            host->asked[me].pop_front();
        }
        serve(vm, me, next);
    }
}

// Asks the other side of `me` to do `request`: queued, and its event loop told, should its thread
// be doing nothing. False where that side, or the script, is not there to ask.
bool ask(Side me, const std::shared_ptr<Request>& request) {
    const Side other = me == kProgram ? kScript : kProgram;
    const std::shared_ptr<ScriptHost> host = request->run->host;
    std::lock_guard<std::mutex> hold(host->mutex);
    VM* there = host->sides[other];
    if (there == nullptr || host->ended || request->run->ended) return false;
    host->asked[other].push_back(request);
    there->postNative([host, other](VM& vm) { serveAsked(vm, host, other); });
    host->changed.notify_all();
    return true;
}

// Waits, with the VM's lock given up, until `finished` (read under the host's mutex), the host
// ends, or the other side asks something, which is then served here.
template <class Finished>
void waitServing(VM& vm, const std::shared_ptr<ScriptHost>& host, Side me, Finished finished) {
    for (;;) {
        bool serveNow = false;
        {
            LockReleased released(vm);
            std::unique_lock<std::mutex> hold(host->mutex);
            host->changed.wait(hold, [&] { return finished() || host->ended || !host->asked[me].empty(); });
            if (finished() || (host->ended && host->asked[me].empty())) return;
            serveNow = !host->asked[me].empty();
        }
        if (serveNow) serveAsked(vm, host, me);
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

// Why a run is not there to ask, as text.
std::string notThere(const ScriptRun& run, Side me) {
    std::string failure = !run.failure.empty() ? run.failure : run.host->failure;
    return std::string(me == kProgram ? "the script is not running" : "the program is not there to ask") +
           (failure.empty() ? std::string() : ": " + failure);
}

// Asks the other side and waits for the answer, as a pair.
Value request(VM& vm, Side me, const std::shared_ptr<Request>& asked) {
    const std::shared_ptr<ScriptRun>& run = asked->run;
    const std::shared_ptr<ScriptHost> host = run->host;
    if (!ask(me, asked)) {
        std::string failure;
        {
            std::lock_guard<std::mutex> hold(host->mutex);
            failure = notThere(*run, me);
        }
        return answerPair(vm, false, vm.str(failure));
    }
    waitServing(vm, host, me, [&] { return asked->done; });
    std::string failure;
    {
        std::lock_guard<std::mutex> hold(host->mutex);
        failure = asked->done ? asked->failure
                              : "the " + std::string(me == kProgram ? "script" : "program") + " ended before it answered" +
                                    (host->failure.empty() ? std::string() : ": " + host->failure);
    }
    if (!failure.empty()) return answerPair(vm, false, vm.str(failure));
    // What this side cannot make again (a data class it has not) fails as the call.
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
    asked->run = run;
    asked->target = handle.id;
    asked->member = member;
    ObjList* list = newList(vm);
    TempRoot keepList(vm, Value::object(list));
    for (int i = 0; i < argc; i++) vm.listAdd(list, args[i]);
    asked->arguments = write(vm, run, me, Value::object(list));
    const Value answer = request(vm, me, asked);
    // A function value's call answers itself, or throws where the other side threw.
    if (kind == Request::Function) {
        ObjList* pair = asList(answer);
        if (!vm.listGet(pair, 0).truthy()) throw vm.runtimeError(Thrown::IllegalStateException, vm.stringify(vm.listGet(pair, 1)));
        return vm.listGet(pair, 1);
    }
    return answer;
}

// ---- Starting a script, on its host's thread ----

// Runs a script's top levels in its host's VM (the library's first, where none has run yet).
// What its top level threw, as text, or nothing.
std::string runTopLevel(VM& vm, const std::shared_ptr<ScriptRun>& run) {
    {
        std::lock_guard<std::mutex> hold(runsMutex);
        starting[&vm] = run;
    }
    std::string failure;
    try {
        for (int module : run->compiled->modules) vm.runModule(module);
    } catch (WwThrow& thrown) {
        failure = describe(vm, thrown);
    }
    ScriptHost& host = *run->host;
    {
        std::lock_guard<std::mutex> hold(host.mutex);
        if (failure.empty()) run->ready = true;
        else {
            run->ended = true;
            run->failure = failure;
        }
    }
    host.changed.notify_all();
    return failure;
}

// Ends a host as its thread ends: every script in it ended with it.
void hostEnded(ScriptHost& host, const std::string& failure) {
    {
        std::lock_guard<std::mutex> hold(host.mutex);
        host.sides[kScript] = nullptr;
        host.ended = true;
        host.failure = failure;
        host.asked[kScript].clear();
    }
    host.changed.notify_all();
}

// The thread of a script with a VM of its own, made from its compile's image. `parent` counts
// this thread among its own until the very end: while it does, the program ends its process at
// once rather than taking itself apart under the thread (run.cpp, runProgram).
void ownMain(std::shared_ptr<ScriptHost> host, std::shared_ptr<ScriptRun> run, std::shared_ptr<Host> files, VM* parent) {
    // Kept on the heap: if threads of its own are still running when it ends, the VM is left as
    // it is rather than taken apart under them (as a worker's is).
    VM* vm = new VM(true);
    Program* program = new Program;
    std::shared_ptr<const CompiledScript> compiled = run->compiled;
    vm->host = std::move(files);  // in the sandbox of the code that started it, if any
    vm->programImage = [compiled] { return compiled->image; };
    std::string failure;
    bool waiting = false;
    try {
        readProgram(*program, *vm, compiled->image, true);
        vm->load(*program);
        vm->jobStarted();  // it waits for calls until it is stopped
        waiting = true;
        {
            std::lock_guard<std::mutex> hold(host->mutex);
            host->sides[kScript] = vm;
            host->up = true;
        }
        if (runTopLevel(*vm, run).empty()) vm->runEventLoop(true);
    } catch (WwThrow& thrown) {
        failure = describe(*vm, thrown);
    } catch (const WwExit&) {
    } catch (const WorkerStop&) {
    } catch (const std::exception& problem) {
        failure = std::string("the script could not start: ") + problem.what();
    }
    {
        std::lock_guard<std::mutex> hold(host->mutex);
        run->ended = true;
        if (run->failure.empty()) run->failure = failure.empty() && !run->ready ? std::string("the script ended before its top level had run") : failure;
    }
    hostEnded(*host, failure);
    {
        std::lock_guard<std::mutex> hold(runsMutex);
        starting.erase(vm);
    }
    if (waiting) vm->jobFinished();
    if (!vm->threadsAlive()) {
        delete vm;
        delete program;
    }
    parent->scriptEnded();
}

// The thread of a ScriptVM: a VM with the standard library loaded and a compiler of its own,
// both on this thread, which compiles and runs the scripts the program gives it.
void sharedMain(std::shared_ptr<ScriptHost> host, std::shared_ptr<Host> files, VM* parent) {
    VM* vm = new VM(true);
    vm->host = std::move(files);
    std::string failure;
    bool waiting = false;
    {
        parsePreludeAlone(true);  // see compiler.h
        auto* session = new CompilerSession(*vm);
        try {
            session->libraryPaths = commandLibraryPaths();
            vm->load(session->program);
            vm->jobStarted();  // it waits for scripts and calls until it is stopped
            waiting = true;
            {
                std::lock_guard<std::mutex> hold(host->mutex);
                host->sides[kScript] = vm;
                host->session = session;
                host->up = true;
            }
            host->changed.notify_all();
            vm->runEventLoop(true);
        } catch (WwThrow& thrown) {
            failure = describe(*vm, thrown);
        } catch (const WwExit&) {
        } catch (const WorkerStop&) {
        } catch (const std::exception& problem) {
            failure = std::string("the ScriptVM could not start: ") + problem.what();
        }
        {
            std::lock_guard<std::mutex> hold(host->mutex);
            host->session = nullptr;
        }
        // What its compiles kept is written from the session's code, so before the session goes.
        finishKeeping();
        if (!vm->threadsAlive()) delete session;
    }
    hostEnded(*host, failure);
    {
        std::lock_guard<std::mutex> hold(runsMutex);
        starting.erase(vm);
    }
    if (waiting) vm->jobFinished();
    if (!vm->threadsAlive()) delete vm;
    parent->scriptEnded();
}

std::shared_ptr<ScriptRun> runStarting(VM& vm) {
    std::lock_guard<std::mutex> hold(runsMutex);
    auto found = starting.find(&vm);
    return found == starting.end() ? nullptr : found->second.lock();
}

// A list of the problems, each [file, line, column, message], as script.ww reads them; in the
// order of the text (the checker finds them in its own order).
Value problemList(VM& vm, std::vector<Problem> problems) {
    std::stable_sort(problems.begin(), problems.end(), [](const Problem& a, const Problem& b) {
        return a.file != b.file ? a.file < b.file : a.line != b.line ? a.line < b.line : a.col < b.col;
    });
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

// What a script's compile is given.
struct CompileArgs {
    std::string source, name, directory, contractName, contractSource, api, gives;
};

// Compiles a script into a program of its own, on a thread of its own with a VM of its own (a
// VM keeps things for its thread): its image, or what is wrong with it.
void compileAlone(const CompileArgs& a, std::shared_ptr<CompiledScript>& made, std::vector<Problem>& problems, std::string& broke) {
    std::thread([&] {
        std::lock_guard<std::mutex> one(compiling);
        try {
            // With WW_TIMES set, how long each part of a script's compile took, on standard error.
            using Clock = std::chrono::steady_clock;
            const auto began = Clock::now();
            auto since = [](Clock::time_point from) { return std::chrono::duration<double, std::milli>(Clock::now() - from).count(); };
            parsePreludeAlone(true);  // compiled again and again: see compiler.h
            VM own(true);
            const double vmMade = since(began);
            CompilerSession session(own);
            const double prelude = since(began) - vmMade;
            session.libraryPaths = commandLibraryPaths();
            auto compiled = std::make_shared<CompiledScript>();
            compiled->name = a.name;
            compiled->gives = !a.gives.empty();
            session.compileScript(a.source, a.name, a.directory, compiled->names, a.contractSource, a.contractName, a.api, a.gives, compiled->modules,
                                  &compiled->contractSlots);
            const double compiledAt = since(began);
            std::ostringstream image;
            writeProgram(session.program, image);
            // What it kept compiled (a library it imports) is written from this session's code, so
            // before the session goes.
            finishKeeping();
            if (std::getenv("WW_TIMES") != nullptr)
                std::fprintf(stderr, "ww: script %s compiled in %.1f ms: a VM %.1f, the standard library %.1f, the script %.1f, its image (%zu bytes) and what was kept %.1f\n",
                             a.name.c_str(), since(began), vmMade, prelude, compiledAt - vmMade - prelude, image.str().size(), since(began) - compiledAt);
            compiled->image = image.str();
            made = std::move(compiled);
        } catch (const CompileError& e) {
            addProblems(e, problems);
        } catch (const std::exception& e) {
            broke = e.what();
        }
    }).join();
}

// A ScriptVM's compile, done on its own thread by its own compiler and loaded there.
struct SharedCompile {
    CompileArgs args;
    std::shared_ptr<CompiledScript> made;
    std::vector<Problem> problems;
    std::string broke;
    bool done = false;
};

void compileShared(VM& vm, const std::shared_ptr<ScriptHost>& host, const std::shared_ptr<SharedCompile>& job) {
    std::shared_ptr<CompiledScript> made;
    std::vector<Problem> problems;
    std::string broke;
    {
        std::lock_guard<std::mutex> one(compiling);
        CompilerSession* session = nullptr;
        {
            std::lock_guard<std::mutex> hold(host->mutex);
            session = host->session;
        }
        try {
            if (session == nullptr) throw std::runtime_error("the ScriptVM has ended");
            const auto began = std::chrono::steady_clock::now();
            const CompileArgs& a = job->args;
            auto compiled = std::make_shared<CompiledScript>();
            compiled->name = a.name;
            compiled->gives = !a.gives.empty();
            compiled->in = host;
            session->compileScript(a.source, a.name, a.directory, compiled->names, a.contractSource, a.contractName, a.api, a.gives, compiled->modules,
                                   &compiled->contractSlots);
            vm.load(session->program);
            if (std::getenv("WW_TIMES") != nullptr)
                std::fprintf(stderr, "ww: script %s compiled in its ScriptVM in %.1f ms\n", a.name.c_str(),
                             std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - began).count());
            made = std::move(compiled);
        } catch (const CompileError& e) {
            addProblems(e, problems);
        } catch (const std::exception& e) {
            broke = e.what();
        }
    }
    {
        std::lock_guard<std::mutex> hold(host->mutex);
        job->made = std::move(made);
        job->problems = std::move(problems);
        job->broke = std::move(broke);
        job->done = true;
    }
    host->changed.notify_all();
}

// Waits for a host's thread to be up, its VM made; false where it ended first.
bool waitUp(VM& vm, const std::shared_ptr<ScriptHost>& host) {
    LockReleased released(vm);
    std::unique_lock<std::mutex> hold(host->mutex);
    host->changed.wait(hold, [&] { return host->up || host->ended; });
    return host->up && !host->ended;
}

}  // namespace

void registerScriptsHere(VM& vm) {
    ObjClass* script = vm.alloc<ObjClass>(vm.intern("script"));
    auto def = [&](const char* name, int minArgs, int maxArgs, NativeFn fn) {
        script->statics.set(vm.intern(name), Value::object(vm.alloc<ObjNative>(std::string("script.") + name, minArgs, maxArgs, fn)));
    };

    // vm(): a ScriptVM, its thread started and its VM made, the standard library loaded.
    def("vm", 0, 0, [](VM& vm, int, Value*) -> Value {
        auto host = std::make_shared<ScriptHost>();
        host->shared = true;
        host->sides[kProgram] = &vm;
        vm.scriptStarted();
        std::thread(sharedMain, host, vm.currentHostShared(), &vm).detach();
        if (!waitUp(vm, host)) {
            std::string failure;
            {
                std::lock_guard<std::mutex> hold(host->mutex);
                failure = host->failure;
            }
            throw vm.runtimeError(Thrown::IllegalStateException, "a ScriptVM could not start" + (failure.empty() ? std::string() : ": " + failure));
        }
        auto* kept = new std::shared_ptr<ScriptHost>(host);
        return Value::object(vm.alloc<ObjResource>("script vm", reinterpret_cast<int64_t>(kept), &releaseHost));
    });

    // compile(vm, source, name, directory, contractName, contractSource, api, gives): the script
    // compiled (in the ScriptVM `vm`, or, where it is null, as a program of its own), or a list
    // of what is wrong with it. This thread lets the program's other threads go on meanwhile.
    def("compile", 8, 8, [](VM& vm, int, Value* args) -> Value {
        CompileArgs a{vm.toStr(args[1], "the script's source"), vm.toStr(args[2], "the script's name"), vm.toStr(args[3], "the script's folder"),
                      vm.toStr(args[4], "the contract's name"), vm.toStr(args[5], "the contract"), vm.toStr(args[6], "the api's interface"),
                      vm.toStr(args[7], "the interface it gives")};
        std::shared_ptr<CompiledScript> made;
        std::vector<Problem> problems;
        std::string broke;
        if (args[0].isNull()) {
            LockReleased released(vm);
            compileAlone(a, made, problems, broke);
        } else {
            std::shared_ptr<ScriptHost> host = hostOf(vm, args[0]);
            auto job = std::make_shared<SharedCompile>();
            job->args = std::move(a);
            VM* there = nullptr;
            {
                std::lock_guard<std::mutex> hold(host->mutex);
                if (!host->ended) there = host->sides[kScript];
                if (there != nullptr) there->postNative([host, job](VM& own) { compileShared(own, host, job); });
            }
            if (there == nullptr) throw vm.runtimeError(Thrown::IllegalStateException, "the ScriptVM has been stopped");
            {
                LockReleased released(vm);
                std::unique_lock<std::mutex> hold(host->mutex);
                host->changed.wait(hold, [&] { return job->done || host->ended; });
                if (!job->done) broke = "the ScriptVM ended" + (host->failure.empty() ? std::string() : ": " + host->failure);
            }
            made = job->made;
            problems = job->problems;
            if (broke.empty()) broke = job->broke;
        }
        if (!broke.empty()) throw vm.runtimeError(Thrown::IllegalStateException, "the script could not be compiled: " + broke);
        if (made == nullptr) return problemList(vm, problems);
        // Its image counted as the heap's, so that one dropped is collected in time (foreign memory's is so).
        vm.account(made->image.size());
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
    // threw, as text. `api` is what the script's `api` stands for, if anything. A script of its
    // own gets a VM of its own; one compiled in a ScriptVM starts there, once.
    def("start", 2, 2, [](VM& vm, int, Value* args) -> Value {
        std::shared_ptr<const CompiledScript> compiled = compiledOf(vm, args[0]);
        auto run = std::make_shared<ScriptRun>();
        run->compiled = compiled;
        std::shared_ptr<ScriptHost> host;
        if (compiled->image.empty()) {
            host = compiled->in.lock();
            if (host == nullptr) throw vm.runtimeError(Thrown::IllegalStateException, "the ScriptVM it was compiled in has ended");
            {
                std::lock_guard<std::mutex> hold(host->mutex);
                if (compiled->started) throw vm.runtimeError(Thrown::IllegalStateException, compiled->name + " has been started in its ScriptVM already: compile it again for another");
                compiled->started = true;
            }
            run->host = host;
            if (!args[1].isNull()) run->api = write(vm, run, kProgram, args[1]);
            VM* there = nullptr;
            {
                std::lock_guard<std::mutex> hold(host->mutex);
                if (!host->ended) there = host->sides[kScript];
                if (there != nullptr) there->postNative([run](VM& own) { runTopLevel(own, run); });
            }
            if (there == nullptr) throw vm.runtimeError(Thrown::IllegalStateException, "the ScriptVM has been stopped");
        } else {
            host = std::make_shared<ScriptHost>();
            host->sides[kProgram] = &vm;
            run->host = host;
            if (!args[1].isNull()) run->api = write(vm, run, kProgram, args[1]);
            vm.scriptStarted();
            std::thread(ownMain, host, run, vm.currentHostShared(), &vm).detach();
        }
        // Its top level may call the program's api: served here while this waits.
        waitServing(vm, host, kProgram, [&] { return run->ready || run->ended; });
        std::string failure;
        {
            std::lock_guard<std::mutex> hold(host->mutex);
            if (!run->ready) failure = !run->failure.empty() ? run->failure : host->failure.empty() ? "the script ended before its top level had run" : host->failure;
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
        asked->run = run;
        asked->member = vm.toStr(args[1], "the function's name");
        asked->arguments = write(vm, run, kProgram, args[2]);
        return request(vm, kProgram, asked);
    });

    // gives(running): [true, the script's object of the interface it gives] or [false, why not].
    def("gives", 1, 1, [](VM& vm, int, Value* args) -> Value {
        std::shared_ptr<ScriptRun> run = runOf(vm, args[0]);
        if (!run->compiled->gives) return answerPair(vm, false, vm.str("the script was compiled with no gives = ...: it gives no object"));
        auto asked = std::make_shared<Request>();
        asked->kind = Request::Gives;
        asked->run = run;
        return request(vm, kProgram, asked);
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

    // In a script, as its top level runs: the object its `api` stands for, the program's own.
    def("api", 0, 0, [](VM& vm, int, Value*) -> Value {
        std::shared_ptr<ScriptRun> run = runStarting(vm);
        if (run == nullptr || run->api == nullptr) throw vm.runtimeError(Thrown::IllegalStateException, "this script was started with no api object");
        return read(vm, run, kScript, *run->api);
    });

    // Stops a script, whatever it is doing, and waits until it has ended. In a ScriptVM, the
    // script alone ends (no more calls, its handles let go), the VM going on for the others.
    def("stop", 1, 1, [](VM& vm, int, Value* args) -> Value {
        std::shared_ptr<ScriptRun> run = runOf(vm, args[0]);
        std::shared_ptr<ScriptHost> host = run->host;
        if (host->shared) {
            VM* there = nullptr;
            {
                std::lock_guard<std::mutex> hold(host->mutex);
                run->ended = true;
                if (!host->ended) there = host->sides[kScript];
                if (there != nullptr) there->postNative([run](VM& own) { forgetHandles(own, run.get()); });
            }
            host->changed.notify_all();
        } else {
            stopHost(*host);
            LockReleased released(vm);
            std::unique_lock<std::mutex> hold(host->mutex);
            host->changed.wait(hold, [&] { return host->ended; });
        }
        forgetHandles(vm, run.get());
        return Value::null();
    });

    def("isRunning", 1, 1, [](VM& vm, int, Value* args) -> Value {
        std::shared_ptr<ScriptRun> run = runOf(vm, args[0]);
        std::lock_guard<std::mutex> hold(run->host->mutex);
        return Value::boolean(!run->ended && !run->host->ended);
    });

    // A ScriptVM stopped, every script in it with it, waited for.
    def("stopVm", 1, 1, [](VM& vm, int, Value* args) -> Value {
        std::shared_ptr<ScriptHost> host = hostOf(vm, args[0]);
        stopHost(*host);
        LockReleased released(vm);
        std::unique_lock<std::mutex> hold(host->mutex);
        host->changed.wait(hold, [&] { return host->ended; });
        return Value::null();
    });

    def("vmIsRunning", 1, 1, [](VM& vm, int, Value* args) -> Value {
        std::shared_ptr<ScriptHost> host = hostOf(vm, args[0]);
        std::lock_guard<std::mutex> hold(host->mutex);
        return Value::boolean(!host->ended);
    });

    vm.defineGlobal("__script", Value::object(script));
}

}  // namespace ww
