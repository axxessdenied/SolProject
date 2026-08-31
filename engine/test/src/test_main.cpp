#include "sol/test/test.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <vector>

#if defined(_MSC_VER)
#include <crtdbg.h>
#endif

namespace sol::test {

namespace {

TestCase* g_head = nullptr;
TestCase* g_tail = nullptr;
int g_failuresInCurrentTest = 0;

// ⚑⚑⚑ PHASE 33 STAGE A. THIS EXISTS BECAUSE A SUITE RUNTIME THAT NOBODY CAN
// ATTRIBUTE IS A SUITE RUNTIME NOBODY CAN DEFEND, and this project has now
// twice been surprised by its own test suite. `ctest` reports one number per
// BINARY, which is where the trail used to end: `sim.unit` was 84 s and there
// was no way to learn, without editing this file, that 81.5 s of it was TWO
// tests out of 224 and that the third-largest was 34 ms. The arc's standing
// risk 2 was written against a `ctest --preset dev` of "~90-107 s" and the
// real figure when Phase 33 opened was 171 s - a phase's worth of drift that
// nothing reported, because nothing was watching.
//
// ⚑⚑ IT IS QUIET WHEN THERE IS NOTHING TO SAY, and that is what keeps it from
// becoming noise nobody reads. Nine of this project's thirteen test binaries
// finish in under two seconds; printing a table for them would train a reader
// to skip the block in the four cases where it matters. So: the total is
// always one line, and the breakdown appears only when the binary is slow
// enough for the question to be worth asking.
constexpr double kSlowBinarySeconds = 1.0;
// A row has to be worth a line. 5% of a slow binary is a real share; below
// that the honest report is the total and nothing else.
constexpr double kSlowTestFraction = 0.05;
constexpr std::size_t kMaxSlowRows = 8;

struct Timing
{
    const char* name = nullptr;
    double seconds = 0.0;
};

} // namespace

void registerTest(TestCase& testCase)
{
    if (g_tail != nullptr) {
        g_tail->next = &testCase;
    } else {
        g_head = &testCase;
    }
    g_tail = &testCase;
}

void reportFailure(const char* expression, const char* file, int line)
{
    std::printf("    FAILED: %s (%s:%d)\n", expression, file, line);
    ++g_failuresInCurrentTest;
}

int runAllTests()
{
    int testCount = 0;
    int failedCount = 0;
    std::vector<Timing> timings;
    double totalSeconds = 0.0;

    for (TestCase* testCase = g_head; testCase != nullptr; testCase = testCase->next) {
        ++testCount;
        g_failuresInCurrentTest = 0;
        std::printf("[ RUN  ] %s\n", testCase->name);
        // ⚑ FLUSHED BEFORE THE TEST RUNS, NOT AFTER IT. stdout is buffered and a
        // CRT assertion goes to stderr UNBUFFERED, so a test that aborts - an
        // out-of-range subscript, a failed SOL_ASSERT - takes the whole pending
        // buffer with it and the name of the test that crashed is exactly what
        // is lost. This repo has paid for that twice while chasing mutants.
        std::fflush(stdout);
        // ⚑ Around the test body ONLY. The printf either side is the runner's
        // own cost and belongs to nobody's test, and at 224 tests it would be
        // the only thing a fast binary's table ever showed.
        const std::chrono::steady_clock::time_point started = std::chrono::steady_clock::now();
        testCase->fn();
        const double seconds =
            std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
        timings.push_back({testCase->name, seconds});
        totalSeconds += seconds;
        if (g_failuresInCurrentTest == 0) {
            std::printf("[  OK  ] %s\n", testCase->name);
        } else {
            std::printf("[ FAIL ] %s (%d check(s) failed)\n", testCase->name, g_failuresInCurrentTest);
            ++failedCount;
        }
    }

    // ⚑ UNCHANGED, DELIBERATELY. This exact line is what the hand-over checks
    // to prove a binary is not a stale build ("223 test(s) ran"), so the
    // timing report is added after it rather than folded into it.
    std::printf("%d test(s) ran, %d failed\n", testCount, failedCount);
    std::printf("%.2f s in test bodies\n", totalSeconds);
    if (totalSeconds >= kSlowBinarySeconds) {
        std::sort(timings.begin(), timings.end(), [](const Timing& a, const Timing& b) {
            return a.seconds > b.seconds;
        });
        std::size_t shown = 0;
        for (const Timing& timing : timings) {
            if (shown >= kMaxSlowRows || timing.seconds < totalSeconds * kSlowTestFraction) {
                break;
            }
            std::printf("  %5.1f%%  %8.2f s  %s\n",
                        totalSeconds > 0.0 ? timing.seconds / totalSeconds * 100.0 : 0.0,
                        timing.seconds,
                        timing.name);
            ++shown;
        }
    }
    return (failedCount == 0 && testCount > 0) ? 0 : 1;
}

} // namespace sol::test

int main()
{
#if defined(_MSC_VER)
    // Debug-CRT assertions (e.g. vector subscript out of range) must fail the
    // process on stderr, not block forever in an invisible dialog box.
    _CrtSetReportMode(_CRT_ASSERT, _CRTDBG_MODE_FILE);
    _CrtSetReportFile(_CRT_ASSERT, _CRTDBG_FILE_STDERR);
    _CrtSetReportMode(_CRT_ERROR, _CRTDBG_MODE_FILE);
    _CrtSetReportFile(_CRT_ERROR, _CRTDBG_FILE_STDERR);
    _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
#endif
    return sol::test::runAllTests();
}
