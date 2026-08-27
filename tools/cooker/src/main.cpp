// The asset cooker: source tree in, cooked tree out.
//
// ⚑⚑ THIS FILE HELD THE COOK ITSELF UNTIL PHASE 24 STAGE T, AND WHAT IS LEFT IS
// THE POINT. The extension dispatch, the staleness policy, the output-collision
// abort and the stray sweep are in `cook.hpp` now, beside the codecs, for the
// reason `outputs.hpp` wrote down when it moved for the same one: `cooker.unit`
// links the LIBRARY, and a rule that lives in the executable is a rule with no
// test. A guard that can stop an entire build and a sweep that DELETES were
// untested since Phase 5 for no reason other than which file they sat in.
//
// ⚑ And the second reason, which is what stage T is for: the Forge already
// links `sol_cooker_lib`, so a cook that is a library call is a cook the tool
// can run - without `cooker.exe` being shipped, which keeps
// `packaging/check_layout.cmake`'s `cooker*` exclusion true.
//
// What stays here is what only a command line has: argv, and an exit code.

#include "cook.hpp"

#include "sol/core/log.hpp"

int main(int argc, char** argv)
{
    if (argc != 3) {
        SOL_LOG_ERROR("usage: cooker <source-assets-dir> <output-dir>");
        return 1;
    }

    // ⚑ Nothing is logged here: `cookDirectory` reports every cook, every
    // failure and its own tally, so that the Forge's run says exactly what a
    // command-line run says.
    return sol::cooker::cookDirectory(argv[1], argv[2]).ok() ? 0 : 1;
}
