#pragma once

namespace sol::test {

using TestFn = void (*)();

struct TestCase
{
    const char* name = nullptr;
    TestFn fn = nullptr;
    TestCase* next = nullptr;
};

void registerTest(TestCase& testCase);
void reportFailure(const char* expression, const char* file, int line);

// Runs every registered test; returns a process exit code (0 = all passed).
[[nodiscard]] int runAllTests();

struct Registrar
{
    explicit Registrar(TestCase& testCase) { registerTest(testCase); }
};

} // namespace sol::test

#define SOL_TEST(name)                                                                                       \
    static void solTestFn_##name();                                                                         \
    static sol::test::TestCase solTestCase_##name{#name, &solTestFn_##name, nullptr};                       \
    static sol::test::Registrar solTestRegistrar_##name{solTestCase_##name};                                \
    static void solTestFn_##name()

#define SOL_CHECK(expression)                                                                                \
    do {                                                                                                     \
        if (!(expression)) {                                                                                 \
            sol::test::reportFailure(#expression, __FILE__, __LINE__);                                       \
        }                                                                                                    \
    } while (0)
