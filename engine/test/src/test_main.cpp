#include "sol/test/test.hpp"

#include <cstdio>
#include <cstdlib>

#if defined(_MSC_VER)
#include <crtdbg.h>
#endif

namespace sol::test {

namespace {

TestCase* g_head = nullptr;
TestCase* g_tail = nullptr;
int g_failuresInCurrentTest = 0;

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
        testCase->fn();
        if (g_failuresInCurrentTest == 0) {
            std::printf("[  OK  ] %s\n", testCase->name);
        } else {
            std::printf("[ FAIL ] %s (%d check(s) failed)\n", testCase->name, g_failuresInCurrentTest);
            ++failedCount;
        }
    }

    std::printf("%d test(s) ran, %d failed\n", testCount, failedCount);
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
