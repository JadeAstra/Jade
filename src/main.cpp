// The ww executable's start (M6.5 part 5): a program built into the executable runs, and
// otherwise this is the ww command. In no section a built program leaves out, and not in
// the build's proofs, which have a start of their own (proof_main.cpp): it names the command,
// and the scripts a program compiles as it runs, both on the compiler side.

#include "run.h"

int main(int argc, char** argv) {
    ww::registerScripts = ww::registerScriptsHere;  // called only where that side is in the file
    return ww::startExecutable(argc, argv, ww::wwCommandMain);
}
