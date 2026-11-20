// The sampling profiler: a clock thread asks the program's VM for a sample every so often; the
// VM takes it at its next safe point (VM::safePoint), where the calls under way are where they
// are, and hands them here. The report says how much of the samples each function and line
// had, itself (the code in it) and in all (with what it called), and keeps the stacks for a
// flame graph. Only code running counts: a thread asleep, or waiting on a lock, a file or the
// network, is not sampled.

#include "profile.h"

#include "vm.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <map>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <unordered_set>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace ww::profile {

namespace {

struct State {
    std::string path;
    int microseconds = 1000;
    std::chrono::steady_clock::time_point began;

    std::mutex mutex;  // everything below
    VM* vm = nullptr;
    bool attached = false;  // a VM was given: later ones (workers, scripts) are not sampled
    uint64_t samples = 0;
    std::unordered_map<std::string, uint64_t> self, total, lines, threads;
    std::unordered_map<std::string, std::string> fileOf;  // a function's file, for the report
    std::unordered_map<std::string, uint64_t> folded;

    std::thread clock;
    std::mutex wait;
    std::condition_variable woken;
    std::atomic<bool> stopping{false};
    bool finished = false;
};

std::atomic<bool> enabled{false};
State* state = nullptr;

std::string functionKey(const Call& c) { return c.name + "  (" + c.source + ")"; }

std::string percent(uint64_t part, uint64_t whole) {
    char text[16];
    std::snprintf(text, sizeof text, "%5.1f%%", whole == 0 ? 0.0 : 100.0 * static_cast<double>(part) / static_cast<double>(whole));
    return text;
}

std::string padded(uint64_t n, size_t width) {
    std::string s = std::to_string(n);
    return s.size() >= width ? s : std::string(width - s.size(), ' ') + s;
}

// The entries of a count, most first, at most `limit`.
std::vector<std::pair<std::string, uint64_t>> top(const std::unordered_map<std::string, uint64_t>& counts, size_t limit) {
    std::vector<std::pair<std::string, uint64_t>> list(counts.begin(), counts.end());
    std::sort(list.begin(), list.end(), [](const auto& a, const auto& b) { return a.second != b.second ? a.second > b.second : a.first < b.first; });
    if (list.size() > limit) list.resize(limit);
    return list;
}

void ticks() {
    State& s = *state;
#ifdef _WIN32
    // Windows wakes a plain wait every 15.6 ms: a high-resolution timer wakes at the time asked.
    HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    if (timer == nullptr) timer = CreateWaitableTimerExW(nullptr, nullptr, 0, TIMER_ALL_ACCESS);
    LARGE_INTEGER due{};
    due.QuadPart = -10LL * s.microseconds;  // 100 ns units, negative: from now
    SetWaitableTimer(timer, &due, std::max(1, s.microseconds / 1000), nullptr, nullptr, FALSE);
    while (!s.stopping) {
        WaitForSingleObject(timer, 100);
        if (s.stopping) break;
        std::lock_guard<std::mutex> vmHold(s.mutex);
        if (s.vm != nullptr) s.vm->sampleWanted();
    }
    CloseHandle(timer);
#else
    std::unique_lock<std::mutex> hold(s.wait);
    while (!s.stopping) {
        s.woken.wait_for(hold, std::chrono::microseconds(s.microseconds));
        if (s.stopping) break;
        std::lock_guard<std::mutex> vmHold(s.mutex);
        if (s.vm != nullptr) s.vm->sampleWanted();
    }
#endif
}

void write() {
    State& s = *state;
    std::lock_guard<std::mutex> hold(s.mutex);
    const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - s.began).count();
    std::ofstream out(s.path, std::ios::binary);
    if (!out) {
        std::cerr << "ww: cannot write the profile to " << s.path << "\n";
        return;
    }
    out << "WinterWolf profile: " << s.samples << " samples, one each " << s.microseconds << " us at most, over "
        << static_cast<int64_t>(seconds * 1000.0) << " ms\n";
    out << "A sample is taken at the program's next loop turn or call after the clock asks, so time in a short function\n"
           "that neither loops nor calls is mostly counted where its caller next calls or loops. Only code running is\n"
           "counted, not a thread asleep or waiting. Self: the code of the function itself; total: with what it calls.\n"
           "Functions put in place of their calls (inline) are counted as the calls they were.\n";
    if (s.threads.size() > 1) {
        out << "\nThreads\n";
        for (const auto& [name, n] : top(s.threads, 50)) out << "  " << percent(n, s.samples) << "  " << padded(n, 8) << "  " << name << "\n";
    }
    out << "\nFunctions, by their own time\n    self   total   samples  function\n";
    for (const auto& [name, n] : top(s.self, 60)) {
        auto t = s.total.find(name);
        out << "  " << percent(n, s.samples) << "  " << percent(t != s.total.end() ? t->second : n, s.samples) << "  " << padded(n, 8) << "  " << name << "\n";
    }
    out << "\nFunctions, by total time (with what they call)\n   total    self   samples  function\n";
    for (const auto& [name, n] : top(s.total, 60)) {
        auto own = s.self.find(name);
        out << "  " << percent(n, s.samples) << "  " << percent(own != s.self.end() ? own->second : 0, s.samples) << "  " << padded(n, 8) << "  " << name << "\n";
    }
    out << "\nLines, by their own time\n    self   samples  line\n";
    for (const auto& [line, n] : top(s.lines, 80)) out << "  " << percent(n, s.samples) << "  " << padded(n, 8) << "  " << line << "\n";
    out << "\nThe stacks, for a flame graph (speedscope.app, or flamegraph.pl), are in " << s.path << ".folded\n";

