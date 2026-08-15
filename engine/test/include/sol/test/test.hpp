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

// Variadic so brace-initializer commas in the expression survive preprocessing.
#define SOL_CHECK(...)                                                                                       \
    do {                                                                                                     \
        if (!(__VA_ARGS__)) {                                                                                \
            sol::test::reportFailure(#__VA_ARGS__, __FILE__, __LINE__);                                      \
        }                                                                                                    \
    } while (0)

// As SOL_CHECK, but bails out of the test on failure — for guards whose
// failure would make the following checks unsafe (e.g. out-of-range indexing).
#define SOL_REQUIRE(...)                                                                                     \
    do {                                                                                                     \
        if (!(__VA_ARGS__)) {                                                                                \
            sol::test::reportFailure(#__VA_ARGS__, __FILE__, __LINE__);                                      \
            return;                                                                                          \
        }                                                                                                    \
    } while (0)
