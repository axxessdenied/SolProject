#include <string>

#include <sol/scripting/vm.hpp>
#include <sol/test/test.hpp>

using sol::scripting::EntityHandle;
using sol::scripting::ScriptVm;
using sol::scripting::toEntity;
using sol::scripting::toHandle;

namespace {

int add(int a, int b)
{
    return a + b;
}

double scale(double value)
{
    return value * 2.0;
}

std::string greet(const std::string& name)
{
    return "hello " + name;
}

struct Counter
{
    int total = 0;
};

void bump(Counter& counter, int amount)
{
    counter.total += amount;
}

EntityHandle makeEntity()
{
    return toHandle({7, 3});
}

int entityIndex(EntityHandle handle)
{
    return static_cast<int>(toEntity(handle).index);
}

void appendLine(const char* line, void* userData)
{
    auto* captured = static_cast<std::string*>(userData);
    captured->append(line);
    captured->push_back('\n');
}

} // namespace

SOL_TEST(scripting_do_string_and_globals)
{
    ScriptVm vm;
    std::string error;
    SOL_CHECK(vm.doString("answer = 6 * 7", "test", &error));

    int answer = 0;
    SOL_CHECK(vm.getGlobal("answer", answer));
    SOL_CHECK(answer == 42);

    vm.setGlobal("name", std::string("aster"));
    std::string name;
    SOL_CHECK(vm.getGlobal("name", name));
    SOL_CHECK(name == "aster");
}

SOL_TEST(scripting_syntax_error_names_chunk)
{
    ScriptVm vm;
    std::string error;
    SOL_CHECK(!vm.doString("this is not lua", "badchunk", &error));
    SOL_CHECK(error.find("badchunk") != std::string::npos);
}

SOL_TEST(scripting_runtime_error_has_traceback)
{
    ScriptVm vm;
    std::string error;
    SOL_CHECK(!vm.doString("local function inner() error('boom') end inner()", "test", &error));
    SOL_CHECK(error.find("boom") != std::string::npos);
    SOL_CHECK(error.find("stack traceback") != std::string::npos);
}

SOL_TEST(scripting_registered_functions)
{
    ScriptVm vm;
    vm.registerFunction<&add>("sol", "add");
    vm.registerFunction<&scale>("sol", "scale");
    vm.registerFunction<&greet>("sol", "greet");

    std::string error;
    SOL_CHECK(
        vm.doString("sum = sol.add(2, 3) big = sol.scale(1.5) msg = sol.greet('pilot')", "test", &error));

    int sum = 0;
    double big = 0.0;
    std::string msg;
    SOL_CHECK(vm.getGlobal("sum", sum));
    SOL_CHECK(sum == 5);
    SOL_CHECK(vm.getGlobal("big", big));
    SOL_CHECK(big == 3.0);
    SOL_CHECK(vm.getGlobal("msg", msg));
    SOL_CHECK(msg == "hello pilot");
}

SOL_TEST(scripting_context_function)
{
    ScriptVm vm;
    Counter counter;
    vm.registerFunction<&bump>("sol", "bump", &counter);

    std::string error;
    SOL_CHECK(vm.doString("sol.bump(5) sol.bump(2)", "test", &error));
    SOL_CHECK(counter.total == 7);
}

SOL_TEST(scripting_argument_type_mismatch_is_an_error)
{
    ScriptVm vm;
    vm.registerFunction<&add>("sol", "add");

    std::string error;
    SOL_CHECK(!vm.doString("sol.add(1, 'x')", "test", &error));
    SOL_CHECK(error.find("argument 2") != std::string::npos);

    error.clear();
    SOL_CHECK(!vm.doString("sol.add(1)", "test", &error));
    SOL_CHECK(error.find("expected 2 argument(s)") != std::string::npos);

    // Non-integral number where an integer is expected is a type error.
    error.clear();
    SOL_CHECK(!vm.doString("sol.add(1.5, 2)", "test", &error));
    SOL_CHECK(error.find("argument 1") != std::string::npos);
}

SOL_TEST(scripting_typed_entity_handles)
{
    ScriptVm vm;
    vm.registerFunction<&makeEntity>("sol", "makeEntity");
    vm.registerFunction<&entityIndex>("sol", "entityIndex");

    std::string error;
    SOL_CHECK(vm.doString("e = sol.makeEntity() idx = sol.entityIndex(e)", "test", &error));
    int index = 0;
    SOL_CHECK(vm.getGlobal("idx", index));
    SOL_CHECK(index == 7);

    // Same handle value compares equal; a plain integer is rejected.
    SOL_CHECK(vm.doString("same = (sol.makeEntity() == e)", "test", &error));
    bool same = false;
    SOL_CHECK(vm.getGlobal("same", same));
    SOL_CHECK(same);

    error.clear();
    SOL_CHECK(!vm.doString("sol.entityIndex(12345)", "test", &error));
    SOL_CHECK(error.find("argument 1") != std::string::npos);

    EntityHandle handle;
    SOL_CHECK(vm.getGlobal("e", handle));
    SOL_CHECK(toEntity(handle).index == 7);
    SOL_CHECK(toEntity(handle).generation == 3);
}

SOL_TEST(scripting_sandbox_excludes_io_and_os)
{
    ScriptVm vm;
    std::string error;
    SOL_CHECK(vm.doString("ok = (io == nil) and (os == nil) and (debug == nil)", "test", &error));
    bool ok = false;
    SOL_CHECK(vm.getGlobal("ok", ok));
    SOL_CHECK(ok);
}

SOL_TEST(scripting_print_routes_to_sink)
{
    ScriptVm vm;
    std::string captured;
    vm.setOutputSink(&appendLine, &captured);

    std::string error;
    SOL_CHECK(vm.doString("print('hi', 42)", "test", &error));
    SOL_CHECK(captured == "hi\t42\n");
}

SOL_TEST(scripting_call_global)
{
    ScriptVm vm;
    std::string error;
    SOL_CHECK(vm.doString("function accumulate(a, b) total = a + b end", "test", &error));
    SOL_CHECK(vm.callGlobal("accumulate", &error, 2, 3.5));

    double total = 0.0;
    SOL_CHECK(vm.getGlobal("total", total));
    SOL_CHECK(total == 5.5);

    SOL_CHECK(!vm.callGlobal("missing", &error));
    SOL_CHECK(error.find("missing") != std::string::npos);
}

SOL_TEST(scripting_tracks_memory)
{
    ScriptVm vm;
    SOL_CHECK(vm.memoryUsed() > 0);
    const std::size_t before = vm.memoryUsed();
    std::string error;
    SOL_CHECK(vm.doString("t = {} for i = 1, 1000 do t[i] = i end", "test", &error));
    SOL_CHECK(vm.memoryUsed() > before);
}