    std::ofstream stacks(s.path + ".folded", std::ios::binary);
    for (const auto& [stack, n] : s.folded) stacks << stack << " " << n << "\n";
    std::cerr << "ww: profile of " << s.samples << " samples written to " << s.path << "\n";
}

}  // namespace

void start(const std::string& path, int microseconds) {
    if (state != nullptr) return;
    state = new State;  // kept to the end: the clock and the report outlive every VM
    state->path = path;
    state->microseconds = std::max(50, microseconds);
    state->began = std::chrono::steady_clock::now();
    enabled = true;
    state->clock = std::thread(ticks);
    std::atexit([] { finish(); });
}

bool on() { return enabled.load(std::memory_order_relaxed); }

void attach(VM& vm) {
    if (!on()) return;
    std::lock_guard<std::mutex> hold(state->mutex);
    if (state->attached) return;
    state->attached = true;
    state->vm = &vm;
}

void detach(VM& vm) {
    if (!on()) return;
    std::lock_guard<std::mutex> hold(state->mutex);
    if (state->vm == &vm) state->vm = nullptr;
}

void record(const std::vector<Call>& calls, const std::string& thread) {
    if (!on() || calls.empty()) return;
    State& s = *state;
    std::lock_guard<std::mutex> hold(s.mutex);
    s.samples++;
    s.threads[thread.empty() ? std::string("main") : thread]++;
    const Call& innermost = calls.front();
    s.self[functionKey(innermost)]++;
    s.lines[innermost.source + ":" + std::to_string(innermost.line) + "  " + innermost.name]++;
    std::unordered_set<std::string> seen;  // a function called inside itself counts once
    std::string stack = thread.empty() ? std::string("main") : thread;
    for (size_t i = calls.size(); i-- > 0;) {
        const std::string key = functionKey(calls[i]);
        if (seen.insert(key).second) s.total[key]++;
        stack += ";" + calls[i].name;
    }
    s.folded[stack]++;
}

void finish() {
    if (state == nullptr) return;
    {
        std::lock_guard<std::mutex> hold(state->wait);
        if (state->finished) return;
        state->finished = true;
        state->stopping = true;
    }
    state->woken.notify_all();
    if (state->clock.joinable()) state->clock.join();
    enabled = false;
    write();
}

}  // namespace ww::profile
