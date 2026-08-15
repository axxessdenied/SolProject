#include "sol/test/test.hpp"

#include <cstdio>

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
    return sol::test::runAllTests();
}
