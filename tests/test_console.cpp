#include <forge/console.hpp>
#include <forge/testing.hpp>

#include <algorithm>

using Args = forge::Console::Args;

TEST_CASE("console: split") {
    CHECK(forge::Console::split("") == Args{});
    CHECK(forge::Console::split("  set  a   b ") == Args{"set", "a", "b"});
    CHECK(forge::Console::split(R"(say "hello world" 'x y' z\ w)") == Args{"say", "hello world", "x y", "z w"});
    CHECK(forge::Console::split(R"(q "a\"b" "")") == Args{"q", "a\"b", ""});
}

TEST_CASE("console: commands and executor") {
    forge::Console::Options o;
    o.redirect_logs = false;
    o.color = false;
    forge::Console console(o);

    Args seen;
    console.add("greet", "greet <name>  say hello", [&](const Args& a) { seen = a; });
    console.add("fail", "fail  always throws", [](const Args&) { throw std::runtime_error("nope"); });

    CHECK(console.execute("greet bob alice"));
    CHECK(seen == Args{"bob", "alice"});
    CHECK_FALSE(console.execute("unknown"));
    CHECK(console.execute("fail"));  // exception is reported, not propagated
    CHECK(console.execute("   "));

    int via_executor = 0;
    console.set_executor([&](std::function<void()> fn) {
        ++via_executor;
        fn();
    });
    CHECK(console.execute("greet x"));
    CHECK(console.execute("help"));  // builtins bypass the executor
    CHECK(via_executor == 1);

    const auto cmds = console.commands();
    CHECK(std::find(cmds.begin(), cmds.end(), "greet") != cmds.end());
    CHECK(std::find(cmds.begin(), cmds.end(), "help") != cmds.end());
    CHECK(console.remove("greet"));
    CHECK_FALSE(console.execute("greet"));
}
