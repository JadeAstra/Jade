// The sampling profiler (ww --profile, WW_PROFILE): where a program spends its time, by
// function and by line, from the calls under way at moments a clock picks.
#pragma once

#include <string>
#include <vector>

namespace ww {

class VM;

namespace profile {

// A call under way: its function's name, the file and the line it is at.
struct Call {
    std::string name;
    std::string source;
    int line = 0;
};

// Turns sampling on for the program about to run: every `microseconds` the program's VM is
// asked for the calls under way at its next safe point. The report is written to `path`, and
// the stacks for a flame graph to `path` + ".folded", as the process ends.
void start(const std::string& path, int microseconds);
// Whether it is on.
bool on();
// The program's VM, made or going: the one sampled (the first made while it is on).
void attach(VM& vm);
void detach(VM& vm);
// One sample: the calls under way, innermost first, on the thread named.
void record(const std::vector<Call>& calls, const std::string& thread);
// Writes the report now, once (the process ending does it otherwise).
void finish();

}  // namespace profile
}  // namespace ww
